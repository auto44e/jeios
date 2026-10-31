/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ABI_H
#define JEIOS_ABI_H
/*
 * M3a #5: experimental native syscall ABI v0 (ADR 0007 D8/D10). This is the
 * ONLY status/op numbering exposed to ring 3. It is explicitly numbered and is
 * DELIBERATELY independent of the internal <jeios/status.h> enum — the kernel
 * must never pass internal je_status numbering across the user boundary (M3A.md
 * rule 5). This header includes <stdint.h> only; a make-check grep enforces that
 * it never includes status.h.
 *
 * Call convention (int 0x80 gate, DPL3, IST=0):
 *   RAX  = operation number in; je_abi_status out.
 *   RDI,RSI,RDX,R10,R8,R9 = up to six argument registers in.
 *   RDX  = optional single u64 scalar result out (0 when no result).
 * Unknown operations return JE_ABI_UNSUPPORTED regardless of arguments. A
 * recognized operation with a nonzero reserved argument returns JE_ABI_INVALID
 * and has no side effect. No stable ABI promise; v0 is experimental.
 */
#include <stdint.h>

/* Explicitly numbered status. NOT related to je_status in status.h. */
typedef enum {
    JE_ABI_OK          = 0,
    JE_ABI_UNSUPPORTED = 1,
    JE_ABI_INVALID     = 2,
    /* M3b handle statuses (ADR 0008 D2): appended, DISTINCT values exposed to
     * ring 3 (not collapsed to INVALID). These are the v0 ABI mapping of the
     * internal JE_HANDLE_* results; abi.h stays independent of handle.h (the
     * glue maps between them). Information leak is not a concern: a handle names
     * only the caller's own process-local table. */
    JE_ABI_H_STALE     = 3,  /* slot FREE/RETIRED or generation mismatch */
    JE_ABI_H_BADINDEX  = 4,  /* handle 0, or index >= table capacity */
    JE_ABI_H_WRONGTYPE = 5,  /* live slot, wrong object type (hosted-only in M3b) */
    JE_ABI_H_DENIED    = 6,  /* live slot, insufficient rights (ring-3-observable
                              * via handle_dup attenuation since the M3b dup PR) */
    JE_ABI_H_QUOTA     = 7,  /* allocate: live-slot count at the process quota */
    JE_ABI_H_NOSLOTS   = 8,  /* allocate: no FREE slot remains */
    JE_ABI_H_REFOVERFLOW = 9, /* M3c (ADR 0009 D4): acquiring an owning reference
                              * past the configured JE_EP_REF_MAX policy bound;
                              * acquires nothing, object unchanged. RESERVED here
                              * (no ring-3 endpoint op yet) — first surfaced by a
                              * later M3c syscall; pinned now so the number is
                              * frozen in the v0 ABI. */
    JE_ABI_H_QUEUE_FULL  = 10, /* M3c D6: try_send on an end whose peer inbound
                              * queue is at JE_EP_QUEUE_DEPTH; nothing enqueued. */
    JE_ABI_H_SHORT_BUFFER = 11, /* M3c D6: try_recv with a buffer smaller than the
                              * head message; RDX = required length, NOT dequeued. */
    JE_ABI_H_PEER_CLOSED = 12, /* M3c D6: try_send to a peer-closed end, or try_recv
                              * on a peer-closed end whose inbound is now drained
                              * (EOF). */
    JE_ABI_H_EMPTY       = 13, /* M3c D6: non-blocking try_recv on an OPEN end with
                              * an empty inbound queue (would block; blocking is
                              * step 6). Distinct from PEER_CLOSED (peer gone). */
    JE_ABI_H_TIMED_OUT   = 14, /* M3c D7/A6: a BLOCKING try_recv (nonzero R10
                              * timeout) whose deadline elapsed before any message,
                              * peer close, or cancel. The timer ISR performs this
                              * terminal transition. A zero timeout never yields
                              * this — it polls and returns EMPTY (A6-3/A6-ABI). */
    JE_ABI_H_CANCELLED   = 15  /* M3c D7/A6: a blocking wait ended by an internal
                              * cancellation. ABI-UNREACHABLE in v1 (no signal/kill
                              * op exists); pinned now so the arbitration CAS has a
                              * defined status for all four terminal causes and a
                              * future signal mechanism wires to a frozen number
                              * (A6-2). */
} je_abi_status;
_Static_assert(JE_ABI_OK == 0, "abi ok");
_Static_assert(JE_ABI_UNSUPPORTED == 1, "abi unsupported");
_Static_assert(JE_ABI_INVALID == 2, "abi invalid");
/* M3b handle statuses are ABI too — pin their numbering so a careless reorder
 * can't silently renumber a status ring 3 already depends on. */
_Static_assert(JE_ABI_H_STALE == 3, "abi h_stale");
_Static_assert(JE_ABI_H_BADINDEX == 4, "abi h_badindex");
_Static_assert(JE_ABI_H_WRONGTYPE == 5, "abi h_wrongtype");
_Static_assert(JE_ABI_H_DENIED == 6, "abi h_denied");
_Static_assert(JE_ABI_H_QUOTA == 7, "abi h_quota");
_Static_assert(JE_ABI_H_NOSLOTS == 8, "abi h_noslots");
_Static_assert(JE_ABI_H_REFOVERFLOW == 9, "abi h_refoverflow");
_Static_assert(JE_ABI_H_QUEUE_FULL == 10, "abi h_queue_full");
_Static_assert(JE_ABI_H_SHORT_BUFFER == 11, "abi h_short_buffer");
_Static_assert(JE_ABI_H_PEER_CLOSED == 12, "abi h_peer_closed");
_Static_assert(JE_ABI_H_EMPTY == 13, "abi h_empty");
_Static_assert(JE_ABI_H_TIMED_OUT == 14, "abi h_timed_out");
_Static_assert(JE_ABI_H_CANCELLED == 15, "abi h_cancelled");

/* Operation numbers (full 64-bit RAX; any value not listed is UNSUPPORTED). */
typedef enum {
    JE_SYS_ABI_QUERY = 0,  /* abi_query(which): RDI 0=version,1=profile -> RDX */
    JE_SYS_YIELD     = 1,  /* yield(): reschedule caller; no authority grant */
    JE_SYS_EXIT      = 2,  /* exit(code): RDI=code; terminate process, no return */
    JE_SYS_COPY_PROBE = 3, /* dev-only copy_probe(src=RDI, dst=RSI, len=RDX), <=256 */
    /* M3b handle ops (ADR 0008). (vm,index) is resolved ONLY via the current
     * user process; a non-user caller gets JE_ABI_INVALID. */
    JE_SYS_HANDLE_SELF  = 5, /* handle_self(): mint a handle to this process's
                              * SELF object -> handle in RDX, status in RAX */
    JE_SYS_HANDLE_CLOSE = 6, /* handle_close(h=RDI): close; status in RAX */
    JE_SYS_HANDLE_QUERY = 7, /* handle_query(h=RDI): -> RDX = object type (rights
                              * NOT exposed here; reserved out bits zero) */
    JE_SYS_HANDLE_RIGHTS = 8, /* handle_rights(h=RDI): -> RDX = rights mask of the
                              * handle, masked to JE_ABI_RIGHTS_KNOWN. Requires
                              * the INSPECT right (policy hook; see note below). */
    JE_SYS_HANDLE_DUP = 9    /* handle_dup(h=RDI, new_rights=RSI): derive a new
                              * handle to the same object with a rights SUBSET.
                              * -> RDX = new handle. Attenuation only (never a
                              * superset: a right the source lacks -> DENIED). */
    , JE_SYS_ENDPOINT_CREATE = 10 /* M3c (ADR 0009 A3-2) endpoint_create(): no
                              * args (all of RDI/RSI/RDX/R10/R8/R9 zero, else
                              * INVALID). Mints BOTH ends of a fresh channel into
                              * the caller's own table with all endpoint rights.
                              * Transactional. -> RAX status; RDX packs the two
                              * 32-bit handles: RDX[31:0]=h0, RDX[63:32]=h1. RDX
                              * is UNDEFINED on failure. UNSUPPORTED on a VM with
                              * no teardown hook (so an endpoint can only exist in
                              * a table whose death path reclaims it). */
    , JE_SYS_TRY_SEND = 11   /* M3c D6: try_send(h=RDI, ubuf=RSI, len=RDX,
                              * meta=R10, R8, R9). Non-blocking: copies len bytes
                              * from ubuf into the peer's inbound queue. len must
                              * be <= JE_EP_MSG_MAX. The handle needs the SEND right.
                              * R10 layout: R10[7:0] = cap_count (0..JE_ABI_MSG_CAPS_MAX;
                              * capability transfer has been live since M3c step 5, ADR 0009
                              * A5-4), R10[63:8] reserved (must be 0). R8 = user u32[]
                              * cap array (non-zero iff cap_count != 0). R9 reserved
                              * (must be 0). [Comment corrected in M4 PR-B; it previously
                              * said cap_count must be 0.]
                              * Status precedence (frozen): decode INVALID
                              * (meta/reserved/len/wide-handle, before any user
                              * access) > BADINDEX/STALE/WRONGTYPE > DENIED >
                              * PEER_CLOSED > QUEUE_FULL > copy-fault INVALID (the
                              * copy runs only after a clean preflight, so a bad
                              * ubuf ranks LAST, never masking a handle/queue
                              * status). RDX = 0 on success. */
    , JE_SYS_TRY_RECV = 12   /* M3c try_recv(h=RDI, ubuf=RSI, cap=RDX, hbuf=R8,
                              * hcap=R9, timeout=R10). Receives the head message
                              * (RECV right). D6 bytes + A5 capabilities + A6
                              * blocking:
                              *  - R8 = user u32[] to receive minted cap handles;
                              *    R9 = its slot capacity (0..JE_ABI_MSG_CAPS_MAX;
                              *    larger rejected). R8!=0 iff R9!=0 (A5-4).
                              *  - R10 = relative-tick timeout (A6): 0 = non-blocking
                              *    poll (today's behavior); nonzero = block up to
                              *    JE_IPC_MAX_WAIT (bound+overflow checked) until
                              *    WOKEN / PEER_CLOSED / TIMED_OUT(14) / CANCELLED(15).
                              *  -> RAX status; RDX[31:0] = bytes delivered (OK) or
                              *    required length (SHORT_BUFFER); RDX[63:32] = caps
                              *    delivered (OK) or required cap count
                              *    (SHORT_BUFFER); both 0 on other statuses. A
                              *    nonzero-timeout recv never returns EMPTY. */
    , JE_SYS_CLOCK_TICKS = 13 /* M4 clock_ticks(): RDI..R9 must be 0. RDX = monotonic
                              * count of 100 Hz PIT ticks since boot (je_sched_ticks). Not
                              * wall time; never decreases; no authority (ADR 0010 D6). */
    , JE_SYS_PROC_SPAWN = 14 /* M4 proc_spawn(launcher=RDI, image_id=RSI, grants=RDX,
                              * grant_count=R10, quotas=R8); R9 reserved (0). Needs SPAWN
                              * on a LAUNCHER handle. RDI and RSI must fit in 32 bits.
                              * Decode (pure) rejects with INVALID (EXHAUSTIVE list;
                              * launcher validity, image_id validity and the grants
                              * pointer are glue/spawn checks, not decode) [Codex
                              * PR-C test review P3]: wide RDI/RSI,
                              * grant_count > JE_ABI_SPAWN_GRANTS_MAX, RDX/count
                              * mismatch, quotas [63:32] != 0, handle quota >
                              * JE_VM_HANDLE_QUOTA_MAX, endpoint quota >
                              * JE_EP_CHILD_QUOTA_MAX, R9 != 0. grants -> grant_count x
                              * je_abi_spawn_grant (<= JE_ABI_SPAWN_GRANTS_MAX); RDX != 0
                              * iff grant_count != 0. quotas: [15:0] child handle quota
                              * (0 = default 15, else 1..JE_VM_HANDLE_QUOTA_MAX), [31:16]
                              * child endpoint creation quota (<= JE_EP_CHILD_QUOTA_MAX,
                              * which is 0 in M4), [63:32] must be 0. -> RDX = new
                              * PROCESS handle (INSPECT|KILL) in the caller's table, 0
                              * on failure. Transactional: on any failure every pool is
                              * back to baseline and the child never ran (ADR 0010 D5). */
    /* 16 PROC_KILL and 17-19 DEV_* are reserved by
     * ADR 0010 for later M4 PRs. Until then they stay UNSUPPORTED. */
    , JE_SYS_PROC_STATUS = 15 /* M4 proc_status(h=RDI, out=RSI): needs INSPECT on a
                              * PROCESS handle. Copies a 16-byte je_abi_proc_status
                              * {u32 state, u32 reason, u64 code} to user memory at RSI
                              * through the checked copy-out path. A fault gives
                              * INVALID and nothing is written. RDX..R9 reserved (0);
                              * RDI[63:32] must be 0. Non-blocking and idempotent
                              * (ADR 0010 D4). */
#ifdef JE_DEV_COPY_INJECT
    , JE_SYS_COPY_FIXUP_INJECT = 4  /* dev-only: raw-copy an unmapped page to test the fixup */
#endif
} je_abi_op;

/* M3b handle object types (RDX from handle_query). Only SELF exists in M3b: a
 * kernel-minted, immortal, resource-free process-identity capability. Type 0 is
 * reserved/invalid so a zeroed slot never reads as a valid type. */
#define JE_ABI_OBJ_SELF 1u
/* M3c endpoint object type (ADR 0009). Pinned equal to the table-side
 * JE_ENDPOINT_OBJ_TYPE in the syscall glue via _Static_assert. */
#define JE_ABI_OBJ_ENDPOINT 2u
#define JE_ABI_OBJ_PROCESS 3u   /* ADR 0010 D3 */
#define JE_ABI_OBJ_LAUNCHER 4u  /* ADR 0010 D3: SPAWN only; no INSPECT; not DUP-able */
/* M3c D6 (step 4): max message payload bytes a try_send accepts. ABI-visible
 * (ring 3 must know the cap); pinned equal to the table-side JE_EP_MSG_MAX in the
 * syscall glue. */
#define JE_ABI_MSG_MAX 64u
/* M3c step 5 (ADR 0009 A5): max capabilities a single message may carry (K).
 * ABI-visible (ring 3 sizes its cap/handle arrays by it); pinned equal to the
 * table-side JE_EP_MSG_CAPS_MAX in the glue. */
#define JE_ABI_MSG_CAPS_MAX 2u
/* M3c step 6 (ADR 0009 A6-3): max blocking-recv timeout, a relative duration in
 * timer ticks. A nonzero R10 timeout above this (or one that would overflow the
 * absolute deadline) is rejected JE_ABI_INVALID — no unbounded block. A policy
 * bound like the other JE_* caps; the rule (bounded + overflow-checked) is firm. */
#define JE_IPC_MAX_WAIT 0x100000000ull  /* 2^32 ticks */
_Static_assert(JE_SYS_HANDLE_SELF == 5, "abi op handle_self");
_Static_assert(JE_SYS_HANDLE_CLOSE == 6, "abi op handle_close");
_Static_assert(JE_SYS_HANDLE_QUERY == 7, "abi op handle_query");
_Static_assert(JE_SYS_HANDLE_RIGHTS == 8, "abi op handle_rights");
_Static_assert(JE_SYS_HANDLE_DUP == 9, "abi op handle_dup");
_Static_assert(JE_SYS_ENDPOINT_CREATE == 10, "abi op endpoint_create");
_Static_assert(JE_SYS_TRY_SEND == 11, "abi op try_send");
_Static_assert(JE_SYS_TRY_RECV == 12, "abi op try_recv");
_Static_assert(JE_SYS_PROC_STATUS == 15, "abi op proc_status (ADR 0010)");
_Static_assert(JE_SYS_CLOCK_TICKS == 13, "abi op clock_ticks (ADR 0010 D6)");
_Static_assert(JE_SYS_PROC_SPAWN == 14, "abi op proc_spawn (ADR 0010 D5)");

/* ADR 0010 D5: spawn grant entry. 8 bytes, little-endian, no padding. `handle` names
 * an ENDPOINT in the caller's table; the caller's handle must hold TRANSFER; `rights`
 * must be a subset of its rights (unknown bits -> INVALID). */
typedef struct {
    uint32_t handle;
    uint32_t rights;
} je_abi_spawn_grant;
_Static_assert(sizeof(je_abi_spawn_grant) == 8u, "spawn grant ABI size");
#define JE_ABI_SPAWN_GRANTS_MAX 4u
#define JE_ABI_SPAWN_QUOTA_HANDLES(q) ((uint32_t)((q) & 0xffffu))
#define JE_ABI_SPAWN_QUOTA_EPS(q)     ((uint32_t)(((q) >> 16) & 0xffffu))
#define JE_ABI_SPAWN_QUOTAS(h, e)     ((uint64_t)(h) | ((uint64_t)(e) << 16))
/* Startup register ABI v1 (ADR 0010 D5). At a spawned process's first dispatch:
 * RDI = JE_ABI_STARTUP_V1, RSI = n grants, RDX/RCX/R8/R9 = the minted handles in grant
 * order (zero beyond n), RSP = image stack top, every other GPR zero. The kernel-seeded
 * supervisor gets the same layout with n = 1 and RDX = its LAUNCHER handle (PR-C lead
 * decision; the ADR left this unspecified). */
#define JE_ABI_STARTUP_V1 1u

/* ADR 0010 D4: PROC_STATUS output. state: 1 LIVE, 2 DYING, 3 DEAD (0 FREE is never
 * observable through a live handle). reason (valid when DYING/DEAD): 0 FAULT,
 * 1 EXIT, 2 BAD_RETURN, 3 KILLED. code: the exit code for EXIT, otherwise 0. */
typedef struct {
    uint32_t state;
    uint32_t reason;
    uint64_t code;
} je_abi_proc_status;
_Static_assert(sizeof(je_abi_proc_status) == 16u, "proc_status ABI size");
#define JE_ABI_PROC_LIVE  1u
#define JE_ABI_PROC_DYING 2u
#define JE_ABI_PROC_DEAD  3u
#define JE_ABI_DEATH_FAULT      0u
#define JE_ABI_DEATH_EXIT       1u
#define JE_ABI_DEATH_BAD_RETURN 2u
#define JE_ABI_DEATH_KILLED     3u

/* M3b rights ABI (ADR 0008 revisit, frozen here). Rights are a fixed uint32_t
 * bitmask. handle_rights masks its output to JE_ABI_RIGHTS_KNOWN so no internal
 * bit can leak to ring 3. A caller-supplied rights mask with an UNKNOWN bit is
 * rejected as INVALID in the handle_dup decode (the first ring-3 path that takes
 * a rights mask); every kernel mint uses a constant, asserted known at compile
 * time.
 *
 * Rights became genuinely enforced (not just a policy hook) with the handle_dup
 * attenuation PR: a process can derive a handle that DROPS INSPECT, and
 * handle_rights on that derived handle returns DENIED — ring-3-observable, proven
 * in QEMU (the M3b-s1 marker), no longer hosted-only. WRONGTYPE from ring 3 is
 * still not reachable (SELF is the only type), and remains hosted-tested. */
#define JE_ABI_RIGHT_INSPECT 0x1u              /* may read the object's metadata */
/* M3c endpoint rights (ADR 0009 D4a), appended to the known set here. These are
 * the first rights beyond INSPECT, so they are the first to make the handle_dup
 * decode's "unknown bit -> INVALID" gate (syscall.c) distinguish a known-but-
 * unheld right (DENIED, decided in the glue) from a malformed bit (INVALID).
 * NOTE (ABI behaviour change, owner-approved 2026-10-18, Option A): appending
 * SEND=0x2 makes 0x2 a KNOWN bit, so the ring-3 call dup(self, 0x2) — asserted in
 * M3b's test_syscall.c to return INVALID while 0x2 was unknown — now returns
 * DENIED (0x2 is a known right a SELF handle does not hold). The test is updated
 * to assert the new DENIED result for 0x2 AND to re-prove the unknown-bit->INVALID
 * contract on a still-unknown bit; see test_syscall.c and docs/IMPLEMENTATION.md. */
#define JE_ABI_RIGHT_SEND     0x2u             /* endpoint: may try_send */
#define JE_ABI_RIGHT_RECV     0x4u             /* endpoint: may try_recv */
#define JE_ABI_RIGHT_TRANSFER 0x8u             /* endpoint: may grant to a peer */
/* ADR 0010 D3: PROCESS rights are INSPECT (shared) and KILL. KILL is known from
 * PR-B so DUP can carry it; the PROC_KILL op that consumes it arrives in PR-D. */
#define JE_ABI_RIGHT_KILL     0x10u            /* process: may kill (ADR 0010 D8) */
#define JE_ABI_RIGHT_SPAWN    0x20u            /* launcher: may spawn (ADR 0010 D5) */
#define JE_ABI_RIGHTS_KNOWN  (JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_SEND \
                              | JE_ABI_RIGHT_RECV | JE_ABI_RIGHT_TRANSFER \
                              | JE_ABI_RIGHT_KILL | JE_ABI_RIGHT_SPAWN)
_Static_assert(JE_ABI_RIGHT_KILL == 0x10u, "abi right kill (ADR 0010)");
_Static_assert(JE_ABI_RIGHT_SPAWN == 0x20u, "abi right spawn (ADR 0010)");
_Static_assert(JE_ABI_RIGHT_INSPECT == 0x1u, "abi right inspect");
_Static_assert(JE_ABI_RIGHT_SEND == 0x2u && JE_ABI_RIGHT_RECV == 0x4u
               && JE_ABI_RIGHT_TRANSFER == 0x8u, "abi endpoint rights pinned");
_Static_assert((JE_ABI_RIGHTS_KNOWN & ~UINT64_C(0xffffffff)) == 0u, "rights fit u32");

/* abi_query selectors (RDI). */
#define JE_ABI_QUERY_VERSION 0u
#define JE_ABI_QUERY_PROFILE 1u

/* Values abi_query returns in RDX. */
#define JE_ABI_VERSION 0u      /* experimental v0 */
#define JE_ABI_PROFILE 0u      /* M3a baseline profile */

#endif
