/*
 * PROCESS records (ADR 0010 D4 / D-DEV). Pure and hostable: caller-supplied pool
 * storage, no global state, no scheduler or vm dependency. The kernel owns one static
 * pool. Each hosted test owns its own.
 *
 * A record is independent of the VM slot and the thread slot. Its identity is
 * {index, generation}. Nothing that must tell two incarnations apart keys on a slot
 * number: the creation charge, the PROCESS handle object, the thread back-link
 * and, later, the D9 binding all use the identity.
 *
 * Lifecycle: FREE -> LIVE (alloc, generation++) -> DYING (on_dying) -> DEAD
 * (on_reaped) -> FREE when DEAD and handle_refs == 0. A LIVE record that has never
 * been published may be abandoned straight back to FREE (spawn rollback, D5).
 *
 * Single CPU; every call requires IF=0 (the kernel calls these with interrupts
 * disabled; hosted tests are single-threaded). Nothing here allocates.
 *
 * Pinned in the PR-B review (lead, proposer in brackets):
 *   - A NULL pool gives JE_ERR_INVALID for every status-returning op. Void ops do
 *     nothing, and valid() returns false. [Claude A7]
 *   - Retired slots stay in state FREE with retired = true, and are not counted in
 *     used_count. [Claude A10]
 *   - A record is "published" exactly when it is run-linked. Only an unlinked
 *     record can be abandoned. [Claude A5, Codex mutant]
 *   - The run link is cleared ONLY by on_reaped: run_linked = false and
 *     thread_slot = vm_index = 0. No other code writes these fields. [Codex P1-3,
 *     Claude P3-7]
 *   - Creation charging requires a LIVE record. [Codex P1-2, Claude A8]
 */
#ifndef JEIOS_PROC_H
#define JEIOS_PROC_H

#include <stdbool.h>
#include <stdint.h>
#include <jeios/status.h>

/* ADR 0010 D2: R = P live + terminal records named by the supervisor (bounded by its
 * handle quota) = 8 + 39 = 47, rounded up to 48. */
#define JE_PROC_REC_MAX 48u
/* handle_refs bound per record (PROCESS handles all live in one table in M4).
 * Exceeding it is JE_ERR_BUSY from je_proc_retain, which the syscall maps to H_REFOVERFLOW. */
#define JE_PROC_REF_MAX 40u
/* Largest valid death reason (JE_DEATH_KILLED). on_dying rejects anything above
 * it. Kept as a literal so this header stays free of scheduler includes; the kernel
 * pins it with a _Static_assert against JE_DEATH_KILLED and JE_ABI_DEATH_KILLED. */
#define JE_PROC_REASON_MAX 3u
/* Highest usable generation. A slot whose generation would pass this is RETIRED: it
 * is never allocated again (ADR 0008 D4 style, no wraparound). Overridable before
 * the include, so hosted tests can exercise retirement with a small value. */
#ifndef JE_PROC_GEN_MAX
#define JE_PROC_GEN_MAX 0xffffffffu
#endif

typedef enum {
    JE_PROC_FREE  = 0,
    JE_PROC_LIVE  = 1,
    JE_PROC_DYING = 2,
    JE_PROC_DEAD  = 3,
} je_proc_state;

/* Identity. gen == 0 is never a valid identity. NONE is EXACTLY {0, 0}; any other
 * gen-0 value (e.g. {1, 0}) is an INVALID identity, not NONE, and is never treated as
 * "uncharged". [Claude round-2 finding: round 1 treated any gen-0 id as NONE, which
 * bypassed the quota.] */
typedef struct {
    uint32_t index;
    uint32_t gen;
} je_proc_id;
#define JE_PROC_ID_NONE ((je_proc_id){ 0u, 0u })

typedef struct {
    uint32_t state;        /* je_proc_state */
    uint32_t gen;          /* last generation issued in this slot (0 = never) */
    bool     retired;      /* generation exhausted: never allocated again */
    uint32_t reason;       /* JE_DEATH_* once DYING/DEAD; 0 while LIVE */
    uint64_t code;         /* exit code (EXIT) or 0 */
    bool     run_linked;   /* run link valid (LIVE/DYING only) */
    uint32_t thread_slot;  /* run link: scheduler slot */
    uint32_t vm_index;     /* run link: vm process index */
    bool     supervisor;   /* kernel-seeded first process (D12) */
    uint32_t handle_refs;  /* live PROCESS handles naming this record */
    uint32_t ep_charge;    /* channels this identity created and still alive */
    uint32_t ep_limit;     /* creation quota for this identity (D-DEV) */
} je_proc_rec;

typedef struct {
    je_proc_rec rec[JE_PROC_REC_MAX];
    uint32_t    used_count;  /* records not FREE */
} je_proc_pool;

/* Every record FREE, gen 0, not retired, counts zero. Call once per pool. */
void je_proc_pool_init(je_proc_pool *pool);

/* Allocate the lowest FREE, non-retired record: generation++, state LIVE, every other
 * field zeroed except ep_limit and supervisor (set from the arguments). Statuses:
 *   pool or out NULL -> JE_ERR_INVALID;
 *   no FREE non-retired record -> JE_ERR_NOMEM, *out = NONE.
 * A slot whose next generation would exceed JE_PROC_GEN_MAX is marked retired
 * instead of being allocated, and the search continues. Retiring a slot this way is
 * permitted even when the call then returns NOMEM: retirement is monotone and frees
 * nothing. Nothing ELSE changes on NOMEM. [Codex P2-8, Claude A9] */
je_status je_proc_alloc(je_proc_pool *pool, uint32_t ep_limit, bool supervisor,
                        je_proc_id *out);

/* True iff `id` names a non-FREE record with exactly that generation. */
bool je_proc_valid(const je_proc_pool *pool, je_proc_id id);

/* Spawn rollback (D5). The record must be LIVE, unpublished (!run_linked), with
 * handle_refs == 0 and ep_charge == 0. It returns to FREE and keeps its generation (the identity was
 * never exposed; the next alloc bumps it). Anything else -> JE_ERR_INVALID with no change. */
je_status je_proc_abandon(je_proc_pool *pool, je_proc_id id);

/* Set the run link {thread_slot, vm_index}. The record must be LIVE and not already
 * linked, else JE_ERR_INVALID with no change. One thread per process. */
je_status je_proc_link(je_proc_pool *pool, je_proc_id id, uint32_t thread_slot,
                       uint32_t vm_index);

/* mark_dying hook. LIVE -> DYING with {reason, code}. If the record is already DYING
 * or DEAD: JE_OK with no change, so the first terminal reason wins. Invalid id, or
 * reason > JE_PROC_REASON_MAX -> JE_ERR_INVALID with no change. [Codex P3-9, Claude A12]
 * Kernel-forced teardown of a fixture process (the test harness destroying a live
 * process) reports reason JE_DEATH_KILLED through this same path, then on_reaped. */
je_status je_proc_on_dying(je_proc_pool *pool, je_proc_id id, uint32_t reason,
                           uint64_t code);

/* Reaper hook, called after je_vm_destroy. DYING -> DEAD, run link cleared
 * (run_linked = false, thread_slot = vm_index = 0). If
 * handle_refs == 0 the record is then freed (FREE, used_count--). Not DYING ->
 * JE_ERR_INVALID with no change. */
je_status je_proc_on_reaped(je_proc_pool *pool, je_proc_id id);

/* PROCESS handle reference. retain: valid id -> handle_refs++ (at JE_PROC_REF_MAX ->
 * JE_ERR_BUSY, no change); invalid -> JE_ERR_INVALID. release: handle_refs--; if the
 * record is DEAD and refs reach 0 it is freed. Release with an invalid id or 0 refs is
 * a no-op: it never underflows. */
je_status je_proc_retain(je_proc_pool *pool, je_proc_id id);
void      je_proc_release(je_proc_pool *pool, je_proc_id id);

/* Snapshot for PROC_STATUS. Any NULL out pointer or an invalid id -> JE_ERR_INVALID. */
je_status je_proc_status(const je_proc_pool *pool, je_proc_id id, uint32_t *state,
                         uint32_t *reason, uint64_t *code);

/* D-DEV creation charge. charge: a LIVE record with ep_charge < ep_limit ->
 * ep_charge++, JE_OK; at the limit -> JE_ERR_DENIED (mapped to QUOTA), no change; an
 * invalid id, or a DYING/DEAD record -> JE_ERR_INVALID, no change. uncharge: decrements only when `id` still names the SAME identity
 * in any non-FREE state (LIVE, DYING or DEAD) and ep_charge > 0; otherwise a no-op,
 * because the creator was freed or reused. JE_PROC_ID_NONE is "uncharged": charge
 * returns JE_OK and uncharge is a no-op (kernel fixtures). */
je_status je_proc_charge(je_proc_pool *pool, je_proc_id id);
void      je_proc_uncharge(je_proc_pool *pool, je_proc_id id);

#endif /* JEIOS_PROC_H */
