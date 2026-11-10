/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AArch64 MP3c device authority is intentionally absent.  Endpoint/lifecycle
 * code still links against the common device boundary, but every authority
 * operation fails closed as UNSUPPORTED and every revoke/death callback is a
 * bounded no-op.
 */
#include <stddef.h>

#include <jeios/device.h>

static je_device je_arm_device;

je_device *je_kernel_device(void)
{
    return &je_arm_device;
}

void je_device_init(je_device *device, je_proc_pool *procs)
{
    if (!device) return;
    *device = (je_device){0};
    device->procs = procs;
}

void *je_device_object(je_device *device)
{
    return (void *)device;
}

bool je_device_is_object(const je_device *device, const void *object)
{
    return device != NULL && object == (const void *)device;
}

je_abi_status je_device_assign(je_device *device, je_proc_id caller,
                               je_proc_id target, je_vm *target_vm,
                               uint32_t target_vm_index, je_handle *out)
{
    (void)device; (void)caller; (void)target; (void)target_vm;
    (void)target_vm_index;
    if (out) *out = 0u;
    return JE_ABI_UNSUPPORTED;
}

je_abi_status je_device_revoke(je_device *device)
{
    (void)device;
    return JE_ABI_UNSUPPORTED;
}

je_abi_status je_device_revoke_close(je_device *device)
{
    (void)device;
    return JE_ABI_OK;
}

je_abi_status je_device_revoke_death(je_device *device)
{
    (void)device;
    return JE_ABI_OK;
}

void je_device_note_fault_after_admin_revoke(je_device *device,
                                             je_proc_id id, uint64_t vector)
{
    (void)device; (void)id; (void)vector;
}

void je_device_note_hardened_refusal(je_device *device) { (void)device; }
void je_device_note_hardened_refusal_after(je_device *device) { (void)device; }

je_abi_status je_device_irq_ack(je_device *device, je_proc_id caller,
                                je_vm *caller_vm, uint32_t caller_vm_index,
                                je_handle handle, uint32_t *out_pending)
{
    (void)device; (void)caller; (void)caller_vm;
    (void)caller_vm_index; (void)handle;
    if (out_pending) *out_pending = 0u;
    return JE_ABI_UNSUPPORTED;
}

void je_device_on_process_dying(je_device *device, je_proc_id id)
{
    (void)device; (void)id;
}

void je_device_irq3(je_device *device) { (void)device; }
void je_device_irq7(je_device *device) { (void)device; }
void je_device_timer(je_device *device, uint64_t now)
{
    (void)device; (void)now;
}

void je_device_apply_dispatch(je_device *device, je_proc_id id,
                              je_handle_table *table, bool user)
{
    (void)device; (void)id; (void)table; (void)user;
}

bool je_device_close_matches(const je_device *device, je_proc_id caller,
                             je_vm *vm, uint32_t vm_index, je_handle handle)
{
    (void)device; (void)caller; (void)vm; (void)vm_index; (void)handle;
    return false;
}

bool je_device_teardown_matches(const je_device *device, je_proc_id owner,
                                const je_handle_table *table)
{
    (void)device; (void)owner; (void)table;
    return false;
}

void je_device_teardown_check(const je_device *device, je_proc_id id,
                              je_handle_table *table)
{
    (void)device; (void)id; (void)table;
}
