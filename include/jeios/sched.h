/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_SCHED_H
#define JEIOS_SCHED_H
#include <stdbool.h>
#include <jeios/trap.h>
#include <jeios/status.h>
#include <jeios/vm.h>
#define JE_THREAD_MAX 8u
/* Single CPU. Bootstrap slot 0 is the idle/monitor task; other slots are kernel
 * threads or (M3a #4) ring-3 user threads bound to a vm process (own CR3 +
 * TSS.RSP0). Init/create/reap require interrupts disabled. Sleep/yield serialize
 * internally. No SIMD, user credentials, SMP, priorities, cancellation or
 * dynamic slot reuse. */
void je_sched_init(void);
je_status je_thread_create(void (*entry)(void *),void *argument);
/* M3a #4 step 3: create a ring-3 thread for a READY vm process (see vm.h). The
 * process's held kernel stack becomes TSS.RSP0; requires NX (ADR 0007 D2). */
je_status je_thread_create_user(je_vm *vm, uint32_t index);
/* M3a #4 step 3: remove a user thread (by owning vm process) from scheduling
 * before je_vm_destroy frees its kernel stack. Interrupts disabled; not the
 * running thread. */
je_status je_sched_drop_user(je_vm *vm, uint32_t index);
je_status je_thread_sleep(uint64_t ticks);
_Noreturn void je_thread_exit(void);
je_trap_frame *je_sched_interrupt(je_trap_frame *frame,bool timer);
/* M3a #4 step 4: terminate ONLY the current user process on a synchronous user
 * fault (or user int 0x80); returns a live successor's frame to iretq to. Called
 * from je_trap_dispatch with interrupts disabled. Never requeues the dead task. */
je_trap_frame *je_sched_fault_terminate(je_trap_frame *frame, uint64_t cr2);
/* M3a #4 step 4: reap DYING user processes from bootstrap (slot 0, irq off)
 * while peers run. Returns count reaped, or a negative je_status on violation. */
int je_sched_reap_dead(void);
bool je_sched_has_dying(void);
je_status je_sched_death_record(je_vm *vm, uint32_t index, uint64_t *vec, uint64_t *err,
                                uint64_t *rip, uint64_t *cs, uint64_t *cr2);
uint64_t je_sched_dispatches_of(je_vm *vm, uint32_t index);
bool je_sched_finished(void);
je_status je_sched_reap(void);
uint64_t je_sched_ticks(void);
uint64_t je_sched_preemptions(void);
uint64_t je_sched_user_cpl3_dispatches(void);
uint64_t je_sched_min_dispatches(void);
extern volatile uint64_t je_timer_ticks;
int je_arch_register_probe(void);
#endif

