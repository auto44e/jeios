/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_SCHED_H
#define JEIOS_SCHED_H
#include <stdbool.h>
#include <jeios/trap.h>
#include <jeios/status.h>
#define JE_THREAD_MAX 8u
/* Single CPU, kernel privilege only. Bootstrap slot 0 is the idle/monitor task.
 * Init/create/reap require interrupts disabled. Sleep/yield serialize internally.
 * No SIMD, user credentials, SMP, priorities, cancellation or dynamic slot reuse. */
void je_sched_init(void);
je_status je_thread_create(void (*entry)(void *),void *argument);
je_status je_thread_sleep(uint64_t ticks);
_Noreturn void je_thread_exit(void);
je_trap_frame *je_sched_interrupt(je_trap_frame *frame,bool timer);
bool je_sched_finished(void);
je_status je_sched_reap(void);
uint64_t je_sched_ticks(void);
uint64_t je_sched_preemptions(void);
uint64_t je_sched_min_dispatches(void);
extern volatile uint64_t je_timer_ticks;
int je_arch_register_probe(void);
#endif

