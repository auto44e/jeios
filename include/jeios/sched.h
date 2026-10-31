/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_SCHED_H
#define JEIOS_SCHED_H
#include <stdbool.h>
#include <jeios/trap.h>
#include <jeios/status.h>
#include <jeios/vm.h>
#include <jeios/endpoint.h>   /* je_ep_channel, for the blocking-recv waiter API */
/* ADR 0010 D2: T = 1 idle + 7 M2 workers + P user threads. Was 8. */
#define JE_THREAD_MAX 16u
#define JE_M2_WORKERS 7u   /* the M2 boot workload stays at exactly 7 workers */
_Static_assert(JE_THREAD_MAX >= 1u + JE_M2_WORKERS + JE_VM_MAX_PROCESSES, "ADR 0010 D2 thread capacity");
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
/* ADR 0010 D5 startup register ABI. Same as je_thread_create_user, but after the frame
 * is zeroed RDI/RSI/RDX/RCX/R8/R9 are set from *st (NULL = all zero), and the chosen
 * scheduler slot is written to *out_slot (may be NULL). No side effect on failure.
 * je_thread_create_user(vm, i) == je_thread_create_user_start(vm, i, NULL, NULL). */
typedef struct { uint64_t rdi, rsi, rdx, rcx, r8, r9; } je_user_startup;
je_status je_thread_create_user_start(je_vm *vm, uint32_t index,
                                      const je_user_startup *st, uint32_t *out_slot);

/* ADR 0010 D8 synchronous kill. Caller: the CURRENT user thread, IF=0 (syscall path).
 * In order:
 *   1. victim record invalid -> JE_ERR_INVALID;
 *   2. victim == the caller's own identity -> JE_ERR_INVALID (use EXIT);
 *   3. victim DYING or DEAD -> JE_OK (idempotent, decided from the RECORD; the first
 *      terminal reason wins);
 *   4. victim is the supervisor -> JE_ERR_INVALID (fail closed; it is unnameable in
 *      the D8 table);
 *   5. LIVE but not run-linked (no thread yet) -> JE_ERR_INVALID (lead decision, PR-D:
 *      fail closed; marking DYING with no thread would leave a vm the reaper never
 *      collects);
 *   6. the linked thread must be USER, carry the same identity, and be READY or
 *      BLOCKED_IPC with a frame; anything else is an invariant break -> fail-stop;
 *   7. mark_dying(slot, KILLED), the single release owner. It releases a WAITING
 *      waiter or a WOKEN reservation (the head stays queued for other RECV holders)
 *      and clears wait_pending, so the victim's continuation never runs and it never
 *      sees CANCELLED.
 * The victim is reaped later by the idle step. mark_dying itself is unchanged. */
je_status je_sched_kill(je_proc_id victim);
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
/* M3a #5: clean exit(code) — terminate the current user process (DEATH_EXIT),
 * return a live successor. And the validated non-switching syscall return path:
 * validate+sanitize the current user frame (ADR "validate on every user return")
 * and resume it, or terminate (DEATH_BAD_RETURN) if the frame is invalid. */
je_trap_frame *je_sched_exit_current(je_trap_frame *frame, uint64_t code);
je_trap_frame *je_sched_return_to_user(je_trap_frame *frame);
/* M3c D7/A6: block the current user thread as a blocking-recv waiter (park +
 * switch; the endpoint waiter must already be registered) and wake one by slot
 * (guarded BLOCKED_IPC->READY). See ADR 0009 A6-CLARIFY-1/STRUCT. */
je_trap_frame *je_sched_block_current(je_trap_frame *frame, je_ep_channel *rec,
                                      uint8_t side, uint64_t deadline);
void je_sched_wake_ipc(uint32_t slot);
/* M3c A6: continuation-prologue accessors (syscall.c). wait_pending reads back the
 * parked recv's owned rec/side + args + terminal RIP; wait_finish clears it once
 * the result is delivered. */
bool je_sched_wait_pending(je_ep_channel **rec, uint8_t *side, uint64_t *rdi,
                           uint64_t *rsi, uint64_t *rdx, uint64_t *r8, uint64_t *r9,
                           uint64_t *post_int_rip);
void je_sched_wait_finish(void);
je_status je_sched_current_slot(uint32_t *out_slot);
/* Public death-reason numbering (matches the scheduler's internal record). */
enum { JE_DEATH_FAULT = 0, JE_DEATH_EXIT = 1, JE_DEATH_BAD_RETURN = 2,
       JE_DEATH_KILLED = 3 /* ADR 0010 D4/D8; produced from PR-D */ };
/* M3a #4 step 4: reap DYING user processes from bootstrap (slot 0, irq off)
 * while peers run. Returns count reaped, or a negative je_status on violation. */
int je_sched_reap_dead(void);
bool je_sched_has_dying(void);
je_status je_sched_death_record(je_vm *vm, uint32_t index, uint64_t *vec, uint64_t *err,
                                uint64_t *rip, uint64_t *cs, uint64_t *cr2);
je_status je_sched_death_reason(je_vm *vm, uint32_t index, uint64_t *reason, uint64_t *code);
uint64_t je_sched_dispatches_of(je_vm *vm, uint32_t index);
je_status je_sched_current_user(je_vm **vm, uint32_t *index);
bool je_sched_finished(void);
je_status je_sched_reap(void);
uint64_t je_sched_ticks(void);
uint64_t je_sched_preemptions(void);
uint64_t je_sched_user_cpl3_dispatches(void);
uint64_t je_sched_min_dispatches(void);
extern volatile uint64_t je_timer_ticks;
int je_arch_register_probe(void);
#endif

