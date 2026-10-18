/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ENDPOINT_H
#define JEIOS_ENDPOINT_H
/*
 * M3c sub-PR 2 (ADR 0009 D-SEQ step 2): the endpoint/channel LIFECYCLE core —
 * the first DESTROYABLE, refcounted kernel object. Standalone and hostable like
 * handle.c: it includes ONLY <jeios/handle.h> plus the standard headers (no
 * vm.h, sched.h, or abi.h), allocates nothing itself (the caller supplies the
 * pool), and has zero global state — so `make check` unit-tests destruction,
 * the split refcount, and REFOVERFLOW with no kernel state.
 *
 * SCOPE OF THIS PR (frozen by ADR 0009):
 *   - the channel pool, the channel record (both ends in one record), the SPLIT
 *     refcount (handle_refs vs inflight_refs), retain/release, destruction on
 *     last reference, and the endpoint-aware alloc/dup/close SEAM over the pure
 *     handle table.
 *   - CREATE IS KERNEL-ONLY: je_endpoint_channel_create() is a direct in-kernel
 *     entry used by the hosted test (and, later, the kernel self-test). There is
 *     NO ring-3 endpoint_create syscall in this PR, by design — it ships WITH the
 *     owner-signed process-death release-walk PR (ADR 0009 D-OWNER-2), so a user
 *     process can never die holding an endpoint before teardown can reclaim it.
 *   - NOT here: messages (data IPC), transfer, waits. inflight_refs exists and is
 *     tested directly, but nothing in this PR queues a cap or registers a waiter.
 *
 * TABLE PURITY (frozen virtue, ADR 0009 D2/D3). The handle table stays fully
 * object-agnostic: it only ever reads/writes the opaque `object` pointer and
 * NEVER dereferences it. All retain/release lives HERE. That makes one invariant
 * load-bearing:
 *
 *   >> PRECONDITION (discharged by the create-syscall PR, NOT this one):
 *   >> an ENDPOINT-typed handle slot is created and closed ONLY through this
 *   >> layer (je_endpoint_handle_* / je_endpoint_channel_*). A raw
 *   >> je_handle_alloc() of an ENDPOINT-typed slot (which takes NO ref), a raw
 *   >> je_handle_close() of one (which drops the slot WITHOUT dropping the ref),
 *   >> or a je_handle_table_init()-zeroing of one, all desynchronize handle_refs
 *   >> from the real slot count. A raw CLOSE under-drops (leaks: fail-safe); a
 *   >> raw ALLOC over-counts so a later endpoint-close drops a ref the slot never
 *   >> took, which CAN free a still-named record early (a use-after-free — NOT
 *   >> fail-safe). THIS PR keeps both impossible by minting endpoints ONLY into
 *   >> kernel-local tables, never into a process's own vm->procs[i].htable. The
 *   >> create-syscall PR MUST add a TYPED alloc+close dispatch (endpoint slots
 *   >> are only ever created/closed through this layer) before any endpoint can
 *   >> live in a process table. "Callers remember to use the wrapper" is NOT an
 *   >> acceptable invariant and is why create stays kernel-only until then.
 */
#include <jeios/handle.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Configured policy bounds (ADR 0009 D4/D5). Small ON PURPOSE so overflow
 * and exhaustion are reachable in hosted tests; these are POLICY, never hardware
 * limits. Pinned by _Static_assert in endpoint.c. -------------------------- */
#define JE_EP_POOL_CAP      4u    /* channel records in a pool */
#define JE_EP_QUEUE_DEPTH   4u    /* messages per end (queue unused this PR) */
#define JE_EP_MAX_WAITERS   2u    /* waiters per end (unused this PR) */
/* Each ref counter must admit every non-handle owning ref the design can force
 * (queued caps + waiters) without spuriously overflowing a legitimate acquire,
 * AND be small enough to hit in a test. handle_refs is bounded by table slots;
 * inflight_refs by queue depth + waiters. One shared max covers both. */
#define JE_EP_REF_MAX       8u

/* ENDPOINT object type tag carried in a handle slot. Type 0 is reserved; SELF is
 * 1 (handle.h). This is the table-side copy (endpoint.c has no ABI include); the
 * syscall glue _Static_asserts it equals the ABI's endpoint object type when the
 * ring-3 ops land. MUST differ from JE_HANDLE_OBJ_SELF. */
#define JE_ENDPOINT_OBJ_TYPE 2u
_Static_assert(JE_ENDPOINT_OBJ_TYPE != JE_HANDLE_OBJ_SELF,
               "endpoint type must differ from SELF");

/* Endpoint rights — table-side copies of the ABI bits (abi.h JE_ABI_RIGHT_*),
 * kept here so endpoint.c has no ABI dependency. The glue pins these equal to
 * the ABI constants. INSPECT is shared with SELF (0x1). */
#define JE_EP_RIGHT_INSPECT  0x1u
#define JE_EP_RIGHT_SEND     0x2u
#define JE_EP_RIGHT_RECV     0x4u
#define JE_EP_RIGHT_TRANSFER 0x8u
#define JE_EP_RIGHTS_ALL (JE_EP_RIGHT_INSPECT | JE_EP_RIGHT_SEND \
                          | JE_EP_RIGHT_RECV | JE_EP_RIGHT_TRANSFER)

/* Status. Distinct enum (no ABI coupling); the glue maps these to je_abi_status
 * names (JE_ABI_H_* + JE_ABI_H_REFOVERFLOW) at the syscall boundary in a later
 * PR, exactly as handle.c's je_handle_status does. */
typedef enum {
    JE_EP_OK = 0,
    JE_EP_BADINDEX,     /* handle/arg malformed, or not an endpoint handle */
    JE_EP_STALE,        /* handle no longer live */
    JE_EP_WRONGTYPE,    /* live slot, not an endpoint */
    JE_EP_DENIED,       /* rights subset check failed (dup) */
    JE_EP_QUOTA,        /* table live-slot quota reached */
    JE_EP_NOSLOTS,      /* no FREE table slot, or pool exhausted */
    JE_EP_REFOVERFLOW,  /* acquiring past JE_EP_REF_MAX; nothing acquired */
    JE_EP_CORRUPT       /* an object pointer that is not a valid pool end
                         * (should be impossible; fail closed, never deref) */
} je_ep_status;

/* An end's lifecycle state (ADR 0009 D2). */
typedef enum {
    JE_EP_END_FREE = 0,   /* the record is not allocated */
    JE_EP_END_OPEN,       /* live; peer is live */
    JE_EP_END_PEER_CLOSED,/* live; peer has gone (reads drain then EOF) */
    JE_EP_END_CLOSED      /* this end has no handle refs left */
} je_ep_end_state;

/* One end of a channel. `queue`/waiter storage is reserved for later PRs; this
 * PR only touches the two counts and the state. */
typedef struct {
    je_ep_end_state state;
    uint16_t        handle_refs;    /* LIVE handle slots naming this end */
    uint16_t        inflight_refs;  /* queued caps + waiters (unused this PR) */
    uint8_t         side;           /* 0 or 1; peer is record->end[side ^ 1] */
} je_ep_end;

/* A channel record: both ends in one allocation, so a peer pointer is always to
 * the same record and can never dangle (ADR 0009 D2). `generation` is INTERNAL
 * double-free / stale-pool-index defence, bumped on free, never exposed to ring
 * 3 (handles bind &end[i], not a pool index), per ADR 0009 D-OWNER-1. NOTE: this
 * lifecycle PR bumps it but does not yet READ it — the borrowed-pointer rule
 * (ADR 0009 D3) keeps stale (rec,side) pairs out of the only refcount APIs that
 * take a raw record, so there is nothing to detour yet. The queue/wait PR that
 * first holds such a pair across a possible free MUST carry+check this
 * generation; until then it is reserved structure, honestly unused. */
typedef struct {
    bool     allocated;      /* record is in use */
    uint32_t generation;     /* bumped on free; internal only */
    je_ep_end end[2];
} je_ep_channel;

/* The pool. Caller-supplied storage (hostable): the kernel owns one static
 * instance; each hosted test owns a local one. No global state in endpoint.c. */
typedef struct {
    je_ep_channel rec[JE_EP_POOL_CAP];
    uint32_t      allocated_count;  /* live records (accounting) */
} je_endpoint_pool;

/* Initialize a pool: every record FREE, generation 1 (never 0), counts zero.
 * MUST be called once per pool lifetime. */
void je_endpoint_pool_init(je_endpoint_pool *pool);

/* ---- Create (KERNEL-ONLY; no ring-3 route exists in this PR). ------------- */
/* Transactionally create a channel and mint one handle to EACH end into the two
 * supplied tables (t0 for end 0, t1 for end 1 — may be the same table). Each
 * minted handle takes a handle_ref. On ANY failure the whole operation unwinds:
 * no record is left allocated and no handle is minted (both *out are 0). A record
 * with handle_refs == 0 is never observable. Rights for each end come from the
 * caller (typically JE_EP_RIGHTS_ALL for the creator). `out0`/`out1` REQUIRED.
 * Status precedence on the second mint failing: the first mint + the record are
 * rolled back and the second mint's status (QUOTA/NOSLOTS) is returned. */
je_ep_status je_endpoint_channel_create(je_endpoint_pool *pool,
                                        je_handle_table *t0, uint32_t rights0,
                                        je_handle_table *t1, uint32_t rights1,
                                        je_handle *out0, je_handle *out1);

/* ---- The endpoint-aware handle seam over the pure table. ------------------ */
/* Close ONE endpoint handle: resolve it as an endpoint, invalidate the table
 * slot (je_handle_close), then release exactly one handle_ref on its end. When an
 * end's handle_refs reaches 0 it goes CLOSED, its live peer goes PEER_CLOSED, and
 * (when BOTH ends have handle_refs == 0 AND inflight_refs == 0) the record is
 * freed and its generation bumped. Closing a stale/non-endpoint handle returns
 * the specific status and releases nothing. Release of a valid live ref is
 * infallible by construction (an impossible failure is an invariant violation,
 * not a rollback case). */
je_ep_status je_endpoint_handle_close(je_endpoint_pool *pool,
                                      je_handle_table *t, je_handle h);

/* Duplicate an endpoint handle with an equal-or-weaker rights subset, into the
 * same table, as a FRESH slot (its own index/generation/close) naming the SAME
 * end — a genuine second handle_ref, NOT a relabel. This is the endpoint
 * equivalent of je_handle_dup_self, but WITH retain-before-commit: it is the
 * reason dup_self stays SELF-only (handle.h) and endpoints never use it.
 *
 * Order (ADR 0009 D8 acquire-before-commit): resolve src as an endpoint
 * (BADINDEX -> STALE -> WRONGTYPE), subset-check new_rights (DENIED on any bit
 * src lacks; equality allowed), RETAIN a handle_ref (REFOVERFLOW if at
 * JE_EP_REF_MAX — acquires nothing), then je_handle_alloc the new slot
 * (QUOTA before NOSLOTS). On ANY alloc failure the retained ref is RELEASED again
 * and *out is 0. The rollback can never trigger destruction because `src` still
 * holds its own handle_ref throughout. `new_rights` must contain only known
 * endpoint bits (caller/glue rejects unknown bits as INVALID upstream, as for
 * dup_self). `out` REQUIRED. */
je_ep_status je_endpoint_handle_dup(je_endpoint_pool *pool, je_handle_table *t,
                                    je_handle src, uint32_t new_rights,
                                    je_handle *out);

/* ---- Direct (non-handle) inflight ref API — for the queue/wait PRs, tested
 * directly here. retain_inflight fails REFOVERFLOW past JE_EP_REF_MAX (acquires
 * nothing); release_inflight on the last ref may free the record under the same
 * both-ends-zero rule as close. These take a resolved end obtained from a handle
 * via je_endpoint_resolve (never a raw user pointer). ----------------------- */
je_ep_status je_endpoint_resolve(const je_endpoint_pool *pool,
                                 const je_handle_table *t, je_handle h,
                                 je_ep_channel **out_rec, uint8_t *out_side);
je_ep_status je_endpoint_retain_inflight(je_endpoint_pool *pool,
                                         je_ep_channel *rec, uint8_t side);
je_ep_status je_endpoint_release_inflight(je_endpoint_pool *pool,
                                          je_ep_channel *rec, uint8_t side);

/* Read-only accessors for tests/assertions (never mutate). */
je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side);
uint32_t        je_endpoint_pool_allocated(const je_endpoint_pool *pool);

#endif /* JEIOS_ENDPOINT_H */
