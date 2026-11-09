/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADR 0013 D2/D3/D4/D6: the AArch64 scheduler seam.
 *
 * The round-robin policy lives in kernel/core/sched_rr.c.  This file owns the
 * architecture contract that policy cannot see: guarded stack mapping, the
 * frozen 304-byte frame, SPSR/ELR validation, the returning-IRQ hand-off, and
 * the ARM_SCHED_TEST proof workers.
 *
 * Caller context for the normal seam is EL1 with DAIF.I masked, one CPU, and
 * the MMU enabled.  The IRQ handler has already ACKed and EOIRed the timer
 * before this file is entered.  No allocator or console operation is allowed
 * in the production IRQ switch path.  JE_ARM_SCHED_TEST deliberately adds
 * bounded PL011 diagnostics to its proof/failure path; that test-only I/O is
 * not compiled into the production hand-off.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jeios/alloc.h>
#include <jeios/sched_rr.h>
#include <jeios/status.h>
#ifdef JE_ARM_EL0_TEST
#include <jeios/abi.h>
#include <jeios/lifecycle.h>
#include <jeios/proc.h>
#include <jeios/syscall.h>
#include <jeios/vm.h>
#endif

#include "paging.h"
#include "trapframe.h"

#define JE_ARM_PAGE_SIZE UINT64_C(0x1000)
#define JE_ARM_SPSR_NZCV UINT64_C(0xf0000000)
#define JE_ARM_SPSR_POLICY UINT64_C(0x345)
#define JE_ARM_DAIF_I (UINT64_C(1) << 7)
#define JE_ARM_SPSR_EL0_POLICY UINT64_C(0x340)
#define JE_ARM_VECTOR_EL1_FIRST 4u
#define JE_ARM_VECTOR_EL1_LAST 7u
#define JE_ARM_VECTOR_EL0_FIRST 8u
#define JE_ARM_VECTOR_EL0_LAST 11u
#define JE_ARM_EL0_PROOF_OP UINT64_C(0x7a)
#define JE_ARM_EL0_PROOF_ARG0 UINT64_C(0x1000)
#define JE_ARM_EL0_PROOF_STATUS JE_ABI_UNSUPPORTED
#define JE_ARM_EL0_PROOF_RESULT UINT64_C(0x5aa5)
#define JE_ARM_MAX_PROOF_WORKERS 2u
#define JE_ARM_SENTINEL_SLOT 3u
#define JE_ARM_MONITOR_SLOT 4u
#define JE_ARM_SCHED_TASKS 4u

extern je_pages *je_arm_scheduler_pages(void);
extern je_arm_mmu je_arm_mmu_context;

extern uint8_t _text[];
extern uint8_t __text_end[];
extern uint8_t __stack_bottom[];
extern uint8_t __stack_top[];

static je_rr_table je_arm_rr;
static bool je_arm_rr_ready;
static bool je_arm_stack_mapped[JE_RR_MAX];
static uint32_t je_arm_frame_generation[JE_RR_MAX];

#ifdef JE_ARM_EL0_TEST
/* These arrays are the scheduler-owned binding. A slot is never allowed to
 * retain a stale root after stack teardown/reuse. */
uint64_t je_arm_user_root[JE_RR_MAX];
uint64_t je_arm_user_sp_el1[JE_RR_MAX];
static je_vm *je_arm_user_vm[JE_RR_MAX];
static uint32_t je_arm_user_index[JE_RR_MAX];
static je_proc_id je_arm_user_proc[JE_RR_MAX];
static uint64_t je_arm_user_entry[JE_RR_MAX];
static bool je_arm_user_slot[JE_RR_MAX];
static je_proc_pool *je_arm_proc_pool;
static bool je_arm_pending_user;
static je_vm *je_arm_pending_vm;
static uint32_t je_arm_pending_index;
static je_proc_id je_arm_pending_proc;

static uint64_t arm_user_expected_sp(uint32_t slot)
{
    if (slot == 0u || slot >= JE_RR_MAX ||
        !je_arm_user_slot[slot] ||
        je_arm_user_index[slot] >= JE_VM_MAX_PROCESSES)
        return 0u;
    return JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE -
           ((uint64_t)je_arm_user_index[slot] * UINT64_C(0x40));
}

extern void je_arm_el0_test_note_svc(uint32_t slot, uint64_t op,
                                     je_arm_trapframe *tf);
extern void je_arm_el0_test_before_return(uint32_t slot,
                                          je_arm_trapframe *tf);
extern void je_arm_el0_test_note_fault(uint32_t slot, uint32_t reason,
                                       uint64_t ec, uint64_t far,
                                       uint64_t il, uint64_t iss,
                                       uint64_t fsc, uint64_t wnr);
extern void je_arm_el0_test_note_reaped(uint32_t index, je_proc_id proc_id);
extern void je_arm_el0_test_poison_tpidr(void);
extern void je_arm_el0_test_note_kstack(uint32_t slot);
extern void je_arm_el0_test_note_irq_switch(uint32_t from_slot,
                                            uint32_t to_slot,
                                            uint64_t elr);
extern void je_arm_el0_test_note_svc_proof(uint32_t slot,
                                           const je_arm_trapframe *tf);
#endif

/*
 * These symbols are deliberately volatile: the handler owns the current
 * identity and the proof workers must load it afresh after every resume.
 */
volatile uint32_t je_arm_sched_current_identity;
volatile uint32_t je_arm_sched_exit_flags[JE_RR_MAX];
volatile uint32_t je_arm_sched_nzcv_phase[JE_RR_MAX];
volatile uint64_t je_arm_sched_canary_checks[JE_RR_MAX];

extern uint64_t je_arm_timer_ticks(void);

#ifdef JE_ARM_SCHED_TEST
static volatile uintptr_t je_arm_sched_stack_base[JE_RR_MAX];
static volatile uintptr_t je_arm_sched_stack_top[JE_RR_MAX];
static volatile uint32_t je_arm_sched_exited[JE_RR_MAX];
__attribute__((used))
static volatile uint32_t je_arm_sched_canary_phase[JE_RR_MAX];
static volatile uint32_t je_arm_sched_generation_fault_slot = UINT32_MAX;
static volatile bool je_arm_sched_mutation_point_reached;
static volatile bool je_arm_sched_idle_requested;
static volatile bool je_arm_sched_idle_started;
static volatile bool je_arm_sched_idle_active;
static volatile bool je_arm_sched_idle_complete;
static volatile bool je_arm_sched_idle_all_sleeping;
static volatile uint64_t je_arm_sched_idle_start_tick;
static volatile uint64_t je_arm_sched_idle_end_tick;
static volatile uint64_t je_arm_sched_idle_ticks;
static volatile uint64_t je_arm_sched_idle_worker_resumes;
static volatile uint32_t je_arm_sched_idle_wake_slot;
static volatile bool je_arm_sched_idle_sleep_request[JE_RR_MAX];
static volatile bool je_arm_sched_idle_waiting[JE_RR_MAX];
static void proof_idle_tick(uint32_t current_slot);
#endif

static _Noreturn void arm_sched_trampoline(void *arg);

static void sched_console_putc(char c)
{
    const uintptr_t dr = (uintptr_t)UINT64_C(0x09000000);
    const uintptr_t fr = dr + (uintptr_t)0x18;
    for (unsigned i = 0u; i < 100000u; ++i) {
        if ((*(volatile uint32_t *)fr & UINT32_C(1 << 5)) == 0u) break;
    }
    *(volatile uint32_t *)dr = (uint32_t)(unsigned char)c;
}

static void sched_console_write(const char *text)
{
    for (unsigned i = 0u; i < 4096u && text[i] != '\0'; ++i)
        sched_console_putc(text[i]);
}

__attribute__((unused))
static void sched_console_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (unsigned i = 0u; i < 16u; ++i)
        text[2u + i] = digits[(value >> ((15u - i) * 4u)) & 0xfu];
    text[18] = '\0';
    sched_console_write(text);
}

__attribute__((unused))
static void sched_console_dec(uint64_t value)
{
    char text[21];
    unsigned pos = 20u;
    text[pos] = '\0';
    do {
        --pos;
        text[pos] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0u);
    sched_console_write(&text[pos]);
}

_Noreturn static void sched_halt(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
    for (;;) __asm__ volatile("wfe" ::: "memory");
}

_Noreturn static void sched_fatal(const char *reason)
{
    sched_console_write("JeiOS/aarch64: FATAL sched ");
    sched_console_write(reason);
    sched_console_write("\r\n");
    sched_halt();
}

uint64_t *je_arm_tf_kernel_sp(je_arm_trapframe *frame)
{
    if (frame == NULL ||
        (frame->spsr & JE_ARM_SPSR_M_MASK) != JE_ARM_SPSR_M_EL1H)
        sched_fatal("kernel-sp mode");
    return &frame->sp;
}

uint64_t *je_arm_tf_user_sp(je_arm_trapframe *frame)
{
    if (frame == NULL ||
        (frame->spsr & JE_ARM_SPSR_M_MASK) != JE_ARM_SPSR_M_EL0T)
        sched_fatal("user-sp mode");
    return &frame->sp;
}

static inline void arm_daif_set_i(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static inline void arm_daif_clear_i(void)
{
    __asm__ volatile("msr daifclr, #2; isb" ::: "memory");
}

static inline uint64_t arm_read_daif(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, daif" : "=r"(value) :: "memory");
    return value;
}

static inline void arm_wfi(void)
{
    /*
     * Arm DDI0487: WFI may wake for a pending interrupt while PSTATE.I is
     * masked; DAIF.I remains masked until the following DAIFClr+ISB, which is
     * the point at which the pending IRQ can actually be taken.
     */
    __asm__ volatile("wfi" ::: "memory");
}

static uint32_t next_frame_generation(uint32_t generation)
{
    ++generation;
    return generation == 0u ? 1u : generation;
}

static void arm_sched_validate_sleeping(void)
{
    for (uint32_t slot = 1u; slot < JE_RR_MAX; ++slot)
        if (je_arm_rr.slots[slot].state == JE_RR_SLEEPING &&
            je_arm_rr.slots[slot].wake == 0u)
            sched_fatal("sleep wake");
}

#ifdef JE_ARM_SCHED_TEST
static bool sched_test_worker_frame_elr(uint32_t slot, uint64_t elr)
{
    extern uint8_t je_arm_worker_canary_start[];
    extern uint8_t je_arm_worker_canary_end[];
    extern void je_arm_sched_exit(void);
    extern void je_arm_sched_idle_prepare(void);
    extern void je_arm_worker_entry(void *arg);
    const uint64_t canary_start =
        (uint64_t)(uintptr_t)je_arm_worker_canary_start;
    const uint64_t canary_end =
        (uint64_t)(uintptr_t)je_arm_worker_canary_end;
    uint64_t worker_start = (uint64_t)(uintptr_t)je_arm_sched_exit;
    const uint64_t idle_prepare =
        (uint64_t)(uintptr_t)je_arm_sched_idle_prepare;
    const uint64_t worker_entry =
        (uint64_t)(uintptr_t)je_arm_worker_entry;
    const uint64_t trampoline =
        (uint64_t)(uintptr_t)arm_sched_trampoline;

    /*
     * The test-linked proof worker transition functions are emitted in the
     * final text suffix: exit, idle preparation, worker entry, and the
     * trampoline all precede __text_end.  Taking the minimum of their starts
     * keeps the whitelist bounded to that suffix while the separate canary
     * interval covers its assembly/NZCV windows.
     */
    if (idle_prepare < worker_start) worker_start = idle_prepare;
    if (worker_entry < worker_start) worker_start = worker_entry;
    if (trampoline < worker_start) worker_start = trampoline;

    return slot != 0u && slot <= JE_ARM_MAX_PROOF_WORKERS &&
           ((elr >= canary_start && elr < canary_end) ||
            (elr >= worker_start && elr < (uint64_t)(uintptr_t)__text_end));
}
#endif

typedef enum {
    JE_ARM_FRAME_BAD = 0,
    JE_ARM_FRAME_OK,
    JE_ARM_FRAME_USER_DEATH
} je_arm_frame_result;

static je_arm_frame_result frame_shape_class(const je_rr_ctx *ctx,
                                             void *opaque, unsigned phase,
                                             bool exact_record,
                                             bool require_cookie)
{
    const uintptr_t frame_address = (uintptr_t)opaque;
    uintptr_t frame_end;
    const uintptr_t size = (uintptr_t)JE_TF_SIZE;
    const uintptr_t top = ctx != NULL ? ctx->top : 0u;
    const uintptr_t lower = ctx != NULL ? ctx->usable_base : 0u;
    const uintptr_t text_start = (uintptr_t)_text;
    const uintptr_t text_end = (uintptr_t)__text_end;
    je_arm_trapframe *frame;

    if (ctx == NULL || opaque == NULL || phase > 1u)
        return JE_ARM_FRAME_BAD;
    if (ctx->slot >= JE_RR_MAX || !je_arm_stack_mapped[ctx->slot])
        return JE_ARM_FRAME_BAD;
    if (exact_record && ctx->frame != opaque) return JE_ARM_FRAME_BAD;
    if (top < lower || top - lower < size) return JE_ARM_FRAME_BAD;
    if (frame_address > UINTPTR_MAX - size) return JE_ARM_FRAME_BAD;
    if (frame_address < lower || frame_address > top - size)
        return JE_ARM_FRAME_BAD;
    if ((frame_address & (uintptr_t)0xf) != 0u) return JE_ARM_FRAME_BAD;
    frame_end = frame_address + size;
    if (frame_end > top || (frame_end & (uintptr_t)0xf) != 0u)
        return JE_ARM_FRAME_BAD;

    /*
     * This dereference happens only after the selected context's owned stack
     * range has been proven to contain the complete frame.  A cross-stack
     * pointer swap therefore fails before C reads attacker-controlled memory.
     */
    frame = (je_arm_trapframe *)opaque;

    /* Shared mode discriminator: vector group and SPSR.M must agree. */
    const uint64_t mode = frame->spsr & JE_ARM_SPSR_M_MASK;
    const bool vector_el1 = frame->vector >= JE_ARM_VECTOR_EL1_FIRST &&
                            frame->vector <= JE_ARM_VECTOR_EL1_LAST;
    const bool vector_el0 = frame->vector >= JE_ARM_VECTOR_EL0_FIRST &&
                            frame->vector <= JE_ARM_VECTOR_EL0_LAST;
    if ((frame->spsr & (UINT64_C(1) << 4)) != 0u ||
        (vector_el1 && mode != JE_ARM_SPSR_M_EL1H) ||
        (vector_el0 && mode != JE_ARM_SPSR_M_EL0T) ||
        (!vector_el1 && !vector_el0))
        return JE_ARM_FRAME_BAD;

    if (require_cookie && phase != 0u) {
        const uint64_t cookie = frame->pad;
        if ((uint32_t)(cookie >> 32) != ctx->slot ||
            (uint32_t)cookie == 0u ||
            (uint32_t)cookie != je_arm_frame_generation[ctx->slot]) {
#ifdef JE_ARM_SCHED_TEST
            je_arm_sched_generation_fault_slot = ctx->slot;
#endif
            return JE_ARM_FRAME_BAD;
        }
    }

    if (mode == JE_ARM_SPSR_M_EL0T) {
#ifdef JE_ARM_EL0_TEST
        if (!je_arm_user_slot[ctx->slot] ||
            je_arm_user_sp_el1[ctx->slot] == 0u ||
            frame_address != top - size ||
            frame_end != (uintptr_t)je_arm_user_sp_el1[ctx->slot])
            return JE_ARM_FRAME_BAD;
        /* EL0 resumes with the fixed EL1 policy and M=EL0t. This is a
         * normalization, not a user-controlled privilege decision. */
        frame->spsr = (frame->spsr & JE_ARM_SPSR_NZCV) |
                      JE_ARM_SPSR_EL0_POLICY;
        const uint64_t user_sp = *je_arm_tf_user_sp(frame);
        const uint64_t arena = JE_VM_ARENA_BASE;
        const uint64_t arena_end = arena + JE_VM_ARENA_SIZE;
        if (phase == 0u) {
            if (frame->elr != arena + je_arm_user_entry[ctx->slot] ||
                user_sp != arm_user_expected_sp(ctx->slot))
                return JE_ARM_FRAME_BAD;
            for (unsigned i = 0u; i < 31u; ++i)
                if (frame->gpr[i] != 0u) return JE_ARM_FRAME_BAD;
        } else if ((frame->elr & UINT64_C(3)) != 0u ||
                   frame->elr < arena || frame->elr >= arena_end ||
                   (user_sp & UINT64_C(0xf)) != 0u ||
                   user_sp <= arena || user_sp > arena_end) {
            return JE_ARM_FRAME_USER_DEATH;
        }
        return JE_ARM_FRAME_OK;
#else
        return JE_ARM_FRAME_BAD;
#endif
    }

    /* EL1h-only policy: this is the MP3a-1 behavior, including the exact
     * trampoline/gpr[0] phase-0 gate. */
    const uint64_t kernel_sp = *je_arm_tf_kernel_sp(frame);
    if (phase == 0u && frame_address != top - size)
        return JE_ARM_FRAME_BAD;
    if (kernel_sp != (uint64_t)frame_end ||
        (kernel_sp & UINT64_C(0xf)) != 0u || kernel_sp > (uint64_t)top)
        return JE_ARM_FRAME_BAD;
    if ((frame->spsr & ~JE_ARM_SPSR_NZCV) != JE_ARM_SPSR_POLICY)
        return JE_ARM_FRAME_BAD;
    if ((frame->elr & UINT64_C(3)) != 0u ||
        frame->elr < (uint64_t)text_start ||
        frame->elr >= (uint64_t)text_end)
        return JE_ARM_FRAME_BAD;
    if (phase == 0u &&
        (ctx->slot == 0u ||
         frame->elr != (uint64_t)(uintptr_t)arm_sched_trampoline ||
         frame->gpr[0] != (uint64_t)ctx->slot))
        return JE_ARM_FRAME_BAD;
#ifdef JE_ARM_SCHED_TEST
    if (phase == 1u && ctx->slot != 0u &&
        ctx->slot <= JE_ARM_MAX_PROOF_WORKERS &&
        !sched_test_worker_frame_elr(ctx->slot, frame->elr))
        return JE_ARM_FRAME_BAD;
#endif
    return JE_ARM_FRAME_OK;
}

static bool frame_shape_valid(const je_rr_ctx *ctx, const void *opaque,
                              unsigned phase, bool exact_record,
                              bool require_cookie)
{
    return frame_shape_class(ctx, (void *)opaque, phase, exact_record,
                             require_cookie) == JE_ARM_FRAME_OK;
}

/*
 * The final gate is intentionally separate from the RR hook call.  The core
 * has already published `next` as RUNNING when it returns, so this check uses
 * the exact local `ret`, the recorded frame pointer, and the pre-switch state
 * captured by arm_switch_target().
 */
__attribute__((unused))
static bool arm_frame_validate(const je_rr_ctx *ctx, const void *ret,
                               unsigned phase, bool was_running)
{
    if (!frame_shape_valid(ctx, ret, phase, true, true)) return false;
    if (was_running || (ctx->slot != 0u && ctx->state != JE_RR_RUNNING))
        return false;
    return true;
}

static void arm_idle_wait(void)
{
    /*
     * je_rr_pick() is called from the returning IRQ handler.  Waiting here
     * would unmask PSTATE.I while je_arm_in_handler is still set and the next
     * timer IRQ would take the double-fault path.  The ARM seam selects the
     * registered slot-0 boot frame after this hook returns; eret then resumes
     * the boot WFI loop with the normal exception-return mask.
     */
}

extern void je_arm_worker_entry(void *arg);

static _Noreturn void arm_sched_trampoline(void *arg)
{
    const uint32_t slot = (uint32_t)(uintptr_t)arg;
    if (!je_arm_rr_ready || slot == 0u || slot >= JE_RR_MAX ||
        je_rr_current(&je_arm_rr) != slot)
        sched_fatal("trampoline arg!=current");
#ifdef JE_ARM_SCHED_TEST
    static volatile uint32_t trampoline_starts[JE_RR_MAX];
    ++trampoline_starts[slot];
    if (trampoline_starts[slot] != 1u)
        sched_fatal("starts==1");
#endif
    je_arm_worker_entry(arg);
    sched_fatal("trampoline returned");
}

#ifndef JE_ARM_SCHED_TEST
void je_arm_worker_entry(void *arg)
{
    (void)arg;
    sched_fatal("scheduler disabled");
}
#endif

static je_status arm_stack_alloc(je_rr_ctx *ctx)
{
    je_pages *pages;
    uint64_t run_base = 0u;
    uint64_t top = 0u;
    je_status status;

    if (ctx == NULL || ctx->slot == 0u) return JE_ERR_INVALID;
#ifdef JE_ARM_EL0_TEST
    if (je_arm_pending_user) {
        if (!je_arm_pending_vm || je_arm_pending_index >= JE_VM_MAX_PROCESSES ||
            je_arm_pending_vm->procs[je_arm_pending_index].state != JE_VM_READY)
            return JE_ERR_INVALID;
        run_base = je_arm_pending_vm->procs[je_arm_pending_index]
                       .kstack_physical;
        status = je_arm_mmu_stack_map(&je_arm_mmu_context, run_base, &top);
        if (status != JE_OK) return status;
        je_arm_user_slot[ctx->slot] = true;
        je_arm_user_vm[ctx->slot] = je_arm_pending_vm;
        je_arm_user_index[ctx->slot] = je_arm_pending_index;
        je_arm_user_proc[ctx->slot] = je_arm_pending_proc;
        je_arm_user_entry[ctx->slot] =
            je_arm_pending_vm->procs[je_arm_pending_index].entry;
        je_arm_user_root[ctx->slot] =
            je_arm_pending_vm->procs[je_arm_pending_index].root;
        je_arm_user_sp_el1[ctx->slot] = top;
        ctx->usable_base = (uintptr_t)(run_base + JE_ARM_PAGE_SIZE);
        ctx->top = (uintptr_t)top;
        je_arm_stack_mapped[ctx->slot] = true;
        je_arm_pending_user = false;
#ifdef JE_ARM_SCHED_TEST
        je_arm_sched_stack_base[ctx->slot] = ctx->usable_base;
        je_arm_sched_stack_top[ctx->slot] = ctx->top;
#endif
        return JE_OK;
    }
#endif
    pages = je_arm_scheduler_pages();
    if (pages == NULL) return JE_ERR_INVALID;
    status = je_pages_alloc(pages, JE_ARM_STACK_RUN_PAGES, &run_base);
    if (status != JE_OK) return status;
    status = je_arm_mmu_stack_map(&je_arm_mmu_context, run_base, &top);
    if (status != JE_OK) {
        (void)je_pages_free(pages, run_base, JE_ARM_STACK_RUN_PAGES);
        return status;
    }
    ctx->usable_base = (uintptr_t)(run_base + JE_ARM_PAGE_SIZE);
    ctx->top = (uintptr_t)top;
    je_arm_stack_mapped[ctx->slot] = true;
#ifdef JE_ARM_SCHED_TEST
    je_arm_sched_stack_base[ctx->slot] = ctx->usable_base;
    je_arm_sched_stack_top[ctx->slot] = ctx->top;
#endif
    return JE_OK;
}

static void arm_stack_free(je_rr_ctx *ctx)
{
#ifdef JE_ARM_EL0_TEST
    if (ctx != NULL && ctx->slot < JE_RR_MAX &&
        je_arm_user_slot[ctx->slot]) {
        const uint64_t run_base =
            (uint64_t)(ctx->usable_base - JE_ARM_PAGE_SIZE);
        if (je_arm_mmu_stack_unmap(&je_arm_mmu_context, run_base) != JE_OK)
            sched_fatal("user-stack-unmap");
        je_arm_user_root[ctx->slot] = 0u;
        je_arm_user_sp_el1[ctx->slot] = 0u;
        je_arm_user_vm[ctx->slot] = NULL;
        je_arm_user_index[ctx->slot] = 0u;
        je_arm_user_proc[ctx->slot] = JE_PROC_ID_NONE;
        je_arm_user_entry[ctx->slot] = 0u;
        je_arm_user_slot[ctx->slot] = false;
        je_arm_stack_mapped[ctx->slot] = false;
        ctx->usable_base = 0u;
        ctx->top = 0u;
#ifdef JE_ARM_SCHED_TEST
        je_arm_sched_stack_base[ctx->slot] = 0u;
        je_arm_sched_stack_top[ctx->slot] = 0u;
#endif
        return;
    }
#endif
    je_pages *pages;
    uint64_t run_base;
    je_status status;

    if (ctx == NULL || ctx->usable_base < JE_ARM_PAGE_SIZE)
        sched_fatal("stack-free bounds");
    pages = je_arm_scheduler_pages();
    if (pages == NULL) sched_fatal("stack-free pages");
    run_base = (uint64_t)(ctx->usable_base - JE_ARM_PAGE_SIZE);
    status = je_arm_mmu_stack_unmap(&je_arm_mmu_context, run_base);
    if (status != JE_OK) sched_fatal("stack-unmap");
    status = je_pages_free(pages, run_base, JE_ARM_STACK_RUN_PAGES);
    if (status != JE_OK) sched_fatal("stack-free");
    je_arm_stack_mapped[ctx->slot] = false;
    ctx->usable_base = 0u;
    ctx->top = 0u;
#ifdef JE_ARM_SCHED_TEST
    je_arm_sched_stack_base[ctx->slot] = 0u;
    je_arm_sched_stack_top[ctx->slot] = 0u;
#endif
}

static je_status arm_frame_init(je_rr_ctx *ctx, void (*entry)(void *),
                                void *arg, void **out_frame)
{
    uintptr_t frame_address;
    je_arm_trapframe *frame;

    (void)entry;
    if (ctx == NULL || out_frame == NULL || ctx->slot == 0u ||
        ctx->top < ctx->usable_base ||
        ctx->top - ctx->usable_base < (uintptr_t)JE_TF_SIZE)
        return JE_ERR_INVALID;
    frame_address = ctx->top - (uintptr_t)JE_TF_SIZE;
    if ((frame_address & (uintptr_t)0xf) != 0u) return JE_ERR_INVALID;
    frame = (je_arm_trapframe *)frame_address;
    for (unsigned i = 0u; i < JE_TF_SIZE / sizeof(uint64_t); ++i)
        ((uint64_t *)frame)[i] = 0u;
    frame->gpr[0] = (uint64_t)(uintptr_t)arg;
    frame->elr = (uint64_t)(uintptr_t)arm_sched_trampoline;
    frame->spsr = JE_ARM_SPSR_POLICY;
    *je_arm_tf_kernel_sp(frame) = (uint64_t)ctx->top;
    frame->vector = JE_ARM_VECTOR_EL1_FIRST + 1u;
#ifdef JE_ARM_EL0_TEST
    if (je_arm_user_slot[ctx->slot]) {
        frame->gpr[0] = 0u;
        frame->elr = JE_VM_ARENA_BASE + je_arm_user_entry[ctx->slot];
        frame->spsr = JE_ARM_SPSR_EL0_POLICY;
        *je_arm_tf_user_sp(frame) = arm_user_expected_sp(ctx->slot);
        frame->vector = JE_ARM_VECTOR_EL0_FIRST;
    }
#endif
    frame->pad = 0u;
    *out_frame = frame;
    return JE_OK;
}

static je_status arm_frame_check(const je_rr_ctx *ctx, void *frame,
                                 unsigned phase)
{
    return frame_shape_valid(ctx, frame, phase, true, true) ? JE_OK : JE_ERR_INVALID;
}

#ifdef JE_ARM_EL0_TEST
static bool arm_user_harden(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(value));
    value &= ~(UINT64_C(3) << 20); /* CPACR_EL1.FPEN=00: no EL0 FP/SIMD. */
    __asm__ volatile("msr cpacr_el1, %0\n\tisb" : : "r"(value) : "memory");
    const uint64_t mdscr = UINT64_C(1) << 12; /* MDSCR_EL1.TDCC */
    const uint64_t cntkctl = UINT64_C(0);
    const uint64_t cntkctl_forbidden = UINT64_C(0x303);
    __asm__ volatile("msr cntkctl_el1, %1\n\t"
                     "msr pmuserenr_el0, xzr\n\t"
                     "msr mdscr_el1, %0\n\t"
                     "msr tpidr_el0, xzr\n\t"
                     "msr tpidrro_el0, xzr\n\t"
                     "isb" : : "r"(mdscr), "r"(cntkctl) : "memory");

    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(value));
    if ((value & (UINT64_C(3) << 20)) != 0u) return false;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(value));
    if ((value & cntkctl_forbidden) != 0u) return false;
    __asm__ volatile("mrs %0, pmuserenr_el0" : "=r"(value));
    if (value != 0u) return false;
    __asm__ volatile("mrs %0, mdscr_el1" : "=r"(value));
    if ((value & (UINT64_C(1) << 12)) == 0u) return false;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(value));
    /*
     * UCI, UCT, DZE and UMA are the EL0-facing SCTLR controls in this target
     * profile. MP1 owns their initial value; this path refuses if read-back
     * shows any of them enabled rather than weakening boot.S.
     */
    const uint64_t el0_sctlr =
        (UINT64_C(1) << 26) | (UINT64_C(1) << 15) |
        (UINT64_C(1) << 14) | (UINT64_C(1) << 9);
    return (value & el0_sctlr) == 0u;
}
#endif

void je_arm_sched_install_ttbr0(uint64_t root_pa, uint16_t asid)
{
    if (root_pa == 0u || (root_pa & (JE_ARM_PAGE_SIZE - 1u)) != 0u ||
        asid > UINT16_C(0xff))
        sched_fatal("ttbr0 root");

    const uint64_t asid_operand = (uint64_t)asid << 48;
    const uint64_t ttbr0 = root_pa | asid_operand;
    /*
     * DDI0487: TTBR0_EL1 selects the next stage-1 root; ASIDE1IS invalidates
     * EL1&0 entries for this ASID in the Inner Shareable domain. The DSB/ISB
     * completion sequence is part of the reviewed contract. QEMU TCG does not
     * model stale-TLB behavior, so this ordering is reviewed, not observed.
     */
    __asm__ volatile("msr ttbr0_el1, %0\n\t"
                     "isb\n\t"
                     "tlbi aside1is, %1\n\t"
                     "dsb ish\n\t"
                     "isb\n\t"
                     : : "r"(ttbr0), "r"(asid_operand) : "memory");
    ++je_arm_tlbi_hosted_calls;
    je_arm_tlbi_hosted_last_operand = asid_operand;
}

uint64_t je_arm_sched_read_ttbr0(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(value));
    return value;
}

static je_status arm_dispatch_install(je_rr_ctx *prev, je_rr_ctx *next)
{
    (void)prev;
    if (next == NULL || next->slot >= JE_RR_MAX) return JE_ERR_INVALID;
#ifdef JE_ARM_EL0_TEST
    if (je_arm_user_slot[next->slot]) {
        if (je_arm_user_root[next->slot] == 0u ||
            !arm_user_harden())
            return JE_ERR_DENIED;
        je_arm_sched_install_ttbr0(je_arm_user_root[next->slot], 1u);
        return JE_OK;
    }
#endif
    je_arm_sched_install_ttbr0(je_arm_mmu_root(&je_arm_mmu_context), 0u);
    return JE_OK;
}

static const je_rr_hooks je_arm_hooks = {
    .frame_init = arm_frame_init,
    .frame_check = arm_frame_check,
    .dispatch_install = arm_dispatch_install,
    .stack_alloc = arm_stack_alloc,
    .stack_free = arm_stack_free,
    .idle_wait = arm_idle_wait
};

void je_arm_sched_init(void)
{
    if (je_arm_rr_ready) return;
    je_rr_init(&je_arm_rr, &je_arm_hooks);
    je_arm_rr.slots[0].usable_base = (uintptr_t)__stack_bottom;
    je_arm_rr.slots[0].top = (uintptr_t)__stack_top;
    je_arm_stack_mapped[0] = true;
#ifdef JE_ARM_SCHED_TEST
    je_arm_sched_stack_base[0] = (uintptr_t)__stack_bottom;
    je_arm_sched_stack_top[0] = (uintptr_t)__stack_top;
#endif
    je_arm_rr_ready = true;
    je_arm_sched_current_identity = 0u;
}

#ifdef JE_ARM_EL0_TEST
void je_arm_sched_set_proc_pool(je_proc_pool *pool)
{
    je_arm_proc_pool = pool;
}

je_status je_arm_sched_create_user(je_vm *vm, uint32_t index,
                                   je_proc_id proc_id, uint32_t *out_slot)
{
    if (!vm || !je_arm_proc_pool || !out_slot ||
        index >= JE_VM_MAX_PROCESSES ||
        vm->procs[index].state != JE_VM_READY ||
        !je_proc_valid(je_arm_proc_pool, proc_id))
        return JE_ERR_INVALID;
    if (je_arm_pending_user) return JE_ERR_BUSY;
    je_arm_pending_user = true;
    je_arm_pending_vm = vm;
    je_arm_pending_index = index;
    je_arm_pending_proc = proc_id;
    const je_status status = je_rr_create(&je_arm_rr, arm_sched_trampoline,
                                          (void *)(uintptr_t)0u, out_slot);
    if (status != JE_OK) {
        je_arm_pending_user = false;
        je_arm_pending_vm = NULL;
        je_arm_pending_proc = JE_PROC_ID_NONE;
        return status;
    }
    const je_status link = je_proc_link(je_arm_proc_pool, proc_id, *out_slot,
                                        index);
    if (link != JE_OK) {
        je_rr_exit(&je_arm_rr, *out_slot);
        return link;
    }
    if (je_sched_register_process(vm, index, proc_id) != JE_OK)
        sched_fatal("register user");
    return JE_OK;
}

je_status je_arm_sched_current_user(je_vm **out_vm, uint32_t *out_index,
                                    uint32_t *out_slot)
{
    const uint32_t slot = je_rr_current(&je_arm_rr);
    if (!out_vm || !out_index || !out_slot || slot >= JE_RR_MAX ||
        !je_arm_user_slot[slot])
        return JE_ERR_INVALID;
    *out_vm = je_arm_user_vm[slot];
    *out_index = je_arm_user_index[slot];
    *out_slot = slot;
    return JE_OK;
}

uint32_t je_arm_el0_test_mode_rejection(uint32_t slot)
{
    if (slot == 0u || slot >= JE_RR_MAX || !je_arm_user_slot[slot])
        return 0u;
    je_rr_ctx *ctx = &je_arm_rr.slots[slot];
    je_arm_trapframe *frame = (je_arm_trapframe *)ctx->frame;
    if (frame == NULL) return 0u;

    const uint64_t saved_elr = frame->elr;
    const uint64_t saved_spsr = frame->spsr;
    const uint64_t saved_sp = *je_arm_tf_user_sp(frame);
    const uint64_t saved_gpr0 = frame->gpr[0];
    const uint64_t saved_vector = frame->vector;
    uint32_t passed = 0u;

    /*
     * Branch-isolation preflight for the mode mutant: the real EL0t branch
     * must classify this malformed user return as BAD.  If the branch is
     * inverted, it reaches the EL1h SP accessor with M=EL0t and fails closed
     * at the mode boundary instead of being hidden by the aggregate marker.
     */
    frame->elr = JE_VM_ARENA_BASE + je_arm_user_entry[slot];
    frame->spsr = JE_ARM_SPSR_EL0_POLICY;
    *je_arm_tf_user_sp(frame) = JE_VM_ARENA_BASE;
    frame->vector = JE_ARM_VECTOR_EL0_FIRST;
    if (frame_shape_class(ctx, frame, 0u, false, false) !=
        JE_ARM_FRAME_BAD)
        sched_fatal("mode probe");

    /*
     * The first case is also the load-bearing negative for the mode mutant:
     * the bad EL1h frame would be accepted by an inverted EL0 branch only if
     * its SP field has the valid EL0 phase-0 value.
     */
    frame->elr = JE_VM_ARENA_BASE + je_arm_user_entry[slot];
    frame->spsr = JE_ARM_SPSR_POLICY;
    *je_arm_tf_kernel_sp(frame) = arm_user_expected_sp(slot);
    frame->gpr[0] = 0u;
    frame->vector = JE_ARM_VECTOR_EL1_FIRST;
    je_arm_frame_result result =
        frame_shape_class(ctx, frame, 0u, false, false);
    if (result == JE_ARM_FRAME_BAD) passed |= UINT32_C(1);

    /*
     * Reset the synthetic frame before case 2.  This is an otherwise-valid
     * EL1h frame whose only deliberate defect is the lower-EL vector tag.
     * Without the vector_el0/M agreement check, the EL1h branch would accept
     * it; stale SP/ELR fields must not reject it first.
     */
    frame->elr = (uint64_t)(uintptr_t)arm_sched_trampoline;
    frame->spsr = JE_ARM_SPSR_POLICY;
    *je_arm_tf_kernel_sp(frame) = (uint64_t)ctx->top;
    frame->gpr[0] = (uint64_t)slot;
    frame->vector = JE_ARM_VECTOR_EL0_FIRST + 1u;
    result = frame_shape_class(ctx, frame, 0u, false, false);
    if (result == JE_ARM_FRAME_BAD) passed |= UINT32_C(2);

    /*
     * Reset the synthetic frame before case 3.  This is an otherwise-valid
     * EL0t frame whose only deliberate defect is the EL1 vector tag.
     */
    frame->elr = JE_VM_ARENA_BASE + je_arm_user_entry[slot];
    frame->spsr = JE_ARM_SPSR_EL0_POLICY;
    *je_arm_tf_user_sp(frame) = arm_user_expected_sp(slot);
    frame->vector = JE_ARM_VECTOR_EL1_FIRST + 1u;
    result = frame_shape_class(ctx, frame, 0u, false, false);
    if (result == JE_ARM_FRAME_BAD) passed |= UINT32_C(4);

    /*
     * M[4] remains a composite negative: its rejection is covered by the
     * shared AArch64 mode/vector checks, not by a dedicated isolating probe.
     */
    frame->spsr = JE_ARM_SPSR_EL0_POLICY | (UINT64_C(1) << 4);
    frame->vector = JE_ARM_VECTOR_EL0_FIRST;
    result = frame_shape_class(ctx, frame, 0u, false, false);
    if (result == JE_ARM_FRAME_BAD) passed |= UINT32_C(8);

    frame->elr = saved_elr;
    frame->spsr = saved_spsr;
    *je_arm_tf_user_sp(frame) = saved_sp;
    frame->gpr[0] = saved_gpr0;
    frame->vector = saved_vector;
    return passed;
}

extern _Noreturn void je_arm_enter_frame(void *frame);
#endif

static je_arm_frame_result record_current_frame(je_rr_ctx *current,
                                                je_arm_trapframe *tf)
{
    uint32_t generation;
    je_arm_frame_result result;

    if (current == NULL || tf == NULL) sched_fatal("current-frame null");
    if (current->frame != NULL) {
        /*
         * A running context must have consumed its saved frame.  The IRQ
         * saver has already written zero to the cookie field at this same
         * address, so the drop-consume proof mutant is caught here by the
         * cookie-zeroed-by-saver check, not by the resume-path generation
         * comparison.
         */
        if (!frame_shape_valid(current, current->frame, current->phase,
                               true, true)) {
#ifdef JE_ARM_SCHED_TEST
            if (((const je_arm_trapframe *)current->frame)->pad == 0u)
                sched_fatal("frame cookie zeroed by IRQ saver");
#endif
            sched_fatal("frame generation");
        }
        sched_fatal("frame not consumed");
    }
    result = frame_shape_class(current, tf, current->phase, false, false);
    if (result == JE_ARM_FRAME_BAD)
        sched_fatal("observed_running");
    if (result == JE_ARM_FRAME_USER_DEATH) return result;
    generation = next_frame_generation(je_arm_frame_generation[current->slot]);
    je_arm_frame_generation[current->slot] = generation;
    current->frame = tf;
    tf->pad = ((uint64_t)current->slot << 32) | generation;
    return JE_ARM_FRAME_OK;
}

static void no_switch_frame(je_rr_ctx *current, je_arm_trapframe *tf)
{
    if (current == NULL || tf == NULL ||
        !frame_shape_valid(current, tf, current->phase, false, false))
        sched_fatal("no-switch validation");
    current->frame = NULL;
}

#ifdef JE_ARM_EL0_TEST
static bool arm_current_is_user(uint32_t slot)
{
    return slot < JE_RR_MAX && je_arm_user_slot[slot];
}

static void arm_user_mark_dying(uint32_t slot, uint32_t reason,
                                uint64_t code, uint64_t vector)
{
    if (!arm_current_is_user(slot) || !je_arm_proc_pool)
        sched_fatal("user death binding");
    const je_status status = je_lc_on_dying(
        slot, je_arm_user_proc[slot], reason, code, vector);
    if (status != JE_OK) sched_fatal("lifecycle dying");
    je_rr_exit(&je_arm_rr, slot);
}

static void arm_reap_zombies(void)
{
    const uint32_t current = je_rr_current(&je_arm_rr);
    for (uint32_t slot = 1u; slot < JE_RR_MAX; ++slot) {
        if (!je_arm_user_slot[slot] ||
            je_arm_rr.slots[slot].state != JE_RR_ZOMBIE ||
            slot == current)
            continue;
        je_vm *vm = je_arm_user_vm[slot];
        const uint32_t index = je_arm_user_index[slot];
        const je_proc_id proc_id = je_arm_user_proc[slot];
        if (je_rr_reap(&je_arm_rr, slot, current) != JE_OK)
            sched_fatal("rr reap");
        if (je_sched_unregister_process(vm, index) != JE_OK ||
            je_lc_reaped(vm, index, proc_id) != JE_OK)
            sched_fatal("lifecycle reap");
        je_arm_el0_test_note_reaped(index, proc_id);
    }
}

static void *arm_switch_to_ready(void)
{
    const uint32_t current = je_rr_current(&je_arm_rr);
    if (current != 0u && current < JE_RR_MAX &&
        je_arm_rr.slots[current].state == JE_RR_RUNNING)
        je_arm_rr.slots[current].state = JE_RR_READY;
    for (uint32_t attempt = 0u; attempt < JE_RR_MAX; ++attempt) {
        const uint32_t next = je_rr_pick(&je_arm_rr);
        if (next == 0u) {
            sched_fatal("no live EL0 successor");
        }
        void *resume = NULL;
        const je_status status = je_rr_switch(&je_arm_rr, next, &resume);
        if (status == JE_OK && resume != NULL) {
            je_arm_rr.slots[next].frame = NULL;
            je_arm_frame_generation[next] =
                next_frame_generation(je_arm_frame_generation[next]);
            je_arm_sched_current_identity = next;
            if (arm_current_is_user(next))
                je_arm_el0_test_poison_tpidr();
            return resume;
        }
        if (arm_current_is_user(next) &&
            frame_shape_class(&je_arm_rr.slots[next],
                              je_arm_rr.slots[next].frame,
                              je_arm_rr.slots[next].phase, true, true) ==
                JE_ARM_FRAME_USER_DEATH) {
            je_arm_el0_test_note_fault(next, JE_ABI_DEATH_BAD_RETURN,
                                       0u, 0u, 0u, 0u, 0u, 0u);
            sched_console_write("JeiOS/aarch64: EL0 TERMINATED slot=");
            sched_console_dec(next);
            sched_console_write(" reason=");
            sched_console_dec(JE_ABI_DEATH_BAD_RETURN);
            sched_console_write(" ec=0x0000000000000000 far=0x0000000000000000"
                                " il=0 iss=0x0000000000000000"
                                " fsc=0x0000000000000000 wnr=0\r\n");
            arm_user_mark_dying(next, JE_ABI_DEATH_BAD_RETURN, 0u,
                                JE_ARM_VECTOR_EL0_FIRST);
            continue;
        }
        if (status == JE_ERR_DENIED)
            sched_fatal("hardening refusal");
        sched_fatal("frame invalid");
    }
    sched_fatal("successor bound");
}

static void *arm_user_death(je_arm_trapframe *tf, uint32_t reason,
                            uint64_t code)
{
    const uint32_t slot = je_rr_current(&je_arm_rr);
    if (!arm_current_is_user(slot) || tf == NULL)
        sched_fatal("fault from non-user");
    const uint64_t vector = tf->vector;
    const uint64_t esr = tf->esr;
    const uint64_t ec = (esr >> 26) & UINT64_C(0x3f);
    const uint64_t il = (esr >> 25) & UINT64_C(1);
    const uint64_t iss = esr & UINT64_C(0x01ffffff);
    const uint64_t fsc = iss & UINT64_C(0x3f);
    const uint64_t wnr = (iss >> 6) & UINT64_C(1);
    je_arm_el0_test_note_fault(slot, reason, ec, tf->far, il, iss, fsc, wnr);
    sched_console_write("JeiOS/aarch64: EL0 TERMINATED slot=");
    sched_console_dec(slot);
    sched_console_write(" reason=");
    sched_console_dec(reason);
    sched_console_write(" ec=");
    sched_console_hex(ec);
    sched_console_write(" far=");
    sched_console_hex(tf->far);
    sched_console_write(" il=");
    sched_console_dec(il);
    sched_console_write(" iss=");
    sched_console_hex(iss);
    sched_console_write(" fsc=");
    sched_console_hex(fsc);
    sched_console_write(" wnr=");
    sched_console_dec(wnr);
    sched_console_write("\r\n");
    arm_user_mark_dying(slot, reason, code, vector);
    return arm_switch_to_ready();
}

void *je_arm_el0_fault(je_arm_trapframe *tf)
{
    if (tf == NULL) sched_fatal("null EL0 fault");
    const je_arm_frame_result shape =
        frame_shape_class(&je_arm_rr.slots[je_rr_current(&je_arm_rr)],
                          tf, 1u, false, false);
    if (shape == JE_ARM_FRAME_USER_DEATH &&
        arm_current_is_user(je_rr_current(&je_arm_rr))) {
        return arm_user_death(tf, JE_ABI_DEATH_FAULT, 0u);
    }
    if (shape != JE_ARM_FRAME_OK) sched_fatal("EL0 fault frame");
    return arm_user_death(tf, JE_ABI_DEATH_FAULT, 0u);
}

void *je_arm_el0_svc(je_arm_trapframe *tf)
{
    if (tf == NULL || !arm_current_is_user(je_rr_current(&je_arm_rr)))
        sched_fatal("SVC from non-user");
    /*
     * Reap only after the restore seam has put execution on the successor's
     * SP_EL1. Reaping from arm_switch_to_ready() would unmap the old current
     * kernel stack while this C call is still using it.
     */
    arm_reap_zombies();
    const je_arm_frame_result shape =
        frame_shape_class(&je_arm_rr.slots[je_rr_current(&je_arm_rr)],
                          tf, 1u, false, false);
    if (shape == JE_ARM_FRAME_BAD)
        sched_fatal("EL0 SVC frame invariant");
    if (shape == JE_ARM_FRAME_USER_DEATH)
        return arm_user_death(tf, JE_ABI_DEATH_BAD_RETURN, 0u);

    const uint32_t svc_slot = je_rr_current(&je_arm_rr);
    je_arm_el0_test_note_kstack(svc_slot);
    const uint64_t esr = tf->esr;
    if (((esr >> 26) & UINT64_C(0x3f)) != UINT64_C(0x15) ||
        ((esr >> 25) & UINT64_C(1)) == 0u ||
        (esr & UINT64_C(0xffff)) != 0u)
        return arm_user_death(tf, JE_ABI_DEATH_FAULT, 0u);

    uint64_t args[6];
    for (unsigned i = 0u; i < 6u; ++i) args[i] = tf->gpr[i];
    const uint64_t op = tf->gpr[8];
    je_abi_status abi_status = JE_ABI_UNSUPPORTED;
    uint64_t result = 0u;
    je_syscall_action action;
#ifdef JE_ARM_EL0_TEST
    if (op == JE_ARM_EL0_PROOF_OP) {
        action = JE_SYSCALL_RETURN;
        if (args[0] == JE_ARM_EL0_PROOF_ARG0) {
            abi_status = JE_ARM_EL0_PROOF_STATUS;
            result = JE_ARM_EL0_PROOF_RESULT;
        } else {
            abi_status = JE_ABI_INVALID;
            result = 0u;
        }
    } else
#endif
        action = je_syscall_decode(op, args, &abi_status, &result);
    if (action != JE_SYSCALL_RETURN && action != JE_SYSCALL_YIELD &&
        action != JE_SYSCALL_EXIT) {
        abi_status = JE_ABI_UNSUPPORTED;
        result = 0u;
    }
    tf->gpr[0] = (uint64_t)abi_status;
    tf->gpr[1] = result;
    if (op == JE_ARM_EL0_PROOF_OP)
        je_arm_el0_test_note_svc_proof(svc_slot, tf);
    je_arm_el0_test_note_svc(je_rr_current(&je_arm_rr), op, tf);

    if (action == JE_SYSCALL_EXIT && abi_status == JE_ABI_OK)
        return arm_user_death(tf, JE_ABI_DEATH_EXIT, result);
    if (action == JE_SYSCALL_YIELD && abi_status == JE_ABI_OK) {
        if (record_current_frame(&je_arm_rr.slots[je_rr_current(&je_arm_rr)],
                                 tf) != JE_ARM_FRAME_OK)
            sched_fatal("yield frame classification");
        je_arm_el0_test_before_return(je_rr_current(&je_arm_rr), tf);
        return arm_switch_to_ready();
    }
    je_rr_ctx *svc_current = &je_arm_rr.slots[je_rr_current(&je_arm_rr)];
    if (record_current_frame(svc_current, tf) != JE_ARM_FRAME_OK)
        sched_fatal("SVC frame classification");
    no_switch_frame(&je_arm_rr.slots[je_rr_current(&je_arm_rr)], tf);
    return tf;
}

_Noreturn void je_arm_sched_start_user(void)
{
    if (je_arm_rr.current != 0u || !je_arm_rr_ready)
        sched_fatal("user start state");
    void *resume = arm_switch_to_ready();
    je_arm_enter_frame(resume);
}
#endif

void *je_arm_sched_no_tick(je_arm_trapframe *tf)
{
    uint32_t current_slot;
    if (!je_arm_rr_ready) sched_fatal("scheduler uninitialized");
    current_slot = je_rr_current(&je_arm_rr);
    if (current_slot >= JE_RR_MAX) sched_fatal("current slot");
#ifdef JE_ARM_SCHED_TEST
    arm_sched_validate_sleeping();
#endif
    const je_arm_frame_result result =
        record_current_frame(&je_arm_rr.slots[current_slot], tf);
#ifdef JE_ARM_EL0_TEST
    if (result == JE_ARM_FRAME_USER_DEATH &&
        arm_current_is_user(current_slot))
        return arm_user_death(tf, JE_ABI_DEATH_BAD_RETURN, 0u);
#else
    (void)result;
#endif
    no_switch_frame(&je_arm_rr.slots[current_slot], tf);
    return tf;
}

#ifdef JE_ARM_SCHED_TEST
static void *je_arm_sched_test_irq(je_arm_trapframe *tf,
                                   uint32_t current_slot);
static void proof_ready_bound_step(void);
#endif

void *je_arm_sched_irq(je_arm_trapframe *tf)
{
    uint32_t current_slot;
    je_rr_ctx *current;

    if (!je_arm_rr_ready) sched_fatal("scheduler uninitialized");
    current_slot = je_rr_current(&je_arm_rr);
    if (current_slot >= JE_RR_MAX) sched_fatal("current slot");
    current = &je_arm_rr.slots[current_slot];
    const je_arm_frame_result result = record_current_frame(current, tf);
#ifdef JE_ARM_EL0_TEST
    if (result == JE_ARM_FRAME_USER_DEATH &&
        arm_current_is_user(current_slot))
        return arm_user_death(tf, JE_ABI_DEATH_BAD_RETURN, 0u);
#else
    (void)result;
#endif

#ifdef JE_ARM_SCHED_TEST
    /* The core tick owns wakeups and the scheduler tick count. */
    arm_sched_validate_sleeping();
    je_rr_tick(&je_arm_rr, je_arm_timer_ticks());
    for (uint32_t slot = 1u; slot <= JE_ARM_MAX_PROOF_WORKERS; ++slot)
        if (je_arm_rr.slots[slot].state != JE_RR_SLEEPING)
            je_arm_sched_idle_waiting[slot] = false;
    proof_idle_tick(current_slot);
    proof_ready_bound_step();
    return je_arm_sched_test_irq(tf, current_slot);
#else
#ifdef JE_ARM_EL0_TEST
    if (arm_current_is_user(current_slot)) {
        je_rr_tick(&je_arm_rr, je_arm_timer_ticks());
        void *resume = arm_switch_to_ready();
        je_arm_el0_test_note_irq_switch(current_slot,
                                        je_rr_current(&je_arm_rr),
                                        tf->elr);
        return resume;
    }
#endif
    arm_sched_validate_sleeping();
    no_switch_frame(current, tf);
    return tf;
#endif
}

#ifdef JE_ARM_SCHED_TEST

/*
 * The remainder is the ARM_SCHED_TEST proof.  It is intentionally kept out of
 * the default image: the production boot remains IRQ-masked and has no worker
 * creation or proof-only policy.
 */

static volatile uint64_t je_arm_sched_preempted_from[JE_RR_MAX];
static volatile uint64_t je_arm_sched_preempted_in_canary[JE_RR_MAX];
static volatile uint64_t je_arm_sched_phase_a_preempts[JE_RR_MAX];
static volatile uint64_t je_arm_sched_phase_b_preempts[JE_RR_MAX];
static volatile uint64_t je_arm_sched_verified_resumes[JE_RR_MAX];
static volatile uint64_t je_arm_sched_interleave[JE_RR_MAX];
static volatile uint64_t je_arm_sched_nzcv_window_preempts[JE_RR_MAX];
static volatile uint64_t je_arm_sched_dispatches_after_exit[JE_RR_MAX];
static volatile uint64_t je_arm_sched_observed_handler[JE_RR_MAX];
static volatile uint64_t je_arm_sched_preemptions;
static volatile uint64_t je_arm_sched_nonpreempt_switch_ins;
static uint64_t je_arm_sched_ready_age[JE_RR_MAX];
static uint32_t je_arm_sched_ready_limit[JE_RR_MAX];
static je_rr_state je_arm_sched_last_state[JE_RR_MAX];
static volatile uint32_t je_arm_sched_worker_slots[JE_ARM_MAX_PROOF_WORKERS];
static volatile bool je_arm_sched_monitor_done;
static volatile uint64_t je_arm_sched_proof_deadline;
static volatile bool je_arm_sched_mutation_announced;
static volatile uint64_t je_arm_sched_guard_target;
static uint32_t je_arm_sched_worker_count;
#ifdef JE_ARM_SCHED_MUTANT_REPLAY_STALE_FRAME
static je_arm_trapframe je_arm_sched_replay_frame
    __attribute__((aligned(16)));
static const void *je_arm_sched_replay_address;
static uint32_t je_arm_sched_replay_slot;
static bool je_arm_sched_replay_saved;
#endif

const uint64_t je_arm_nzcv_patterns[JE_RR_MAX][2] = {
    { 0u, 0u },
    { UINT64_C(0x90000000), UINT64_C(0xa0000000) },
    { UINT64_C(0x50000000), UINT64_C(0xc0000000) },
    { UINT64_C(0x30000000), UINT64_C(0xb0000000) },
    { UINT64_C(0x70000000), UINT64_C(0xd0000000) }
};

#define JE_ARM_RODATA_ROW(value) \
    { value, value, value, value, value, value, value, value, \
      value, value, value, value, value, value, value, value, \
      value, value, value, value, value, value, value, value, \
      value, value, value, value, value, value, value }
const uint64_t je_arm_sched_rodata_patterns[JE_RR_MAX][31] = {
    JE_ARM_RODATA_ROW(UINT64_C(0)),
    JE_ARM_RODATA_ROW(UINT64_C(0x1100000000000000)),
    JE_ARM_RODATA_ROW(UINT64_C(0x2200000000000000)),
    JE_ARM_RODATA_ROW(UINT64_C(0x3300000000000000)),
    JE_ARM_RODATA_ROW(UINT64_C(0x4400000000000000))
};
#undef JE_ARM_RODATA_ROW

static bool addr_between(uint64_t value, uint64_t first, uint64_t last)
{
    return value >= first && value < last;
}

static bool worker_slot(uint32_t slot)
{
    for (uint32_t i = 0u; i < je_arm_sched_worker_count; ++i)
        if (je_arm_sched_worker_slots[i] == slot) return true;
    return false;
}

__attribute__((unused))
static void print_mutation(const char *name)
{
    sched_console_write("JeiOS/aarch64: MUTATION FIRED ");
    sched_console_write(name);
    sched_console_write("\r\n");
}

static _Noreturn void proof_fatal(const char *reason, uint32_t slot)
{
    sched_console_write("JeiOS/aarch64: FATAL sched proof ");
    sched_console_write(reason);
    sched_console_write(" slot=");
    sched_console_dec(slot);
    sched_console_write("\r\n");
    sched_halt();
}

_Noreturn void je_arm_sched_canary_failure(uint32_t reg)
{
    sched_console_write("JeiOS/aarch64: FATAL sched canary reg=");
    sched_console_dec(reg);
    sched_console_write("\r\n");
    sched_halt();
}

static uint32_t nzcv_phase_for(uint32_t slot, uint64_t nzcv)
{
    const uint64_t actual = nzcv & JE_ARM_SPSR_NZCV;
    if (actual == je_arm_nzcv_patterns[slot][0]) return 0u;
    if (actual == je_arm_nzcv_patterns[slot][1]) return 1u;
    proof_fatal("nzcv-window", slot);
}

static bool in_worker_canary(uint64_t elr)
{
    extern uint8_t je_arm_worker_canary_start[];
    extern uint8_t je_arm_worker_canary_end[];
    return addr_between(elr, (uint64_t)(uintptr_t)je_arm_worker_canary_start,
                        (uint64_t)(uintptr_t)je_arm_worker_canary_end);
}

static bool in_nzcv_window(uint64_t elr)
{
    extern uint8_t je_arm_worker_low_nzcv_start[];
    extern uint8_t je_arm_worker_low_nzcv_end[];
    extern uint8_t je_arm_worker_high_nzcv_start[];
    extern uint8_t je_arm_worker_high_nzcv_end[];
    return (elr > (uint64_t)(uintptr_t)je_arm_worker_low_nzcv_start &&
            elr <= (uint64_t)(uintptr_t)je_arm_worker_low_nzcv_end) ||
           (elr > (uint64_t)(uintptr_t)je_arm_worker_high_nzcv_start &&
            elr <= (uint64_t)(uintptr_t)je_arm_worker_high_nzcv_end);
}

static bool in_worker_phase_a(uint64_t elr)
{
    extern uint8_t je_arm_worker_phase_a_start[];
    extern uint8_t je_arm_worker_phase_a_end[];
    return addr_between(elr, (uint64_t)(uintptr_t)je_arm_worker_phase_a_start,
                        (uint64_t)(uintptr_t)je_arm_worker_phase_a_end);
}

static bool in_worker_phase_b(uint64_t elr)
{
    extern uint8_t je_arm_worker_phase_b_start[];
    extern uint8_t je_arm_worker_phase_b_end[];
    return addr_between(elr, (uint64_t)(uintptr_t)je_arm_worker_phase_b_start,
                        (uint64_t)(uintptr_t)je_arm_worker_phase_b_end);
}

static void mutation_before_switch(je_arm_trapframe *tf)
{
    if (!je_arm_sched_mutation_point_reached) {
        sched_console_write("JeiOS/aarch64: MUTATION POINT REACHABLE\r\n");
        je_arm_sched_mutation_point_reached = true;
    }
#ifdef JE_ARM_SCHED_MUTANT_EARLY_FAKE_SWAP
    if (!je_arm_sched_mutation_announced) {
        print_mutation("early-fake-swap");
        je_arm_sched_mutation_announced = true;
        for (unsigned i = 0u; i < 31u; ++i) {
            uint64_t *left = (uint64_t *)je_arm_rr.slots[1].frame;
            uint64_t *right = (uint64_t *)je_arm_rr.slots[2].frame;
            const uint64_t temporary = left[i];
            left[i] = right[i];
            right[i] = temporary;
        }
    }
#endif

#ifdef JE_ARM_SCHED_MUTANT_LIVE_POINTER_SWAP
    if (!je_arm_sched_mutation_announced &&
        je_arm_sched_preempted_in_canary[1] != 0u &&
        je_arm_sched_preempted_in_canary[2] != 0u) {
        print_mutation("live-pointer-swap");
        je_arm_sched_mutation_announced = true;
        void *temporary = je_arm_rr.slots[1].frame;
        je_arm_rr.slots[1].frame = je_arm_rr.slots[2].frame;
        je_arm_rr.slots[2].frame = temporary;
    }
#endif

    (void)tf;
}

static void mutation_after_switch(je_arm_trapframe *ret, uint32_t slot,
                                  unsigned phase)
{
#ifdef JE_ARM_SCHED_MUTANT_REPLAY_STALE_FRAME
    if (phase == 1u && worker_slot(slot) && ret->pad != 0u) {
        if (!je_arm_sched_replay_saved) {
            for (unsigned i = 0u; i < JE_TF_SIZE / sizeof(uint64_t); ++i)
                ((uint64_t *)&je_arm_sched_replay_frame)[i] =
                    ((const uint64_t *)ret)[i];
            je_arm_sched_replay_address = ret;
            je_arm_sched_replay_slot = slot;
            je_arm_sched_replay_saved = true;
        } else if (!je_arm_sched_mutation_announced &&
                   slot == je_arm_sched_replay_slot &&
                   ret == je_arm_sched_replay_address) {
            for (unsigned i = 0u; i < JE_TF_SIZE / sizeof(uint64_t); ++i)
                ((uint64_t *)ret)[i] =
                    ((const uint64_t *)&je_arm_sched_replay_frame)[i];
            print_mutation("replay-stale-frame");
            je_arm_sched_mutation_announced = true;
        }
    }
#endif
#ifdef JE_ARM_SCHED_MUTANT_CORRUPT_SPSR_NZCV
    if (!je_arm_sched_mutation_announced && phase == 1u &&
        worker_slot(slot) && in_nzcv_window(ret->elr)) {
        print_mutation("corrupt-spsr-nzcv");
        je_arm_sched_mutation_announced = true;
        ret->spsr = (ret->spsr & ~JE_ARM_SPSR_NZCV) | UINT64_C(0x10000000);
    }
#endif
#ifdef JE_ARM_SCHED_MUTANT_SET_SPSR_I
    if (!je_arm_sched_mutation_announced) {
        print_mutation("spsr-i");
        je_arm_sched_mutation_announced = true;
        ret->spsr |= JE_ARM_DAIF_I;
    }
    (void)slot;
    (void)phase;
#else
    (void)ret;
    (void)slot;
    (void)phase;
#endif
#ifdef JE_ARM_SCHED_MUTANT_LIVE_CONTENT_SWAP
    if (!je_arm_sched_mutation_announced && phase == 1u &&
        worker_slot(slot) &&
        (in_worker_phase_a(ret->elr) || in_worker_phase_b(ret->elr))) {
        print_mutation("live-content-swap");
        je_arm_sched_mutation_announced = true;
        if (in_worker_phase_a(ret->elr))
            ret->gpr[0] ^= UINT64_C(1);
        else
            ret->gpr[26] ^= UINT64_C(1);
    }
#endif
}

static void proof_entry_check(uint32_t slot, je_arm_trapframe *tf)
{
    const je_rr_ctx *ctx = &je_arm_rr.slots[slot];
    if (!worker_slot(slot)) return;
    if (je_arm_rr.current != slot ||
        !frame_shape_valid(ctx, tf, ctx->phase, false, false))
        proof_fatal("observed_running", slot);
    ++je_arm_sched_observed_handler[slot];
    if (je_arm_sched_idle_active)
        ++je_arm_sched_idle_worker_resumes;
}

static void proof_preemption_counters(uint32_t slot, const je_arm_trapframe *tf,
                                     uint32_t next)
{
    static uint64_t last_checks[JE_RR_MAX];
    static bool saw_preemption[JE_RR_MAX];
    const bool canary = in_worker_canary(tf->elr);

    if (!worker_slot(slot)) return;
    if (canary) ++je_arm_sched_preempted_in_canary[slot];
    if (in_worker_phase_a(tf->elr))
        ++je_arm_sched_phase_a_preempts[slot];
    if (in_worker_phase_b(tf->elr))
        ++je_arm_sched_phase_b_preempts[slot];
    ++je_arm_sched_preempted_from[slot];
    if (saw_preemption[slot] &&
        je_arm_sched_canary_checks[slot] > last_checks[slot])
        ++je_arm_sched_verified_resumes[slot];
    saw_preemption[slot] = true;
    last_checks[slot] = je_arm_sched_canary_checks[slot];
    if (in_nzcv_window(tf->elr)) {
        const uint32_t phase = nzcv_phase_for(slot, tf->spsr);
        je_arm_sched_nzcv_phase[slot] = phase;
        ++je_arm_sched_nzcv_window_preempts[slot];
    }
    if (next != slot && next < JE_RR_MAX &&
        je_arm_rr.slots[next].state == JE_RR_READY)
        ++je_arm_sched_interleave[slot];
}

static bool proof_ready(uint32_t slot)
{
    const je_rr_counters *counts = je_rr_get_counters(&je_arm_rr, slot);
    return counts != NULL && counts->starts == 1u &&
           counts->dispatched_to >= 3u &&
           counts->resumed >= 1u &&
           counts->observed_running >= 1u &&
           je_arm_sched_preempted_in_canary[slot] >= 1u &&
           je_arm_sched_phase_a_preempts[slot] >= 1u &&
           je_arm_sched_phase_b_preempts[slot] >= 1u &&
           je_arm_sched_verified_resumes[slot] >= 1u &&
           je_arm_sched_interleave[slot] >= 1u &&
           je_arm_sched_nzcv_window_preempts[slot] >= 1u;
}

static bool all_proof_ready(void)
{
    for (uint32_t i = 0u; i < je_arm_sched_worker_count; ++i)
        if (!proof_ready(je_arm_sched_worker_slots[i])) return false;
    return true;
}

static void proof_ready_bound_step(void)
{
    uint32_t ready = 0u;
    if (je_arm_sched_idle_complete) return;
    for (uint32_t slot = 1u; slot < JE_RR_MAX; ++slot)
        if (je_arm_rr.slots[slot].state == JE_RR_READY) ++ready;
    for (uint32_t slot = 1u; slot < JE_RR_MAX; ++slot) {
        const je_rr_state state = je_arm_rr.slots[slot].state;
        if (state == JE_RR_READY) {
            if (je_arm_sched_last_state[slot] != JE_RR_READY) {
                je_arm_sched_ready_age[slot] = 0u;
                je_arm_sched_ready_limit[slot] = ready;
            }
            if (je_arm_sched_ready_age[slot] != UINT64_MAX)
                ++je_arm_sched_ready_age[slot];
            if (je_arm_sched_ready_limit[slot] != 0u &&
                je_arm_sched_ready_age[slot] >
                    (uint64_t)je_arm_sched_ready_limit[slot]) {
                proof_fatal("ready-bound", slot);
            }
        } else {
            je_arm_sched_ready_age[slot] = 0u;
            je_arm_sched_ready_limit[slot] = 0u;
        }
        je_arm_sched_last_state[slot] = state;
    }
}

static uint64_t proof_dispatched_sum(void)
{
    uint64_t total = 0u;
    for (uint32_t slot = 1u; slot <= JE_ARM_MONITOR_SLOT; ++slot) {
        const je_rr_counters *counts = je_rr_get_counters(&je_arm_rr, slot);
        if (counts != NULL && UINT64_MAX - total >= counts->dispatched_to)
            total += counts->dispatched_to;
    }
    return total;
}

static void print_counter(uint32_t slot)
{
    const je_rr_counters *counts = je_rr_get_counters(&je_arm_rr, slot);
    if (counts == NULL) proof_fatal("counter-null", slot);
    sched_console_write("JeiOS/aarch64: sched counter slot=");
    sched_console_dec(slot);
    sched_console_write(" starts=");
    sched_console_dec(counts->starts);
    sched_console_write(" dispatched_to=");
    sched_console_dec(counts->dispatched_to);
    sched_console_write(" resumed=");
    sched_console_dec(counts->resumed);
    sched_console_write(" observed_running=");
    sched_console_dec(counts->observed_running);
    sched_console_write(" preempted_from=");
    sched_console_dec(je_arm_sched_preempted_from[slot]);
    sched_console_write(" preempted_in_canary=");
    sched_console_dec(je_arm_sched_preempted_in_canary[slot]);
    sched_console_write(" phase_a_preempts=");
    sched_console_dec(je_arm_sched_phase_a_preempts[slot]);
    sched_console_write(" phase_b_preempts=");
    sched_console_dec(je_arm_sched_phase_b_preempts[slot]);
    sched_console_write(" verified_resumes=");
    sched_console_dec(je_arm_sched_verified_resumes[slot]);
    sched_console_write(" interleave=");
    sched_console_dec(je_arm_sched_interleave[slot]);
    sched_console_write(" nzcv_window_preempts=");
    sched_console_dec(je_arm_sched_nzcv_window_preempts[slot]);
    sched_console_write(" dispatches_after_exit=");
    sched_console_dec(je_arm_sched_dispatches_after_exit[slot]);
    sched_console_write("\r\n");
}

_Noreturn void je_arm_sched_guard_fault(void);

void je_arm_sched_exit(void)
{
    const uint32_t slot = je_rr_current(&je_arm_rr);
    if (slot == 0u || slot >= JE_RR_MAX) sched_fatal("idle exit");
    je_arm_sched_exit_flags[slot] = 1u;
    je_rr_exit(&je_arm_rr, slot);
#ifdef JE_ARM_SCHED_MUTANT_WFI_MASKED
    if (!je_arm_sched_mutation_announced) {
        print_mutation("wfi-i-masked");
        je_arm_sched_mutation_announced = true;
    }
    /*
     * The normal test-only caller masks I before this function.  This
     * negative profile deliberately clears it so the assertion remains
     * named and deterministic; it does not model omitting the later
     * assembly DAIFClr before WFI (known limitation C).
     */
    arm_daif_clear_i();
#endif
    if ((arm_read_daif() & JE_ARM_DAIF_I) == 0u)
        proof_fatal("exit DAIF.I", slot);
    arm_daif_set_i();
    je_arm_sched_exited[slot] = 1u;
}

static void arm_sched_sleep(uint64_t ticks)
{
    const uint32_t slot = je_rr_current(&je_arm_rr);
    uint64_t now;

    if (slot == 0u) sched_fatal("idle sleep");
    arm_daif_set_i();
    now = je_arm_timer_ticks();
    if (slot >= JE_RR_MAX || ticks == 0u ||
        ticks > JE_RR_SLEEP_MAX || now > UINT64_MAX - ticks) {
        arm_daif_clear_i();
        return;
    }
    je_rr_sleep(&je_arm_rr, slot, now, ticks);
    je_arm_sched_ready_age[slot] = 0u;
    __asm__ volatile("" ::: "memory");
    for (;;) {
        if (je_arm_rr.slots[slot].state != JE_RR_SLEEPING) {
            arm_daif_clear_i();
            return;
        }
        arm_wfi();
        arm_daif_clear_i();
        __asm__ volatile("" ::: "memory");
        arm_daif_set_i();
    }
}

static bool idle_participants_sleeping(void)
{
    for (uint32_t slot = 1u; slot <= JE_ARM_SENTINEL_SLOT; ++slot) {
        if (je_arm_rr.slots[slot].state != JE_RR_SLEEPING ||
            je_arm_sched_idle_sleep_request[slot])
            return false;
    }
    return true;
}

static void arm_sched_sleep_begin(uint32_t slot, uint64_t now, uint64_t ticks)
{
    if (slot == 0u || slot >= JE_RR_MAX || ticks == 0u ||
        ticks > JE_RR_SLEEP_MAX || now > UINT64_MAX - ticks)
        sched_fatal("idle phase sleep");
    je_rr_sleep(&je_arm_rr, slot, now, ticks);
    je_arm_sched_ready_age[slot] = 0u;
    __asm__ volatile("" ::: "memory");
}

void je_arm_sched_idle_sleep(void)
{
    const uint32_t slot = je_rr_current(&je_arm_rr);
    if (slot == 0u || slot > JE_ARM_SENTINEL_SLOT ||
        !je_arm_sched_idle_sleep_request[slot])
        sched_fatal("idle phase request");
    je_arm_sched_idle_sleep_request[slot] = false;
    arm_sched_sleep(8u);
}

void je_arm_sched_idle_prepare(void)
{
    const uint32_t slot = je_rr_current(&je_arm_rr);
    uint64_t now;

    if (slot == 0u || slot > JE_ARM_MAX_PROOF_WORKERS ||
        !je_arm_sched_idle_sleep_request[slot])
        sched_fatal("idle phase request");
    arm_daif_set_i();
    je_arm_sched_idle_sleep_request[slot] = false;
    now = je_arm_timer_ticks();
    arm_sched_sleep_begin(slot, now, 8u);
    je_arm_sched_idle_waiting[slot] = true;
}

static void proof_idle_tick(uint32_t current_slot)
{
    if (je_arm_sched_idle_active && !je_arm_sched_idle_wake_slot &&
        current_slot == 0u)
        ++je_arm_sched_idle_ticks;
}

void *je_arm_sched_test_irq(je_arm_trapframe *tf, uint32_t current_slot)
{
    je_rr_ctx *next_ctx;
    bool next_was_running;
    unsigned next_phase;
    uint32_t next;
    void *resume = NULL;
    je_status status;

    proof_entry_check(current_slot, tf);
    mutation_before_switch(tf);
#ifdef JE_ARM_SCHED_MUTANT_UNCONDITIONAL_CUR_READY
    if (je_arm_sched_monitor_done && !je_arm_sched_mutation_announced) {
        print_mutation("unconditional-cur-ready");
        je_arm_sched_mutation_announced = true;
    }
    if (je_arm_sched_monitor_done && current_slot != 0u) {
        je_arm_rr.slots[current_slot].state = JE_RR_READY;
    }
#endif
    next = je_rr_pick(&je_arm_rr);
#ifdef JE_ARM_SCHED_MUTANT_PICK_SKIP_B
    if (next == je_arm_sched_worker_slots[1] &&
        !je_arm_sched_mutation_announced) {
        print_mutation("pick-skips-b");
        je_arm_sched_mutation_announced = true;
        next = 0u;
        for (uint32_t candidate = 1u; candidate < JE_RR_MAX; ++candidate) {
            if (candidate != je_arm_sched_worker_slots[1] &&
                je_arm_rr.slots[candidate].state == JE_RR_READY) {
                next = candidate;
                break;
            }
        }
        if (next == 0u) {
            no_switch_frame(&je_arm_rr.slots[current_slot], tf);
            return tf;
        }
    }
#endif
    if (next == 0u && current_slot == 0u) {
        no_switch_frame(&je_arm_rr.slots[current_slot], tf);
        return tf;
    }
    if (next >= JE_RR_MAX) proof_fatal("pick", current_slot);
    next_ctx = &je_arm_rr.slots[next];
    next_was_running = next_ctx->state == JE_RR_RUNNING;
    next_phase = next_ctx->phase;
    if (je_arm_sched_generation_fault_slot != UINT32_MAX)
        je_arm_sched_generation_fault_slot = UINT32_MAX;
    if (je_arm_sched_idle_active && current_slot == 0u &&
        next != 0u && worker_slot(next) &&
        je_arm_sched_idle_wake_slot == 0u)
        je_arm_sched_idle_wake_slot = next;
    if (je_arm_sched_exited[next] != 0u) {
        ++je_arm_sched_dispatches_after_exit[next];
        proof_fatal("dispatches_after_exit", next);
    }
    if (next_ctx->state == JE_RR_ZOMBIE) {
        ++je_arm_sched_dispatches_after_exit[next];
        proof_fatal("dispatches_after_exit", next);
    }
    if (worker_slot(current_slot))
        ++je_arm_sched_preemptions;
    else if (next != 0u)
        ++je_arm_sched_nonpreempt_switch_ins;
    proof_preemption_counters(current_slot, tf, next);
    status = je_rr_switch(&je_arm_rr, next, &resume);
    if (status != JE_OK || resume == NULL) {
        if (je_arm_sched_generation_fault_slot == next)
            proof_fatal("frame generation", next);
        proof_fatal("frame validation", current_slot);
    }

#ifdef JE_ARM_SCHED_MUTANT_REBUILD_FAKE
    if (!je_arm_sched_mutation_announced &&
        next_phase == 1u && next_ctx->counters.starts == 1u) {
        je_arm_trapframe *fake = (je_arm_trapframe *)resume;
        print_mutation("rebuild-fake-frame");
        je_arm_sched_mutation_announced = true;
        for (unsigned i = 0u; i < JE_TF_SIZE / sizeof(uint64_t); ++i)
            ((uint64_t *)fake)[i] = 0u;
        fake->gpr[0] = next;
        fake->elr = (uint64_t)(uintptr_t)arm_sched_trampoline;
        fake->spsr = JE_ARM_SPSR_POLICY;
        *je_arm_tf_kernel_sp(fake) = (uint64_t)(uintptr_t)resume + JE_TF_SIZE;
        fake->pad = ((uint64_t)next << 32) | UINT64_C(1);
    }
#endif
    mutation_after_switch((je_arm_trapframe *)resume, next, next_phase);
    if (!arm_frame_validate(next_ctx, resume, next_phase,
                            next_was_running)) {
#ifdef JE_ARM_SCHED_MUTANT_REPLAY_STALE_FRAME
        if (je_arm_sched_generation_fault_slot == next)
            proof_fatal("frame generation", next);
#endif
        proof_fatal("frame validation", next);
    }
#ifdef JE_ARM_SCHED_MUTANT_DROP_CONSUME
    if (!je_arm_sched_mutation_announced) {
        print_mutation("drop-consume-on-resume");
        je_arm_sched_mutation_announced = true;
    }
#else
    next_ctx->frame = NULL;
#endif
    je_arm_frame_generation[next] =
        next_frame_generation(je_arm_frame_generation[next]);
    je_arm_sched_current_identity = next;
    return resume;
}

static void monitor_wait(void)
{
    uint64_t now = je_arm_timer_ticks();
    if (!je_arm_sched_monitor_done) {
        if (!je_arm_sched_idle_requested) {
            if (now > je_arm_sched_proof_deadline) {
                proof_fatal("readiness deadline", JE_ARM_MONITOR_SLOT);
            }
            if (all_proof_ready()) {
                je_arm_sched_idle_requested = true;
                for (uint32_t i = 0u; i < JE_ARM_MAX_PROOF_WORKERS; ++i)
                    je_arm_sched_idle_sleep_request[
                        je_arm_sched_worker_slots[i]] = true;
                je_arm_sched_idle_sleep_request[JE_ARM_SENTINEL_SLOT] = true;
                je_arm_sched_proof_deadline = now + 64u;
            }
        } else if (!je_arm_sched_idle_complete) {
            if (!je_arm_sched_idle_started && idle_participants_sleeping()) {
                je_arm_sched_idle_started = true;
                je_arm_sched_idle_active = true;
                je_arm_sched_idle_all_sleeping = true;
                je_arm_sched_idle_start_tick = now;
                je_arm_sched_idle_ticks = 0u;
                je_arm_sched_idle_wake_slot = 0u;
                je_arm_sched_idle_worker_resumes = 0u;
                /*
                 * This is the last running worker.  arm_sched_sleep() masks
                 * I across the SLEEPING publication, so the next timer must
                 * take the seam through je_rr_pick()==0 and slot 0.
                 */
                arm_sched_sleep(8u);
                now = je_arm_timer_ticks();
            }
            if (je_arm_sched_idle_wake_slot != 0u &&
                je_arm_sched_idle_worker_resumes != 0u) {
                je_arm_sched_idle_active = false;
                je_arm_sched_idle_complete = true;
                je_arm_sched_idle_end_tick = now;
                sched_console_write("JeiOS/aarch64: sched idle PASS "
                                    "all_sleep=");
                sched_console_dec(je_arm_sched_idle_all_sleeping ? 1u : 0u);
                sched_console_write(" start=");
                sched_console_dec(je_arm_sched_idle_start_tick);
                sched_console_write(" end=");
                sched_console_dec(je_arm_sched_idle_end_tick);
                sched_console_write(" idle_ticks=");
                sched_console_dec(je_arm_sched_idle_ticks);
                sched_console_write(" woke_slot=");
                sched_console_dec(je_arm_sched_idle_wake_slot);
                sched_console_write(" worker_resumes=");
                sched_console_dec(je_arm_sched_idle_worker_resumes);
                sched_console_write("\r\n");
                for (uint32_t i = 0u; i < JE_ARM_MAX_PROOF_WORKERS; ++i)
                    je_arm_sched_exit_flags[je_arm_sched_worker_slots[i]] = 1u;
                je_arm_sched_monitor_done = true;
                je_arm_sched_proof_deadline = now + 8u;
            } else if (now > je_arm_sched_proof_deadline) {
                proof_fatal("idle phase", JE_ARM_MONITOR_SLOT);
            }
        }
    } else {
        const bool dead = je_rr_get_state(&je_arm_rr, 1u) == JE_RR_ZOMBIE &&
                          je_rr_get_state(&je_arm_rr, 2u) == JE_RR_ZOMBIE;
        if (dead && now >= je_arm_sched_proof_deadline + 4u) {
            for (uint32_t i = 1u; i <= JE_ARM_MONITOR_SLOT; ++i)
                if (je_arm_sched_dispatches_after_exit[i] != 0u)
                    proof_fatal("dispatches_after_exit", i);
            arm_daif_set_i();
            for (uint32_t i = 1u; i <= JE_ARM_MONITOR_SLOT; ++i)
                print_counter(i);
            sched_console_write("JeiOS/aarch64: sched oracle dispatched=");
            sched_console_dec(proof_dispatched_sum());
            sched_console_write(" preemptions=");
            sched_console_dec(je_arm_sched_preemptions);
            sched_console_write(" nonpreempt_switch_ins=");
            sched_console_dec(je_arm_sched_nonpreempt_switch_ins);
            sched_console_write(" ready_bound=1\r\n");
            sched_console_write("JeiOS/aarch64: sched guard target=");
            sched_console_hex(je_arm_sched_guard_target);
            sched_console_write("\r\n");
            je_arm_sched_guard_fault();
        }
    }
}

void je_arm_worker_entry(void *arg)
{
    const uint32_t slot = (uint32_t)(uintptr_t)arg;
    if (slot == JE_ARM_MONITOR_SLOT) {
        for (;;) {
            monitor_wait();
            arm_sched_sleep(1u);
        }
    }
    if (slot == JE_ARM_SENTINEL_SLOT) {
        for (;;) {
            if (je_arm_sched_idle_sleep_request[slot])
                je_arm_sched_idle_sleep();
            else
                arm_sched_sleep(1u);
        }
    }
    /* The real canary loop is supplied below as a basic-asm function. */
    extern void je_arm_worker_canary(void);
    je_arm_worker_canary();
    sched_fatal("worker returned");
}

static void sched_create_workers(void)
{
    uint32_t slot;
    for (uint32_t i = 0u; i < JE_ARM_SCHED_TASKS; ++i) {
        if (je_rr_create(&je_arm_rr, je_arm_worker_entry,
                         (void *)(uintptr_t)(i + 1u), &slot) != JE_OK)
            sched_fatal("worker create");
        if (slot != i + 1u) sched_fatal("worker slots");
        if (i < JE_ARM_MAX_PROOF_WORKERS)
            je_arm_sched_worker_slots[i] = slot;
    }
    je_arm_sched_worker_count = JE_ARM_MAX_PROOF_WORKERS;
    je_arm_sched_guard_target =
        (uint64_t)(je_arm_rr.slots[je_arm_sched_worker_slots[0]].usable_base -
                   (uintptr_t)JE_ARM_PAGE_SIZE);
    je_arm_sched_proof_deadline = je_arm_timer_ticks() + 64u;
}

_Noreturn static void sched_unmask_and_idle(void)
{
    arm_daif_clear_i();
    for (;;) arm_wfi();
}

_Noreturn void je_arm_sched_test(void)
{
    sched_create_workers();
#ifdef JE_ARM_SCHED_MUTANT_DROP_MOV_SP
    print_mutation("drop-mov-sp");
    je_arm_sched_mutation_announced = true;
#endif
#ifdef JE_ARM_SCHED_MUTANT_DROP_HIGH_REG_RESTORE
    print_mutation("drop-high-reg-restore");
    je_arm_sched_mutation_announced = true;
#endif
    sched_console_write("JeiOS/aarch64: sched test workers=");
    sched_console_hex(je_arm_sched_worker_count);
    sched_console_write("\r\n");
    sched_unmask_and_idle();
}

bool je_arm_sched_guard_fault_check(const je_arm_trapframe *tf, uint64_t ec)
{
    extern uint8_t je_arm_sched_guard_store_site[];
    uint64_t fsc;
    uint64_t wnr;

    if (tf == NULL) return false;
    fsc = tf->esr & UINT64_C(0x3f);
    wnr = (tf->esr >> 6) & 1u;
    return tf->vector == 4u && ec == UINT64_C(0x25) &&
           fsc == UINT64_C(0x07) && wnr == 1u &&
           tf->far == je_arm_sched_guard_target &&
           tf->elr == (uint64_t)(uintptr_t)je_arm_sched_guard_store_site;
}

_Noreturn void je_arm_sched_guard_fault(void)
{
    extern uint8_t je_arm_sched_guard_store_site[];
    const uint64_t target = je_arm_sched_guard_target;
    __asm__ volatile(".globl je_arm_sched_guard_store_site\n"
                     "je_arm_sched_guard_store_site:\n"
                     "str xzr, [%0]"
                     :: "r"(target) : "memory");
    (void)target;
    (void)je_arm_sched_guard_store_site;
    sched_fatal("guard did not fault");
}

#endif /* JE_ARM_SCHED_TEST */

#ifdef JE_ARM_SCHED_TEST
/*
 * The proof worker alternates two genuinely different live-register windows:
 * phase A keeps x0..x25 live while x26..x30 are scratch, and phase B keeps
 * x26..x30 live while x0..x25 are scratch.  Phase B compares the high
 * registers directly after the returning IRQ; no saved copy can hide a
 * missing high-register restore.  The C side requires a preemption in both
 * labelled windows for every proof worker.
 *
 * Each NZCV window uses only flag-preserving SUB/CBNZ instructions after MSR
 * and reads MRS before any CMP.  The handler classifies the saved SPSR while
 * the post-ERET MRS proves the restore path.
 */
__asm__(
    ".text\n"
    ".align 2\n"
    ".globl je_arm_worker_canary\n"
    ".type je_arm_worker_canary, %function\n"
    ".globl je_arm_worker_canary_start\n"
    "je_arm_worker_canary_start:\n"
    "je_arm_worker_canary:\n"
    "    sub sp, sp, #256\n"
    "    str x0, [sp, #0]\n"
    ".Lje_sched_loop:\n"
    "    adrp x0, je_arm_sched_canary_phase\n"
    "    add x0, x0, #:lo12:je_arm_sched_canary_phase\n"
    "    ldr w2, [sp, #0]\n"
    "    ldr w1, [x0, x2, lsl #2]\n"
    "    cbnz w1, .Lje_sched_phase_b\n"
    ".Lje_sched_phase_a:\n"
    "    ldr w1, [sp, #0]\n"
    "    adrp x29, je_arm_sched_rodata_patterns\n"
    "    add x29, x29, #:lo12:je_arm_sched_rodata_patterns\n"
    "    mov x26, #248\n"
    "    mul x26, x1, x26\n"
    "    add x29, x29, x26\n"
    "    ldr x0, [x29, #0]\n"
    "    ldr x1, [x29, #8]\n"
    "    ldr x2, [x29, #16]\n"
    "    ldr x3, [x29, #24]\n"
    "    ldr x4, [x29, #32]\n"
    "    ldr x5, [x29, #40]\n"
    "    ldr x6, [x29, #48]\n"
    "    ldr x7, [x29, #56]\n"
    "    ldr x8, [x29, #64]\n"
    "    ldr x9, [x29, #72]\n"
    "    ldr x10, [x29, #80]\n"
    "    ldr x11, [x29, #88]\n"
    "    ldr x12, [x29, #96]\n"
    "    ldr x13, [x29, #104]\n"
    "    ldr x14, [x29, #112]\n"
    "    ldr x15, [x29, #120]\n"
    "    ldr x16, [x29, #128]\n"
    "    ldr x17, [x29, #136]\n"
    "    ldr x18, [x29, #144]\n"
    "    ldr x19, [x29, #152]\n"
    "    ldr x20, [x29, #160]\n"
    "    ldr x21, [x29, #168]\n"
    "    ldr x22, [x29, #176]\n"
    "    ldr x23, [x29, #184]\n"
    "    ldr x24, [x29, #192]\n"
    "    ldr x25, [x29, #200]\n"
    "    ldr x26, [x29, #208]\n"
    "    ldr x27, [x29, #216]\n"
    "    ldr x28, [x29, #224]\n"
    "    ldr x30, [x29, #240]\n"
    "    ldr x29, [x29, #232]\n"
    "    add x0, x0, #0\n"
    "    add x1, x1, #1\n"
    "    add x2, x2, #2\n"
    "    add x3, x3, #3\n"
    "    add x4, x4, #4\n"
    "    add x5, x5, #5\n"
    "    add x6, x6, #6\n"
    "    add x7, x7, #7\n"
    "    add x8, x8, #8\n"
    "    add x9, x9, #9\n"
    "    add x10, x10, #10\n"
    "    add x11, x11, #11\n"
    "    add x12, x12, #12\n"
    "    add x13, x13, #13\n"
    "    add x14, x14, #14\n"
    "    add x15, x15, #15\n"
    "    add x16, x16, #16\n"
    "    add x17, x17, #17\n"
    "    add x18, x18, #18\n"
    "    add x19, x19, #19\n"
    "    add x20, x20, #20\n"
    "    add x21, x21, #21\n"
    "    add x22, x22, #22\n"
    "    add x23, x23, #23\n"
    "    add x24, x24, #24\n"
    "    add x25, x25, #25\n"
    "    add x26, x26, #26\n"
    "    add x27, x27, #27\n"
    "    add x28, x28, #28\n"
    "    add x30, x30, #30\n"
    "    add x29, x29, #29\n"
    "    str x26, [sp, #8]\n"
    "    str x27, [sp, #16]\n"
    "    str x28, [sp, #24]\n"
    "    str x29, [sp, #32]\n"
    "    str x30, [sp, #40]\n"
    "    ldr w27, [sp, #0]\n"
    "    adrp x28, je_arm_sched_nzcv_phase\n"
    "    add x28, x28, #:lo12:je_arm_sched_nzcv_phase\n"
    "    ldr w26, [x28, x27, lsl #2]\n"
    "    add x27, x27, x27\n"
    "    add x27, x27, x26\n"
    "    adrp x28, je_arm_nzcv_patterns\n"
    "    add x28, x28, #:lo12:je_arm_nzcv_patterns\n"
    "    ldr x30, [x28, x27, lsl #3]\n"
    ".globl je_arm_worker_phase_a_start\n"
    "je_arm_worker_phase_a_start:\n"
    ".globl je_arm_worker_low_nzcv_start\n"
    "je_arm_worker_low_nzcv_start:\n"
    "    msr nzcv, x30\n"
    "    mov x28, #1024\n"
    ".Lje_sched_nzcv_spin:\n"
    "    sub x28, x28, #1\n"
    "    cbnz x28, .Lje_sched_nzcv_spin\n"
    ".globl je_arm_worker_low_nzcv_end\n"
    "je_arm_worker_low_nzcv_end:\n"
    "    mrs x28, nzcv\n"
    "    cmp x28, x30\n"
    "    b.ne .Lje_sched_fail_nzcv\n"
    "    adrp x26, je_arm_sched_current_identity\n"
    "    add x26, x26, #:lo12:je_arm_sched_current_identity\n"
    "    ldr w26, [x26]\n"
    "    ldr w27, [sp, #0]\n"
    "    cmp w26, w27\n"
    "    b.ne .Lje_sched_fail_identity\n"
    "    adrp x28, je_arm_sched_stack_base\n"
    "    add x28, x28, #:lo12:je_arm_sched_stack_base\n"
    "    ldr x29, [x28, x27, lsl #3]\n"
    "    cmp sp, x29\n"
    "    b.lo .Lje_sched_fail_sp\n"
    "    adrp x28, je_arm_sched_stack_top\n"
    "    add x28, x28, #:lo12:je_arm_sched_stack_top\n"
    "    ldr x29, [x28, x27, lsl #3]\n"
    "    cmp sp, x29\n"
    "    b.hi .Lje_sched_fail_sp\n"
    "    adrp x26, je_arm_sched_canary_checks\n"
    "    add x26, x26, #:lo12:je_arm_sched_canary_checks\n"
    "    add x26, x26, x27, lsl #3\n"
    "    ldr x28, [x26]\n"
    "    add x28, x28, #1\n"
    "    str x28, [x26]\n"
    "    adrp x26, je_arm_sched_exit_flags\n"
    "    add x26, x26, #:lo12:je_arm_sched_exit_flags\n"
    "    ldr w28, [x26, x27, lsl #2]\n"
    "    cbnz w28, .Lje_sched_exit\n"
    "    adrp x29, je_arm_sched_rodata_patterns\n"
    "    add x29, x29, #:lo12:je_arm_sched_rodata_patterns\n"
    "    ldr w27, [sp, #0]\n"
    "    mov x26, #248\n"
    "    mul x26, x27, x26\n"
    "    add x29, x29, x26\n"
    ".macro JE_SCHED_CHECK reg, num\n"
    "    ldr x30, [x29, #((\\num) * 8)]\n"
    "    add x30, x30, #(\\num)\n"
    "    cmp x\\reg, x30\n"
    "    b.ne .Lje_sched_fail_\\num\n"
    ".endm\n"
    "    JE_SCHED_CHECK 0, 0\n"
    "    JE_SCHED_CHECK 1, 1\n"
    "    JE_SCHED_CHECK 2, 2\n"
    "    JE_SCHED_CHECK 3, 3\n"
    "    JE_SCHED_CHECK 4, 4\n"
    "    JE_SCHED_CHECK 5, 5\n"
    "    JE_SCHED_CHECK 6, 6\n"
    "    JE_SCHED_CHECK 7, 7\n"
    "    JE_SCHED_CHECK 8, 8\n"
    "    JE_SCHED_CHECK 9, 9\n"
    "    JE_SCHED_CHECK 10, 10\n"
    "    JE_SCHED_CHECK 11, 11\n"
    "    JE_SCHED_CHECK 12, 12\n"
    "    JE_SCHED_CHECK 13, 13\n"
    "    JE_SCHED_CHECK 14, 14\n"
    "    JE_SCHED_CHECK 15, 15\n"
    "    JE_SCHED_CHECK 16, 16\n"
    "    JE_SCHED_CHECK 17, 17\n"
    "    JE_SCHED_CHECK 18, 18\n"
    "    JE_SCHED_CHECK 19, 19\n"
    "    JE_SCHED_CHECK 20, 20\n"
    "    JE_SCHED_CHECK 21, 21\n"
    "    JE_SCHED_CHECK 22, 22\n"
    "    JE_SCHED_CHECK 23, 23\n"
    "    JE_SCHED_CHECK 24, 24\n"
    "    JE_SCHED_CHECK 25, 25\n"
    ".globl je_arm_worker_phase_a_end\n"
    "je_arm_worker_phase_a_end:\n"
    "    ldr w1, [sp, #0]\n"
    "    adrp x26, je_arm_sched_exit_flags\n"
    "    add x26, x26, #:lo12:je_arm_sched_exit_flags\n"
    "    ldr w28, [x26, x1, lsl #2]\n"
    "    cbnz w28, .Lje_sched_exit\n"
    "    adrp x26, je_arm_sched_idle_sleep_request\n"
    "    add x26, x26, #:lo12:je_arm_sched_idle_sleep_request\n"
    "    ldrb w28, [x26, x1]\n"
    "    cbnz w28, .Lje_sched_idle_sleep\n"
    "    adrp x28, je_arm_sched_canary_phase\n"
    "    add x28, x28, #:lo12:je_arm_sched_canary_phase\n"
    "    mov w26, #1\n"
    "    str w26, [x28, x1, lsl #2]\n"
    "    adrp x28, je_arm_sched_nzcv_phase\n"
    "    add x28, x28, #:lo12:je_arm_sched_nzcv_phase\n"
    "    ldr w26, [x28, x1, lsl #2]\n"
    "    eor w26, w26, #1\n"
    "    str w26, [x28, x1, lsl #2]\n"
    "    adrp x29, je_arm_sched_rodata_patterns\n"
    "    add x29, x29, #:lo12:je_arm_sched_rodata_patterns\n"
    "    ldr w1, [sp, #0]\n"
    "    mov x26, #248\n"
    "    mul x26, x1, x26\n"
    "    add x29, x29, x26\n"
    "    ldr x26, [x29, #208]\n"
    "    add x26, x26, #26\n"
    "    ldr x27, [x29, #216]\n"
    "    add x27, x27, #27\n"
    "    ldr x28, [x29, #224]\n"
    "    add x28, x28, #28\n"
    "    ldr x30, [x29, #240]\n"
    "    add x30, x30, #30\n"
    "    ldr x29, [x29, #232]\n"
    "    add x29, x29, #29\n"
    "    b .Lje_sched_loop\n"

    ".Lje_sched_phase_b:\n"
    ".globl je_arm_worker_phase_b_start\n"
    "je_arm_worker_phase_b_start:\n"
    "    ldr w1, [sp, #0]\n"
    "    adrp x0, je_arm_sched_nzcv_phase\n"
    "    add x0, x0, #:lo12:je_arm_sched_nzcv_phase\n"
    "    ldr w3, [x0, x1, lsl #2]\n"
    "    add w2, w1, w1\n"
    "    add w2, w2, w3\n"
    "    adrp x3, je_arm_nzcv_patterns\n"
    "    add x3, x3, #:lo12:je_arm_nzcv_patterns\n"
    "    ldr x0, [x3, x2, lsl #3]\n"
    ".globl je_arm_worker_high_nzcv_start\n"
    "je_arm_worker_high_nzcv_start:\n"
    "    msr nzcv, x0\n"
    "    mov x1, #1024\n"
    ".Lje_sched_high_nzcv_spin:\n"
    "    sub x1, x1, #1\n"
    "    cbnz x1, .Lje_sched_high_nzcv_spin\n"
    ".globl je_arm_worker_high_nzcv_end\n"
    "je_arm_worker_high_nzcv_end:\n"
    "    mrs x1, nzcv\n"
    "    cmp x1, x0\n"
    "    b.ne .Lje_sched_fail_nzcv\n"
    "    adrp x0, je_arm_sched_current_identity\n"
    "    add x0, x0, #:lo12:je_arm_sched_current_identity\n"
    "    ldr w0, [x0]\n"
    "    ldr w1, [sp, #0]\n"
    "    cmp w0, w1\n"
    "    b.ne .Lje_sched_fail_identity\n"
    "    adrp x2, je_arm_sched_stack_base\n"
    "    add x2, x2, #:lo12:je_arm_sched_stack_base\n"
    "    ldr x3, [x2, x1, lsl #3]\n"
    "    cmp sp, x3\n"
    "    b.lo .Lje_sched_fail_sp\n"
    "    adrp x2, je_arm_sched_stack_top\n"
    "    add x2, x2, #:lo12:je_arm_sched_stack_top\n"
    "    ldr x3, [x2, x1, lsl #3]\n"
    "    cmp sp, x3\n"
    "    b.hi .Lje_sched_fail_sp\n"
    "    adrp x2, je_arm_sched_rodata_patterns\n"
    "    add x2, x2, #:lo12:je_arm_sched_rodata_patterns\n"
    "    mov x3, #248\n"
    "    mul x3, x1, x3\n"
    "    add x2, x2, x3\n"
    "    ldr x0, [x2, #208]\n"
    "    add x0, x0, #26\n"
    "    cmp x26, x0\n"
    "    b.ne .Lje_sched_fail_26\n"
    "    ldr x0, [x2, #216]\n"
    "    add x0, x0, #27\n"
    "    cmp x27, x0\n"
    "    b.ne .Lje_sched_fail_27\n"
    "    ldr x0, [x2, #224]\n"
    "    add x0, x0, #28\n"
    "    cmp x28, x0\n"
    "    b.ne .Lje_sched_fail_28\n"
    "    ldr x0, [x2, #232]\n"
    "    add x0, x0, #29\n"
    "    cmp x29, x0\n"
    "    b.ne .Lje_sched_fail_29\n"
    "    ldr x0, [x2, #240]\n"
    "    add x0, x0, #30\n"
    "    cmp x30, x0\n"
    "    b.ne .Lje_sched_fail_30\n"
    ".globl je_arm_worker_phase_b_end\n"
    "je_arm_worker_phase_b_end:\n"
    "    adrp x2, je_arm_sched_canary_checks\n"
    "    add x2, x2, #:lo12:je_arm_sched_canary_checks\n"
    "    add x2, x2, x1, lsl #3\n"
    "    ldr x0, [x2]\n"
    "    add x0, x0, #1\n"
    "    str x0, [x2]\n"
    "    adrp x2, je_arm_sched_exit_flags\n"
    "    add x2, x2, #:lo12:je_arm_sched_exit_flags\n"
    "    ldr w0, [x2, x1, lsl #2]\n"
    "    cbnz w0, .Lje_sched_exit\n"
    "    adrp x2, je_arm_sched_idle_sleep_request\n"
    "    add x2, x2, #:lo12:je_arm_sched_idle_sleep_request\n"
    "    ldrb w0, [x2, x1]\n"
    "    cbnz w0, .Lje_sched_idle_sleep\n"
    "    adrp x2, je_arm_sched_canary_phase\n"
    "    add x2, x2, #:lo12:je_arm_sched_canary_phase\n"
    "    str wzr, [x2, x1, lsl #2]\n"
    "    adrp x2, je_arm_sched_nzcv_phase\n"
    "    add x2, x2, #:lo12:je_arm_sched_nzcv_phase\n"
    "    ldr w0, [x2, x1, lsl #2]\n"
    "    eor w0, w0, #1\n"
    "    str w0, [x2, x1, lsl #2]\n"
    "    b .Lje_sched_loop\n"

    ".Lje_sched_idle_sleep:\n"
    "    adrp x0, je_arm_sched_canary_phase\n"
    "    add x0, x0, #:lo12:je_arm_sched_canary_phase\n"
    "    ldr w1, [sp, #0]\n"
    "    str wzr, [x0, x1, lsl #2]\n"
    "    msr daifset, #2\n"
    "    bl je_arm_sched_idle_prepare\n"
    ".Lje_sched_idle_wfi:\n"
    "    wfi\n"
    "    msr daifclr, #2\n"
    "    isb\n"
    "    adrp x0, je_arm_sched_idle_waiting\n"
    "    add x0, x0, #:lo12:je_arm_sched_idle_waiting\n"
    "    ldr w1, [sp, #0]\n"
    "    ldrb w0, [x0, x1]\n"
    "    cbnz w0, .Lje_sched_idle_mask\n"
    "    b .Lje_sched_loop\n"
    ".Lje_sched_idle_mask:\n"
    "    msr daifset, #2\n"
    "    b .Lje_sched_idle_wfi\n"
    ".Lje_sched_exit:\n"
    "    msr daifset, #2\n"
    "    bl je_arm_sched_exit\n"
    "    msr daifclr, #2\n"
    "    isb\n"
    ".Lje_sched_exit_wait:\n"
    "    wfi\n"
    "    b .Lje_sched_exit_wait\n"
    ".Lje_sched_fail_identity:\n"
    "    mov w0, #31\n"
    "    b je_arm_sched_canary_failure\n"
    ".Lje_sched_fail_nzcv:\n"
    "    mov w0, #32\n"
    "    b je_arm_sched_canary_failure\n"
    ".Lje_sched_fail_sp:\n"
    "    mov w0, #31\n"
    "    b je_arm_sched_canary_failure\n"
    ".macro JE_SCHED_FAIL num\n"
    ".Lje_sched_fail_\\num:\n"
    "    mov w0, #(\\num)\n"
    "    b je_arm_sched_canary_failure\n"
    ".endm\n"
    "    JE_SCHED_FAIL 0\n"
    "    JE_SCHED_FAIL 1\n"
    "    JE_SCHED_FAIL 2\n"
    "    JE_SCHED_FAIL 3\n"
    "    JE_SCHED_FAIL 4\n"
    "    JE_SCHED_FAIL 5\n"
    "    JE_SCHED_FAIL 6\n"
    "    JE_SCHED_FAIL 7\n"
    "    JE_SCHED_FAIL 8\n"
    "    JE_SCHED_FAIL 9\n"
    "    JE_SCHED_FAIL 10\n"
    "    JE_SCHED_FAIL 11\n"
    "    JE_SCHED_FAIL 12\n"
    "    JE_SCHED_FAIL 13\n"
    "    JE_SCHED_FAIL 14\n"
    "    JE_SCHED_FAIL 15\n"
    "    JE_SCHED_FAIL 16\n"
    "    JE_SCHED_FAIL 17\n"
    "    JE_SCHED_FAIL 18\n"
    "    JE_SCHED_FAIL 19\n"
    "    JE_SCHED_FAIL 20\n"
    "    JE_SCHED_FAIL 21\n"
    "    JE_SCHED_FAIL 22\n"
    "    JE_SCHED_FAIL 23\n"
    "    JE_SCHED_FAIL 24\n"
    "    JE_SCHED_FAIL 25\n"
    "    JE_SCHED_FAIL 26\n"
    "    JE_SCHED_FAIL 27\n"
    "    JE_SCHED_FAIL 28\n"
    "    JE_SCHED_FAIL 29\n"
    "    JE_SCHED_FAIL 30\n"
    ".globl je_arm_worker_canary_end\n"
    "je_arm_worker_canary_end:\n"
    ".size je_arm_worker_canary, .-je_arm_worker_canary\n"
);
#endif /* JE_ARM_SCHED_TEST */
