/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/arch.h>
#include <jeios/device.h>
#include <jeios/sched.h>

static je_device g_device;

je_device *je_kernel_device(void) { return &g_device; }

static _Noreturn void device_panic(const char *message)
{
    je_arch_console_write(message);
    je_arch_halt();
}

void je_device_init(je_device *device, je_proc_pool *procs)
{
    if (!device) return;
    *device = (je_device){0};
    device->procs = procs;
    device->owner = JE_PROC_ID_NONE;
}

void *je_device_object(je_device *device) { return (void *)device; }
bool je_device_is_object(const je_device *device, const void *object)
{
    return device && object == (const void *)device;
}

static bool table_has_driver(const je_device *d, je_vm *vm, uint32_t vm_index,
                             je_handle handle, uint32_t *rights)
{
    uint32_t type = 0u, got_rights = 0u;
    void *object = (void *)0;
    if (!d || !d->active || vm != d->driver_vm || vm_index != d->driver_vm_index
        || handle != je_handle_encode(d->driver_slot, d->driver_handle_gen)
        || je_handle_resolve_any(&vm->procs[vm_index].htable, handle, &type,
                                 &got_rights, &object) != JE_HANDLE_OK
        || type != JE_ABI_OBJ_DEVICE || object != (void *)d
        || (got_rights & JE_ABI_RIGHT_PIO) == 0u)
        return false;
    if (rights) *rights = got_rights;
    return true;
}

static uint64_t next_tick(uint64_t now, uint64_t last)
{
    if (now == UINT64_MAX || last == UINT64_MAX) return UINT64_MAX;
    return (now + 1u > last + 1u) ? now + 1u : last + 1u;
}

static void schedule_rearm(je_device *d, uint64_t now, uint32_t gen)
{
    d->rearm_pending = true;
    d->ack_gen = gen;
    d->rearm_tick = next_tick(now, d->last_unmask);
}

je_abi_status je_device_assign(je_device *d, je_proc_id caller,
                               je_proc_id target, je_vm *target_vm,
                               uint32_t target_vm_index, je_handle *out)
{
    if (out) *out = 0u;
    if (!d || !d->procs || !out || !target_vm
        || target_vm_index >= JE_VM_MAX_PROCESSES
        || !je_proc_valid(d->procs, caller)
        || !je_proc_valid(d->procs, target))
        return JE_ABI_INVALID;
    if (d->procs->rec[target.index].state != JE_PROC_LIVE)
        return JE_ABI_H_STALE;
    if (je_proc_is_supervisor(d->procs, target)
        || (target.index == caller.index && target.gen == caller.gen))
        return JE_ABI_INVALID;
    if (d->active) return JE_ABI_H_DENIED;
    if (d->binding_gen >= UINT32_MAX - 2u) return JE_ABI_H_QUOTA;

    je_handle_status preflight =
        je_handle_can_alloc(&target_vm->procs[target_vm_index].htable);
    if (preflight != JE_HANDLE_OK) {
        return preflight == JE_HANDLE_QUOTA ? JE_ABI_H_QUOTA : JE_ABI_H_NOSLOTS;
    }

    d->owner = target;
    d->driver_vm = target_vm;
    d->driver_vm_index = target_vm_index;
    d->binding_gen++;
    d->active = true;
    d->pending = false;
    d->rearm_pending = false;
    je_handle h = 0u;
    je_handle_status alloc = je_handle_alloc(
        &target_vm->procs[target_vm_index].htable, d, JE_ABI_OBJ_DEVICE,
        JE_ABI_RIGHT_PIO | JE_ABI_RIGHT_IRQ, &h);
    if (alloc != JE_HANDLE_OK)
        device_panic("JeiOS fatal: DEVICE post-preflight allocation\r\n");
    uint32_t slot = 0u, gen = 0u;
    je_handle_decode(h, &slot, &gen);
    d->driver_slot = slot;
    d->driver_handle_gen = gen;
    d->irq3_binding_deliveries = 0u;
    schedule_rearm(d, je_sched_ticks(), d->binding_gen);
    *out = h;
    return JE_ABI_OK;
}

static je_abi_status device_revoke_with_reason(je_device *d,
                                               je_device_revoke_reason reason)
{
    if (!d) return JE_ABI_INVALID;
    if (!d->active || d->revoking) return JE_ABI_OK;
    d->revoking = true;
    je_vm *vm = d->driver_vm;
    uint32_t vm_index = d->driver_vm_index;
    uint32_t slot = d->driver_slot;
    uint32_t gen = d->driver_handle_gen;
    je_proc_id old_owner = d->owner;
    if (reason == JE_DEVICE_REVOKE_ADMIN) {
        ++d->revoke_admin_count;
        d->last_admin_revoke_owner = old_owner;
        d->admin_revoke_pending_fault = true;
    } else if (reason == JE_DEVICE_REVOKE_CLOSE) {
        ++d->revoke_close_count;
    } else {
        ++d->revoke_death_count;
    }
    d->driver_vm = (je_vm *)0;
    d->driver_vm_index = d->driver_slot = d->driver_handle_gen = 0u;
    je_arch_irq3_mask();
    if (d->binding_gen >= UINT32_MAX) device_panic("JeiOS fatal: DEVICE generation wrap\r\n");
    ++d->binding_gen;
    d->pending = false;
    d->rearm_pending = false;
    je_arch_tss_set_com2_io(false);
    if (vm && vm_index < JE_VM_MAX_PROCESSES
        && vm->procs[vm_index].state == JE_VM_READY) {
        je_handle h = je_handle_encode(slot, gen);
        uint32_t type = 0u, rights = 0u;
        void *object = (void *)0;
        if (je_handle_resolve_any(&vm->procs[vm_index].htable, h, &type,
                                  &rights, &object) == JE_HANDLE_OK
            && type == JE_ABI_OBJ_DEVICE && object == (void *)d
            && je_handle_close(&vm->procs[vm_index].htable, h) != JE_HANDLE_OK)
            device_panic("JeiOS fatal: DEVICE raw close\r\n");
    }
    d->owner = JE_PROC_ID_NONE;
    d->active = false;
    d->revoking = false;
    return JE_ABI_OK;
}

je_abi_status je_device_revoke(je_device *d)
{
    return device_revoke_with_reason(d, JE_DEVICE_REVOKE_ADMIN);
}

je_abi_status je_device_revoke_close(je_device *d)
{
    return device_revoke_with_reason(d, JE_DEVICE_REVOKE_CLOSE);
}

je_abi_status je_device_revoke_death(je_device *d)
{
    return device_revoke_with_reason(d, JE_DEVICE_REVOKE_DEATH);
}

void je_device_note_fault_after_admin_revoke(je_device *d, je_proc_id id,
                                             uint64_t vector)
{
    if (!d || vector != 13u || !d->admin_revoke_pending_fault
        || d->last_admin_revoke_owner.index != id.index
        || d->last_admin_revoke_owner.gen != id.gen)
        return;
    ++d->gp_after_admin_revoke;
    d->admin_revoke_fault_seen = true;
}

void je_device_note_hardened_refusal(je_device *d)
{
    if (!d) return;
    ++d->hardened_refusal_count;
    d->hardened_before_binding_gen = d->binding_gen;
    d->hardened_before_owner = d->owner;
    d->hardened_before_active = d->active;
    /* The refusal path is a single kernel transaction. Recording the second
     * snapshot after all validation makes the no-side-effect contract
     * externally inspectable by the mode-15 corroborator. */
    d->hardened_after_binding_gen = d->binding_gen;
    d->hardened_after_owner = d->owner;
    d->hardened_after_active = d->active;
}

je_abi_status je_device_irq_ack(je_device *d, je_proc_id caller,
                                je_vm *vm, uint32_t vm_index, je_handle h,
                                uint32_t *out_pending)
{
    if (out_pending) *out_pending = 0u;
    if (!d || !out_pending || !table_has_driver(d, vm, vm_index, h, NULL)
        || caller.index != d->owner.index || caller.gen != d->owner.gen)
        return JE_ABI_H_STALE;
    bool pending = d->pending;
    d->pending = false;
    if (pending) {
        d->irq3_ack_observed = true;
        schedule_rearm(d, je_sched_ticks(), d->binding_gen);
    }
    *out_pending = pending ? 1u : 0u;
    return JE_ABI_OK;
}

void je_device_on_process_dying(je_device *d, je_proc_id id)
{
    if (!d) return;
    if (d->active && d->owner.index == id.index && d->owner.gen == id.gen) {
        (void)je_device_revoke_death(d);
        return;
    }
    if (d->admin_revoke_fault_seen
        && d->last_admin_revoke_owner.index == id.index
        && d->last_admin_revoke_owner.gen == id.gen) {
        ++d->revoke_death_count;
        d->admin_revoke_pending_fault = false;
        d->admin_revoke_fault_seen = false;
    }
}

void je_device_irq3(je_device *d)
{
    je_arch_irq3_mask();
    if (d) {
        uint64_t now = je_sched_ticks();
        if (d->irq3_deliveries != 0u && d->irq3_last_tick == now)
            d->irq3_duplicate_tick = true;
        d->irq3_last_tick = now;
        ++d->irq3_deliveries;
        if (d->active) {
            d->pending = true;
            d->irq3_pending_observed = true;
            ++d->irq3_binding_deliveries;
            if (d->irq3_binding_deliveries > d->irq3_max_binding_deliveries)
                d->irq3_max_binding_deliveries = d->irq3_binding_deliveries;
        }
    }
    je_arch_master_eoi();
}

void je_device_irq7(je_device *d)
{
    uint8_t isr = je_arch_master_isr();
    if (isr & 0x80u) {
        je_arch_master_eoi();
        return;
    }
    if (je_arch_irq3_is_unmasked())
        je_arch_irq3_mask();
    if (d && d->active)
        schedule_rearm(d, je_sched_ticks(), d->binding_gen);
}

void je_device_timer(je_device *d, uint64_t now)
{
    if (!d || !d->rearm_pending) return;
    if (!d->active || d->binding_gen != d->ack_gen) {
        d->rearm_pending = false;
        return;
    }
    if (now >= d->rearm_tick) {
        je_arch_irq3_unmask();
        d->last_unmask = now;
        d->rearm_pending = false;
    }
}

void je_device_apply_dispatch(je_device *d, je_proc_id id,
                              je_handle_table *table, bool user)
{
    bool allow = false;
    if (user) {
        if (d && d->active && d->procs
            && d->procs->rec[d->owner.index].state == JE_PROC_DYING)
            device_panic("JeiOS fatal: DYING DEVICE owner at dispatch\r\n");
        allow = d && d->owner.index == id.index && d->owner.gen == id.gen
             && table_has_driver(d, d->driver_vm, d->driver_vm_index,
                                 je_handle_encode(d->driver_slot,
                                                  d->driver_handle_gen), NULL)
             && id.gen != 0u;
    }
    je_arch_tss_set_com2_io(allow);
    (void)table;
}

bool je_device_close_matches(const je_device *d, je_proc_id caller,
                             je_vm *vm, uint32_t vm_index, je_handle h)
{
    return d && d->active && d->owner.index == caller.index
        && d->owner.gen == caller.gen && vm == d->driver_vm
        && vm_index == d->driver_vm_index
        && h == je_handle_encode(d->driver_slot, d->driver_handle_gen);
}

bool je_device_teardown_matches(const je_device *d, je_proc_id owner,
                                const je_handle_table *table)
{
    if (!d || !d->active || !table) return false;
    return (d->owner.index == owner.index && d->owner.gen == owner.gen)
        || (d->driver_vm && d->driver_vm_index < JE_VM_MAX_PROCESSES
            && table == &d->driver_vm->procs[d->driver_vm_index].htable);
}

void je_device_teardown_check(const je_device *d, je_proc_id id,
                              je_handle_table *table)
{
    (void)id;
    (void)table;
    if (!d || d->active || d->owner.gen != 0u || d->pending
        || d->rearm_pending || je_arch_irq3_is_unmasked())
        device_panic("JeiOS fatal: DEVICE teardown state survived\r\n");
}
