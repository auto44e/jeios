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
    uint32_t irq3_binding_deliveries;
    uint32_t irq3_max_binding_deliveries;
    bool irq3_duplicate_tick;
    bool irq3_pending_observed;
    bool irq3_ack_observed;
    uint32_t revoke_admin_count;
    uint32_t revoke_close_count;
    uint32_t revoke_death_count;
    uint32_t hardened_refusal_count;
    uint32_t hardened_before_binding_gen;
    uint32_t hardened_after_binding_gen;
    je_proc_id hardened_before_owner;
    je_proc_id hardened_after_owner;
    bool hardened_before_active;
    bool hardened_after_active;
    uint8_t hardened_before_com2_io;
    uint8_t hardened_after_com2_io;
    bool hardened_before_irq3_unmasked;
    bool hardened_after_irq3_unmasked;
    je_proc_id last_admin_revoke_owner;
    je_proc_id admin_revoke_owner_death;
    bool admin_revoke_pending_fault;
    bool admin_revoke_fault_seen;
    uint32_t gp_after_admin_revoke;
    uint32_t admin_revoke_owner_death_count;
    bool active;
    bool pending;
    bool revoking;
    bool rearm_pending;
} je_device;

typedef enum {
    JE_DEVICE_REVOKE_ADMIN = 0u,
    JE_DEVICE_REVOKE_CLOSE = 1u,
    JE_DEVICE_REVOKE_DEATH = 2u
} je_device_revoke_reason;

je_device *je_kernel_device(void);
void je_device_init(je_device *device, je_proc_pool *procs);
void *je_device_object(je_device *device);
bool je_device_is_object(const je_device *device, const void *object);

je_abi_status je_device_assign(je_device *device, je_proc_id caller,
                               je_proc_id target, je_vm *target_vm,
                               uint32_t target_vm_index, je_handle *out);
je_abi_status je_device_revoke(je_device *device);
je_abi_status je_device_revoke_close(je_device *device);
je_abi_status je_device_revoke_death(je_device *device);
void je_device_note_fault_after_admin_revoke(je_device *device,
                                             je_proc_id id, uint64_t vector);
void je_device_note_hardened_refusal(je_device *device);
void je_device_note_hardened_refusal_after(je_device *device);
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
bool je_device_teardown_matches(const je_device *device, je_proc_id owner,
                                const je_handle_table *table);
void je_device_teardown_check(const je_device *device, je_proc_id id,
                              je_handle_table *table);

#endif
