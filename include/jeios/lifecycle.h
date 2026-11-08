/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_LIFECYCLE_H
#define JEIOS_LIFECYCLE_H

#include <stdint.h>
#include <jeios/proc.h>
#include <jeios/status.h>
#include <jeios/vm.h>

void je_sched_set_proc_pool(je_proc_pool *pool);
je_status je_sched_register_process(je_vm *vm, uint32_t index, je_proc_id id);
je_status je_sched_unregister_process(je_vm *vm, uint32_t index);
je_proc_pool *je_sched_proc_pool(void);

je_vm *je_lc_registered_vm(uint32_t index);
je_proc_id je_lc_registered_proc(uint32_t index);
je_proc_pool *je_lc_proc_pool(void);

void je_lc_release_waiter(uint32_t slot);
je_status je_lc_on_dying(uint32_t slot, je_proc_id proc_id, uint32_t reason,
                         uint64_t code, uint64_t vector);

typedef enum {
    JE_LC_KILL_INVALID = 0,
    JE_LC_KILL_ALREADY = 1,
    JE_LC_KILL_OK = 2
} je_lc_kill_result;

je_lc_kill_result je_lc_kill_check(je_proc_id victim, je_proc_id caller,
                                   uint32_t *slot, uint32_t *vm_index);
je_status je_lc_reaped(je_vm *vm, uint32_t index, je_proc_id proc_id);

#endif /* JEIOS_LIFECYCLE_H */
