/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADR 0014: the arch-neutral process-lifecycle OWNER. This module is the single
 * authority for the process registry (process_pool / registered_vm[] /
 * registered_proc[]) and for the ordered lifecycle side effects (waiter release,
 * device death hooks, je_proc_on_dying, destroy + on_reaped). The per-arch run
 * queue (x86 sched.c; ARM sched_arm.c in MP3) CALLS this module; this module
 * never reads run-queue or architectural state (no trap frame, CR3, TSS.RSP0,
 * RFLAGS, or threads[] here — enforced by tools/check_lifecycle_boundary.sh).
 *
 * Caller context (contract): single CPU, interrupts OFF. The registry writers,
 * je_lc_on_dying, je_lc_kill_check and je_lc_reaped all run under the caller's
 * IF=0 (x86) / DAIF.I-masked (ARM) invariant; this module adds no locking.
 *
 * Fail-stop: these functions RETURN a je_status; the run-queue caller is
 * responsible for fail-stopping (abort_scheduler on x86) when one reports an
 * error, exactly as the pre-extraction mark_dying / reap_dead did inline.
 */
#include <jeios/abi.h>            /* JE_ABI_DEATH_FAULT */
#include <jeios/device.h>
#include <jeios/endpoint_kern.h>
#include <jeios/lifecycle.h>

#if defined(JE_ARM_NO_DEVICE_HOOKS) && !defined(JE_ARM_NO_DEVICE_HOOKS_ACK)
#error "JE_ARM_NO_DEVICE_HOOKS requires an explicit build-profile acknowledgement"
#endif

static je_proc_pool *process_pool;
static je_vm *registered_vm[JE_VM_MAX_PROCESSES];
static je_proc_id registered_proc[JE_VM_MAX_PROCESSES];

void je_sched_set_proc_pool(je_proc_pool *pool)
{
    process_pool = pool;
    for (uint32_t i = 0u; i < JE_VM_MAX_PROCESSES; ++i) {
        registered_vm[i] = (je_vm *)0;
        registered_proc[i] = JE_PROC_ID_NONE;
    }
}

je_status je_sched_register_process(je_vm *vm, uint32_t index, je_proc_id id)
{
    if (!process_pool || !vm || index >= JE_VM_MAX_PROCESSES
        || !je_proc_valid(process_pool, id)
        || (registered_vm[index] != (je_vm *)0 && registered_vm[index] != vm))
        return JE_ERR_INVALID;
    registered_vm[index] = vm;
    registered_proc[index] = id;
    return JE_OK;
}

je_status je_sched_unregister_process(je_vm *vm, uint32_t index)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    if (registered_vm[index] != vm) return JE_OK;
    registered_vm[index] = (je_vm *)0;
    registered_proc[index] = JE_PROC_ID_NONE;
    return JE_OK;
}

je_proc_pool *je_sched_proc_pool(void)
{
    return je_lc_proc_pool();
}

je_vm *je_lc_registered_vm(uint32_t index)
{
    if (index >= JE_VM_MAX_PROCESSES) return (je_vm *)0;
    return registered_vm[index];
}

je_proc_id je_lc_registered_proc(uint32_t index)
{
    if (index >= JE_VM_MAX_PROCESSES) return JE_PROC_ID_NONE;
    return registered_proc[index];
}

je_proc_pool *je_lc_proc_pool(void)
{
    return process_pool;
}

/* M3c A6-CLARIFY-2: release any blocking-recv waiter/reservation a thread holds
 * before its slot is torn down. Keyed on the thread slot (== the endpoint
 * waiter_id). Idempotent: a safe no-op if the thread held none, so the two call
 * sites (the death path in mark_dying, and the live-teardown path in
 * je_sched_drop_user) can both call it and it takes effect once (ADR 0014 part 2,
 * corrected). */
void je_lc_release_waiter(uint32_t slot)
{
    (void)je_endpoint_kern_waiter_release_by_id(slot);
}

/* The ordered dying side-effect prefix, moved verbatim from mark_dying. ORDER is
 * load-bearing (pinned by tests/test_lifecycle_order.c): (1) release the waiter
 * ALWAYS, including for a legacy thread with no PROCESS record (proc_id.gen==0);
 * then, only when the thread HAS a record: (2) on a FAULT death, note the fault
 * after any admin revoke; (3) run the device dying hook; (4) transition the
 * PROCESS record with je_proc_on_dying and RETURN its status so the caller
 * fail-stops on a non-OK result. The two device hooks stay under
 * JE_SCHED_HOSTED_TEST exactly as in the pre-extraction sched.c, so the hosted
 * kill-all test compiles them out while test_lifecycle_order observes them. */
je_status je_lc_on_dying(uint32_t slot, je_proc_id proc_id, uint32_t reason,
                         uint64_t code, uint64_t vector)
{
    je_lc_release_waiter(slot);
    if (proc_id.gen == 0u) return JE_OK;
#if defined(JE_SCHED_HOSTED_TEST) || defined(JE_ARM_NO_DEVICE_HOOKS)
    (void)vector;
#endif
#if !defined(JE_SCHED_HOSTED_TEST) && !defined(JE_ARM_NO_DEVICE_HOOKS)
    if (reason == JE_ABI_DEATH_FAULT)
        je_device_note_fault_after_admin_revoke(je_kernel_device(), proc_id, vector);
    je_device_on_process_dying(je_kernel_device(), proc_id);
#endif
    return je_proc_on_dying(process_pool, proc_id, reason, code);
}

/* The je_sched_kill D8 decision made from the PROCESS RECORD alone (ADR 0010 D8
 * steps 1-5): victim-status lookup, caller==victim, DYING/DEAD idempotent,
 * LIVE-only, supervisor-unnameable, and the run-link lookup. The run-queue gates
 * (the IF<<9 check, the slot kind/state/frame gate, the rip/cs snapshot) and the
 * mark_dying call stay in the caller. Returns INVALID / ALREADY (caller returns
 * JE_OK) / OK(+slot,+vm_index). Caller holds IF=0. */
je_lc_kill_result je_lc_kill_check(je_proc_id victim, je_proc_id caller,
                                   uint32_t *slot, uint32_t *vm_index)
{
    uint32_t victim_state = JE_PROC_FREE;
    uint32_t victim_reason = 0u;
    uint64_t victim_code = 0u;
    if (!process_pool
        || je_proc_status(process_pool, victim, &victim_state, &victim_reason,
                          &victim_code) != JE_OK)
        return JE_LC_KILL_INVALID;
    if (caller.index == victim.index && caller.gen == victim.gen)
        return JE_LC_KILL_INVALID;
    if (victim_state == JE_PROC_DYING || victim_state == JE_PROC_DEAD)
        return JE_LC_KILL_ALREADY;
    if (victim_state != JE_PROC_LIVE)
        return JE_LC_KILL_INVALID;
    if (je_proc_is_supervisor(process_pool, victim))
        return JE_LC_KILL_INVALID;

    uint32_t linked_slot = 0u;
    uint32_t linked_vm_index = 0u;
    if (je_proc_run_link(process_pool, victim, &linked_slot, &linked_vm_index)
        != JE_OK)
        return JE_LC_KILL_INVALID;
    if (slot) *slot = linked_slot;
    if (vm_index) *vm_index = linked_vm_index;
    return JE_LC_KILL_OK;
}

/* Reclaim a reaped process: destroy its VM-owned resources (the held kstack run +
 * page tables) then, if it has a PROCESS record, run on_reaped. MUST be called
 * only AFTER the run queue has dropped the thread slot (je_sched_drop_user): the
 * kstack is freed HERE by destroy, never by the reaper's slot clear (that would
 * double-free, M3a #4 step 3). Returns the first failing status; the caller
 * fail-stops on non-OK, exactly as the pre-extraction reap_dead did. */
je_status je_lc_reaped(je_vm *vm, uint32_t index, je_proc_id proc_id)
{
    je_status s = je_vm_destroy_owned(vm, index, proc_id);
    if (s != JE_OK) return s;
    if (proc_id.gen != 0u) return je_proc_on_reaped(process_pool, proc_id);
    return JE_OK;
}
