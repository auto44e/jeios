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
#define JE_EP_QUEUE_DEPTH   4u    /* messages per end inbound queue */
#define JE_EP_MAX_WAITERS   2u    /* waiters per end (unused this PR) */
/* M3c step 5: max payload BYTES per message (a policy bound, newly chosen here —
 * ADR 0009 said the message max is pinned in an M3c PR; the lifecycle PR did not,
 * so this is it). <= 255 so a per-slot length fits a uint8_t. */
#define JE_EP_MSG_MAX       64u
/* M3c step 5 (ADR 0009 A5): max CAPABILITIES a single message may carry. A policy
 * bound (K); 2 so the recv/send rollback is exercised (a 2nd mint can hit QUOTA).
 * DEPTH*K <= JE_EP_REF_MAX so a full queue of cap-carrying messages cannot
 * overflow an end's inflight count. */
#define JE_EP_MSG_CAPS_MAX  2u
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
    JE_EP_QUEUE_FULL,   /* try_send: peer inbound at JE_EP_QUEUE_DEPTH */
    JE_EP_SHORT_BUFFER, /* try_recv: caller buffer < head message; not dequeued */
    JE_EP_PEER_CLOSED,  /* try_send to a peer-closed end, or try_recv drained+EOF */
    JE_EP_EMPTY,        /* try_recv: OPEN end, empty queue (would block) */
    JE_EP_INVALID,      /* malformed pure-layer arg (e.g. len > JE_EP_MSG_MAX) */
    JE_EP_WAITERS_BUSY, /* M3c A6-4: the end's single waiter slot is occupied; the
                         * glue maps this to a non-blocking EMPTY (never parks,
                         * never a new ABI status). */
    JE_EP_CORRUPT       /* an object pointer that is not a valid pool end
                         * (should be impossible; fail closed, never deref) */
} je_ep_status;

/* A queued capability descriptor (ADR 0009 A5): a direct reference to a
 * transferred endpoint END (by pool index + side + generation) plus the
 * attenuated rights the receiver will mint. NOT a handle (handles are per-table).
 * Each queued descriptor holds ONE inflight ref on rec[rec_index].end[side].
 * `generation` is a defence-in-depth tripwire; it matches while the inflight ref
 * keeps the record allocated, and distinguishes a since-reused record. */
typedef struct {
    uint16_t rec_index;    /* index into the owning pool's rec[] */
    uint8_t  side;         /* which end of that record (0/1) */
    uint8_t  cap_count_pad;/* reserved/alignment */
    uint32_t generation;   /* rec->generation when queued */
    uint32_t rights;       /* attenuated rights to mint (subset of source) */
} je_ep_capdesc;

/* One inbound message queue (a ring buffer of fixed-max-size messages). Each slot
 * carries payload bytes AND 0..JE_EP_MSG_CAPS_MAX capability descriptors (A5). */
typedef struct {
    uint8_t       buf[JE_EP_QUEUE_DEPTH][JE_EP_MSG_MAX];
    uint8_t       len[JE_EP_QUEUE_DEPTH];       /* payload bytes per slot */
    je_ep_capdesc cap[JE_EP_QUEUE_DEPTH][JE_EP_MSG_CAPS_MAX];
    uint8_t       cap_count[JE_EP_QUEUE_DEPTH];  /* caps per slot (0..CAPS_MAX) */
    uint8_t       head;                          /* index of the oldest message */
    uint8_t       count;                         /* number queued (0..DEPTH) */
} je_ep_queue;

/* An end's lifecycle state (ADR 0009 D2). */
/* M3c D7/A6: the terminal cause of a blocking recv wait. WAITING is the initial
 * (non-terminal) state; the other four are terminal and set by exactly one CAS
 * (je_ipc_wait_arbitrate). Priority among causes eligible in ONE entry snapshot
 * is A6-1 drain-first (a queued message => WOKEN even if peer closed); once
 * parked, first-critical-section-wins and the later causes are no-ops. */
typedef enum {
    JE_EP_WAIT_NONE = 0,   /* no waiter registered on this end */
    JE_EP_WAIT_WAITING,    /* parked, no terminal cause yet */
    JE_EP_WAIT_WOKEN,      /* a message became deliverable (head reserved) */
    JE_EP_WAIT_TIMED_OUT,  /* deadline elapsed (timer ISR) */
    JE_EP_WAIT_PEER_CLOSED,/* peer end closed with this end's inbound empty */
    JE_EP_WAIT_CANCELLED   /* internal cancel (A6-2; no ABI trigger in v1) */
} je_ep_wait_cause;

/* Per-end waiter (A6-4: at most ONE per end). `present` gates the slot; `waiter_id`
 * is an OPAQUE token the scheduler assigns (the thread slot) so the pure endpoint
 * layer can name whom to wake WITHOUT depending on sched.c. `cause` is the single
 * terminal CAS target. `head_reserved` marks that the current inbound head is
 * claimed by this waiter (WOKEN): it is NOT dequeued, just not deliverable to any
 * other receiver until consume (commit) or release (A6-5/A6-CLARIFY-2). */
typedef struct {
    bool             present;
    uint32_t         waiter_id;
    je_ep_wait_cause cause;
    bool             head_reserved;
} je_ep_waiter;

typedef enum {
    JE_EP_END_FREE = 0,   /* the record is not allocated */
    JE_EP_END_OPEN,       /* live; peer is live */
    JE_EP_END_PEER_CLOSED,/* live; peer has gone (reads drain then EOF) */
    JE_EP_END_CLOSED      /* this end has no handle refs left */
} je_ep_end_state;

/* One end of a channel. `inbound` holds messages sent by the PEER and waiting for
 * THIS end's try_recv (Model A). Waiter storage is reserved for step 6. */
typedef struct {
    je_ep_end_state state;
    uint16_t        handle_refs;    /* LIVE handle slots naming this end */
    uint16_t        inflight_refs;  /* queued caps + waiters */
    uint8_t         side;           /* 0 or 1; peer is record->end[side ^ 1] */
    je_ep_queue     inbound;        /* messages awaiting this end's try_recv */
    je_ep_waiter    waiter;         /* M3c A6: at most one blocking receiver (A6-4) */
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

/* ---- Non-blocking data IPC (M3c step 4, ADR 0009 D6, data-only per D9) ------
 * All pure/hostable: they move KERNEL bytes only (the glue does copy_from/to_user
 * around them, honoring D6's copy-then-enqueue / peek-copy-then-dequeue ordering).
 * A message is bytes only — no capabilities until step 5. */

/* try_send PREFLIGHT: the read-only half of send — resolve the handle (needs
 * SEND), check self-end state and peer-inbound capacity — WITHOUT touching user
 * memory or mutating anything. The glue calls this FIRST (so a bad user pointer
 * never masks a DENIED/PEER_CLOSED/QUEUE_FULL status, and user memory is never
 * read for an unauthorized handle), THEN copy_from_user into staging, THEN
 * je_endpoint_try_send to enqueue. Returns the same status precedence try_send
 * would (BADINDEX/STALE/WRONGTYPE -> DENIED -> PEER_CLOSED -> QUEUE_FULL -> OK).
 * `len` is validated (> JE_EP_MSG_MAX -> JE_EP_INVALID). */
je_ep_status je_endpoint_send_preflight(je_endpoint_pool *pool, je_handle_table *t,
                                        je_handle h, size_t len);

/* Send preflight WITHOUT the QUEUE_FULL check (main handle + SEND + self-end
 * PEER_CLOSED only). The cap-send glue runs this first, then resolves/validates
 * caps, so per-cap DENIED/STALE outrank QUEUE_FULL (A5-4 precedence); the final
 * try_send_caps enforces QUEUE_FULL atomically. */
je_ep_status je_endpoint_send_preflight_main(je_endpoint_pool *pool,
                                             je_handle_table *t, je_handle h,
                                             size_t len);

/* try_send: enqueue a COPY of kbuf[0..len) (kernel memory) into the PEER end's
 * inbound queue. len must be <= JE_EP_MSG_MAX (else JE_EP_INVALID). Resolves the
 * handle as an endpoint requiring the SEND right. Status precedence:
 * BADINDEX/STALE/WRONGTYPE -> DENIED -> PEER_CLOSED (self end peer-closed) ->
 * QUEUE_FULL -> OK. On any non-OK status NOTHING is enqueued. kbuf may be NULL
 * iff len == 0 (a valid empty message). */
je_ep_status je_endpoint_try_send(je_endpoint_pool *pool, je_handle_table *t,
                                  je_handle h, const void *kbuf, size_t len);

/* A recv lease token (D6 / endpoint.h generation-carry rule): identifies the
 * exact head message a peek observed, so commit dequeues only that message. */
typedef struct {
    je_ep_channel *rec;
    uint8_t        side;
    uint32_t       generation;  /* rec->generation at peek time */
    uint8_t        head;        /* queue head index at peek time */
    uint8_t        len;         /* head message length at peek time */
} je_ep_recv_lease;

/* try_recv PEEK: resolve the handle (needs RECV), apply the state/empty rules,
 * and — on OK — copy the head message into out_kbuf (kernel memory, up to cap
 * bytes) WITHOUT dequeuing, writing *out_len = message length and filling *lease.
 * If cap < message length: returns JE_EP_SHORT_BUFFER, writes *out_len = required
 * length, copies nothing, no lease. Empty+OPEN -> JE_EP_EMPTY; empty+peer-closed
 * -> JE_EP_PEER_CLOSED (EOF); nonempty -> deliver regardless of peer state (drain
 * first). Copies into a CALLER buffer (the glue's kernel stack), so the glue
 * never holds a pointer into pool storage across copy_to_user. */
je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
                                   je_handle h, void *out_kbuf, size_t cap,
                                   size_t *out_len, je_ep_recv_lease *lease);

/* try_recv COMMIT: dequeue the message the lease identifies. Called by the glue
 * ONLY after a successful copy_to_user. INFALLIBLE for a valid lease: if the
 * lease no longer matches (record freed/reused/mutated — impossible under IF=0
 * via ordinary API), it returns JE_EP_CORRUPT and the caller MUST treat that as
 * fatal (a retry would double-deliver). Zeroes the vacated slot. */
je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
                                     const je_ep_recv_lease *lease);

/* ---- Capability transfer (M3c step 5, ADR 0009 A5) ------------------------
 * These extend send/recv to carry capabilities. The data-only forms above are
 * the caps==0 case and remain valid. The glue resolves user handles to
 * descriptors (send) and mints handles from descriptors (recv); the pure layer
 * manages the queued-cap inflight refs and the queue storage. */

/* send PREFLIGHT with caps: as je_endpoint_send_preflight, plus it validates that
 * ncaps <= JE_EP_MSG_CAPS_MAX and each descriptor's target is a live endpoint in
 * THIS pool with only known rights bits. Does NOT retain or mutate. (The GLUE
 * attenuates each cap against the SENDER's handle rights — requiring TRANSFER and
 * a subset — when it builds the descriptors; ends carry no rights, so the pure
 * layer only checks liveness + known-bits.) Returns the first failing status. */
je_ep_status je_endpoint_send_preflight_caps(je_endpoint_pool *pool,
                                             je_handle_table *t, je_handle h,
                                             size_t len,
                                             const je_ep_capdesc *caps,
                                             uint8_t ncaps);

/* try_send with caps: preflight (re-check), RETAIN one inflight ref per cap on
 * each target (REFOVERFLOW rolls back all retained-so-far), then enqueue the
 * message carrying the byte payload AND the cap descriptors into the peer inbound.
 * All-or-nothing: on any failure nothing is enqueued and no inflight ref is left
 * retained. `caps`/`ncaps` may be 0 (the data-only case). */
je_ep_status je_endpoint_try_send_caps(je_endpoint_pool *pool, je_handle_table *t,
                                       je_handle h, const void *kbuf, size_t len,
                                       const je_ep_capdesc *caps, uint8_t ncaps);

/* recv PEEK with caps: as je_endpoint_recv_peek, plus it reports the head
 * message's cap_count via *out_ncaps and copies the cap descriptors into
 * out_caps[0..CAPS_MAX) (kernel memory). If the caller's handle-slot capacity
 * (hcap) < cap_count, returns JE_EP_SHORT_BUFFER with *out_ncaps = required
 * (alongside the byte-length rule). No dequeue, no mint. */
je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
                                        je_handle h, void *out_kbuf, size_t cap,
                                        size_t *out_len, uint8_t hcap,
                                        je_ep_capdesc *out_caps, uint8_t *out_ncaps,
                                        je_ep_recv_lease *lease);

/* Classify a peeked cap descriptor: is its target still LIVE (mintable) or dead
 * (CLOSED / generation-stale -> deliver handle 0, A5-2)? Pure, read-only. */
bool je_endpoint_capdesc_live(const je_endpoint_pool *pool,
                              const je_ep_capdesc *d);

/* Resolve a LIVE cap descriptor to its (rec, side) for the glue's mint. Returns
 * JE_EP_OK + the end, or JE_EP_STALE/CORRUPT for a dead/invalid descriptor. */
je_ep_status je_endpoint_capdesc_resolve(je_endpoint_pool *pool,
                                         const je_ep_capdesc *d,
                                         je_ep_channel **out_rec, uint8_t *out_side);

/* Retain one handle_ref on a cap descriptor's target (the glue's destination mint
 * uses this before alloc'ing the receiver handle; REFOVERFLOW if at the bound). */
je_ep_status je_endpoint_capdesc_retain_handle(je_endpoint_pool *pool,
                                               const je_ep_capdesc *d);
/* Undo a capdesc_retain_handle (mint rollback). Infallible for a valid retain. */
void je_endpoint_capdesc_release_handle(je_endpoint_pool *pool,
                                        const je_ep_capdesc *d);

/* recv COMMIT with caps: dequeue the leased head message AND release the inflight
 * ref each of its queued caps held. Called by the glue ONLY after the byte + the
 * handle outputs have been copied out (or the dead-cap zeros staged). Infallible
 * for a valid lease (mismatch -> CORRUPT, fatal). */
je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
                                          const je_ep_recv_lease *lease);

/* Read-only accessors for tests/assertions (never mutate). */
je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side);
uint32_t        je_endpoint_pool_allocated(const je_endpoint_pool *pool);
/* Inbound queue depth for a resolved end (tests). */
uint8_t         je_endpoint_inbound_count(const je_ep_channel *rec, uint8_t side);

/* Process-death RELEASE WALK (ADR 0009 D-OWNER-2 + AMENDMENT A3-1): drop every
 * LIVE endpoint handle in table `t`, closing peers and freeing now-unreferenced
 * channels, so a dying process leaks no pool record. Enumerates `t` via the
 * read-only je_handle_live_at iterator and closes each endpoint slot through
 * je_endpoint_handle_close (never writes `t->slots` directly, never touches any
 * other table — a surviving peer only sees its end go PEER_CLOSED). Idempotent
 * by construction: each slot owns exactly one handle_ref and is invalidated
 * before its ref drops. Returns the number of endpoint handles released. A
 * resolve/close failure on a slot the iterator just reported LIVE is an
 * invariant violation: the caller MUST treat a negative return as fatal (the
 * record pool is corrupt) and stop before any table-zeroing. On success every
 * endpoint slot in `t` is guaranteed no longer LIVE. MUST run with IF=0 (the
 * caller — je_vm_destroy — holds the IRQ-disabled critical section, A3-1). */
int je_endpoint_release_table(je_endpoint_pool *pool, je_handle_table *t);

/* ---- M3c D7/A6: blocking-recv waiter primitives (PURE; sched.c owns the thread
 * side, endpoint_kern.c bridges the wake into the scheduler). ---- */

/* Outcome of the single terminal CAS (je_ipc_wait_arbitrate). WON means THIS call
 * performed the WAITING->cause transition (the caller now owns the follow-up: a
 * WOKEN winner reserved the head; the bridge must wake waiter_id). LOST means the
 * waiter was absent or already terminal (no-op). */
typedef enum { JE_EP_ARB_LOST = 0, JE_EP_ARB_WON = 1 } je_ep_arb_result;

/* ENTRY SNAPSHOT (A6-ABI): evaluate an OPEN end once, WITHOUT registering a
 * waiter, exactly as non-blocking try_recv's state/empty rules — used by both the
 * zero-timeout poll and the first check of a blocking recv. Returns OK
 * (deliverable: caller proceeds to the normal peek/mint/copy), PEER_CLOSED, or
 * EMPTY (would block, OR the head is reserved by an existing waiter). It takes no
 * buffer, so it never returns SHORT_BUFFER — the real peek that follows an OK does
 * the size check. Never parks, never reserves. */
je_ep_status je_endpoint_recv_entry_probe(je_endpoint_pool *pool, je_handle_table *t,
                                          je_handle h);

/* REGISTER a waiter on the end named by handle `h` (needs RECV). Fails
 * JE_EP_QUEUE_FULL-equivalent (returns JE_EP_WAITERS_BUSY) if the end already has
 * a present waiter (A6-4 per-end bound; caller then returns EMPTY, never parks).
 * On OK the end holds {present, waiter_id, cause=WAITING, head_reserved=false}.
 * Does NOT block the thread — sched.c does that after this returns OK. */
je_ep_status je_endpoint_wait_register(je_endpoint_pool *pool, je_handle_table *t,
                                       je_handle h, uint32_t waiter_id,
                                       je_ep_channel **out_rec, uint8_t *out_side);

/* The single terminal CAS. On the end (rec,side): if a waiter is present and in
 * WAITING, set `cause` (WOKEN additionally reserves the inbound head) and return
 * WON + *out_waiter_id; otherwise return LOST. MUST run under IF=0. This is the
 * ONLY path that leaves WAITING. Callers: enqueue (WOKEN), peer-close
 * (PEER_CLOSED), timer (TIMED_OUT), internal cancel (CANCELLED). A6-1 drain-first
 * is enforced by the callers' eligibility (enqueue only arbitrates WOKEN when it
 * actually queued a message; close only arbitrates PEER_CLOSED on an empty end). */
je_ep_arb_result je_endpoint_wait_arbitrate(je_ep_channel *rec, uint8_t side,
                                            je_ep_wait_cause cause,
                                            uint32_t *out_waiter_id);

/* Read the terminal cause a waiter was stamped with (the continuation prologue
 * maps it to the ABI status). Returns JE_EP_WAIT_NONE if no waiter present. */
je_ep_wait_cause je_endpoint_wait_cause(const je_ep_channel *rec, uint8_t side);

/* True iff end (rec,side) has a present WAITING waiter; reports its waiter_id.
 * Used to check ownership BEFORE a cancel arbitrate. */
bool je_endpoint_wait_id(const je_ep_channel *rec, uint8_t side, uint32_t *out_id);

/* RELEASE a waiter (A6-CLARIFY-2 single idempotent owner): if end (rec,side)'s
 * present waiter has waiter_id == `who`, clear it; if it held a reserved head,
 * un-reserve (the head stays enqueued, deliverable again at the queue front), and
 * drop the waiter's inflight ref (freeing the record if it was the last). A
 * mismatch / no-present-waiter is a safe no-op (idempotent). Returns true iff it
 * cleared THIS waiter. MUST run under IF=0. Called by: a successful recv commit
 * (consume), a copy-fault rollback, and thread teardown (mark_dying/reaper). */
bool je_endpoint_wait_release(je_endpoint_pool *pool, je_ep_channel *rec, uint8_t side,
                             uint32_t who);

/* Scan-free helper for teardown: find the end (if any) in the WHOLE pool whose
 * present waiter has waiter_id == `who`, and release it (as above). Returns true
 * iff one was found+released. Used by mark_dying, which knows the thread slot
 * (waiter_id) but not which end it parked on. MUST run under IF=0. */
bool je_endpoint_wait_release_by_id(je_endpoint_pool *pool, uint32_t who);

/* A6-5 continuation read: peek the RESERVED head of an OWNED end (rec,side) for a
 * waiter resuming after WOKEN — bytes AND cap descriptors. Bypasses handle
 * resolution (owned-reference rule, A6-5) and the reserved-head guard. SHORT_BUFFER
 * leaves it queued+reserved; CORRUPT if the end has no reserved head. Commit via
 * je_endpoint_recv_commit_caps then je_endpoint_wait_release to free waiter+reservation. */
je_ep_status je_endpoint_recv_peek_owned_caps(je_endpoint_pool *pool, je_ep_channel *rec,
                                              uint8_t side, void *out_kbuf, size_t cap,
                                              size_t *out_len, uint8_t hcap,
                                              je_ep_capdesc *out_caps, uint8_t *out_ncaps,
                                              je_ep_recv_lease *lease);

#endif /* JEIOS_ENDPOINT_H */
