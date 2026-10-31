/*
 * ADR 0010 D5 journaled spawn (kernel only). Called from the PROC_SPAWN syscall glue
 * with IF=0, after the decode checks and after the grants are copied into kernel
 * memory.
 *
 * Status precedence (PR-C lead decision; validation before any state change):
 *   1. launcher: resolve as LAUNCHER with SPAWN (BADINDEX / STALE / WRONGTYPE / DENIED);
 *   2. image_id unknown -> INVALID;
 *   3. quotas: handle quota 0 = default 15, else 1..JE_VM_HANDLE_QUOTA_MAX; endpoint
 *      quota <= JE_EP_CHILD_QUOTA_MAX; n > the effective child handle quota (after the
 *      default is applied) -> INVALID before any state change [Claude PR-C review P2-1];
 *      any other -> INVALID;
 *   4. each grant in order: resolve as an ENDPOINT holding TRANSFER, with rights a
 *      subset (H_* status of the first failing grant);
 *   5. caller-table preflight je_handle_can_alloc (QUOTA / NOSLOTS);
 *   6. the journal, steps 1-5: PROCESS record, VM build, grant mints, thread, publish.
 *      Internal NOMEM/BUSY map to H_NOSLOTS: ADR 0010 adds no new statuses.
 *
 * On OK, *out is a PROCESS handle with INSPECT|KILL in caller_idx's table. On any
 * failure *out = 0, the child never ran, and the page, endpoint, handle, record,
 * thread and vm-slot pools are exactly at their baseline (the journal undoes in
 * reverse).
 *
 * Rollback once the child thread exists (journal step 4+): the record was run-linked,
 * so it cannot be abandoned (proc.h). It goes through the pinned kernel-forced
 * teardown rule: on_dying(JE_DEATH_KILLED) after drop_user and vm_destroy, then
 * on_reaped, which frees it (no handle was published). [Found independently by the
 * Codex worker (r2) and the Claude review (P1-1). Rejected: a new je_proc_unlink API,
 * which widens the set of record mutators for no gain.]
 */
#ifndef JEIOS_SPAWN_H
#define JEIOS_SPAWN_H

#include <stdint.h>
#include <jeios/abi.h>
#include <jeios/handle.h>
#include <jeios/vm.h>

je_abi_status je_proc_spawn(je_vm *vm, uint32_t caller_idx, je_handle launcher,
                            uint32_t image_id, const je_abi_spawn_grant *grants,
                            uint32_t n, uint32_t handle_quota, uint32_t ep_quota,
                            je_handle *out);

/* Kernel-internal fail-Nth hook. It is NOT reachable from the ABI: only kernel
 * fixture code calls it. step 1..5 = fail at that journal step; for step 3,
 * grant_index picks which mint fails. step 0 disarms. The hook fires once, then
 * disarms. Its failure is reported as H_NOSLOTS, like a real resource failure.
 * step > 5 or grant_index >= JE_ABI_SPAWN_GRANTS_MAX are ignored (disarmed). The hook
 * is ALSO disarmed at the end of every je_proc_spawn call, so an unfired arming can
 * never fail a later spawn [Claude P2-2]. It stays compiled in the normal kernel
 * because the QEMU smoke run is its evidence; nothing in the syscall path calls it. */
void je_spawn_failpoint_set(uint32_t step, uint32_t grant_index);

#endif /* JEIOS_SPAWN_H */
