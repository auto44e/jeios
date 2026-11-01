/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_DEVICE_H
#define JEIOS_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <jeios/abi.h>
#include <jeios/handle.h>
#include <jeios/proc.h>
#include <jeios/vm.h>

typedef struct {
    je_proc_pool *procs;
    je_proc_id owner;
    je_vm *driver_vm;
    uint32_t driver_vm_index;
    uint32_t driver_slot;
    uint32_t driver_handle_gen;
    uint32_t binding_gen;
    uint64_t last_unmask;
    uint64_t rearm_tick;
    uint32_t ack_gen;
    uint64_t irq3_deliveries;
    uint64_t irq3_last_tick;
    bool irq3_duplicate_tick;
    bool active;
    bool pending;
    bool revoking;
    bool rearm_pending;
} je_device;

je_device *je_kernel_device(void);
void je_device_init(je_device *device, je_proc_pool *procs);
void *je_device_object(je_device *device);
bool je_device_is_object(const je_device *device, const void *object);

je_abi_status je_device_assign(je_device *device, je_proc_id caller,
                               je_proc_id target, je_vm *target_vm,
                               uint32_t target_vm_index, je_handle *out);
je_abi_status je_device_revoke(je_device *device);
je_abi_status je_device_irq_ack(je_device *device, je_proc_id caller,
                                je_vm *caller_vm, uint32_t caller_vm_index,
                                je_handle handle, uint32_t *out_pending);

void je_device_on_process_dying(je_device *device, je_proc_id id);
void je_device_irq3(je_device *device);
void je_device_irq7(je_device *device);
void je_device_timer(je_device *device, uint64_t now);
void je_device_apply_dispatch(je_device *device, je_proc_id id,
                              je_handle_table *table, bool user);
bool je_device_close_matches(const je_device *device, je_proc_id caller,
                             je_vm *vm, uint32_t vm_index, je_handle handle);
void je_device_teardown_check(const je_device *device, je_proc_id id,
                              je_handle_table *table);

#endif
