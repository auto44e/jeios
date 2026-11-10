/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP3a-2 AArch64 EL0 execution proof.
 *
 * This is a bounded kernel-owned harness, not a user assertion channel. The
 * kernel chooses each image, observes the saved frame/ESR/FAR, owns the process
 * lifecycle, and advances the negative cases only after reap. QEMU TCG
 * observes EL0 translations, permission faults, exception entry and register
 * return; cache/TLB maintenance remains reviewed evidence.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jeios/abi.h>
#include <jeios/handle.h>
#include <jeios/endpoint_kern.h>
#include <jeios/lifecycle.h>
#include <jeios/proc.h>
#include <jeios/status.h>
#include <jeios/syscall.h>
#include <jeios/usercopy.h>
#include <jeios/vm.h>

#include "paging.h"
#include "trapframe.h"
#include "gic.h"

extern je_pages *je_arm_scheduler_pages(void);
extern je_arm_mmu je_arm_mmu_context;
extern void je_arm_sched_init(void);
extern void je_arm_sched_set_proc_pool(je_proc_pool *pool);
extern je_status je_arm_sched_create_user(je_vm *vm, uint32_t index,
                                           je_proc_id proc_id,
                                           uint32_t *out_slot);
extern je_status je_arm_sched_current_user(je_vm **out_vm, uint32_t *out_index,
                                            uint32_t *out_slot);
extern uint32_t je_arm_el0_test_mode_rejection(uint32_t slot);
extern bool je_arm_sched_ipc_was_blocked(uint32_t slot);
extern bool je_arm_sched_ipc_woken_restart(uint32_t slot);
extern bool je_sched_wait_pending(je_ep_channel **rec, uint8_t *side,
                                  uint64_t *rdi, uint64_t *rsi,
                                  uint64_t *rdx, uint64_t *r8, uint64_t *r9,
                                  uint64_t *post_svc_elr);
extern _Noreturn void je_arm_sched_start_user(void);
extern const je_vm_ops *je_arch_vm_ops(je_pages *pages, int nx_available);
extern uint64_t je_arm_user_root[];
extern void je_arm_sched_install_ttbr0(uint64_t root_pa, uint16_t asid);

#define UART_DR UINT64_C(0x09000000)
#define UART_FR UINT64_C(0x09000018)
#define UART_FR_TXFF UINT32_C(1 << 5)

#define A64_MOVZ(rd, imm, hw) \
    (UINT32_C(0xd2800000) | ((uint32_t)(hw) << 21) | \
     (((uint32_t)(imm) & UINT32_C(0xffff)) << 5) | (uint32_t)(rd))
#define A64_MOVK(rd, imm, hw) \
    (UINT32_C(0xf2800000) | ((uint32_t)(hw) << 21) | \
     (((uint32_t)(imm) & UINT32_C(0xffff)) << 5) | (uint32_t)(rd))
#define A64_ADD(rd, rn, imm) \
    (UINT32_C(0x91000000) | (((uint32_t)(imm) & UINT32_C(0xfff)) << 10) | \
     ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define A64_ADD12(rd, rn, imm) \
    (UINT32_C(0x91400000) | (((uint32_t)(imm) & UINT32_C(0xfff)) << 10) | \
     ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define A64_SUBS_IMM(rd, rn, imm) \
    (UINT32_C(0xf1000000) | (((uint32_t)(imm) & UINT32_C(0xfff)) << 10) | \
     ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define A64_CBNZ(rt, imm19) \
    (UINT32_C(0xb5000000) | (((uint32_t)(imm19) & UINT32_C(0x7ffff)) << 5) | \
     (uint32_t)(rt))
#define A64_BCOND(cond, imm19) \
    (UINT32_C(0x54000000) | (((uint32_t)(imm19) & UINT32_C(0x7ffff)) << 5) | \
     ((uint32_t)(cond) & UINT32_C(0xf)))
#define A64_BCOND_NE UINT32_C(1)
#define A64_BCOND_EQ UINT32_C(0)
#define A64_CMP(rn, rm) \
    (UINT32_C(0xeb00001f) | ((uint32_t)(rm) << 16) | ((uint32_t)(rn) << 5))
#define A64_LDR(rt, rn, imm) \
    (UINT32_C(0xf9400000) | (((uint32_t)(imm) / 8u) << 10) | \
     ((uint32_t)(rn) << 5) | (uint32_t)(rt))
#define A64_STR(rt, rn, imm) \
    (UINT32_C(0xf9000000) | (((uint32_t)(imm) / 8u) << 10) | \
     ((uint32_t)(rn) << 5) | (uint32_t)(rt))
#define A64_LSR(rd, rn, shift) \
    (UINT32_C(0xd340fc00) | (((uint32_t)(shift) & 0x3fu) << 16) | \
     ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define A64_UBFM(rd, rn, immr, imms) \
    (UINT32_C(0xd3400000) | (((uint32_t)(immr) & 0x3fu) << 16) | \
     (((uint32_t)(imms) & 0x3fu) << 10) | ((uint32_t)(rn) << 5) | \
     (uint32_t)(rd))
#define A64_ORR(rd, rn, rm) \
    (UINT32_C(0xaa000000) | ((uint32_t)(rm) << 16) | \
     ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define A64_MRS_TPIDR_EL0(rt) (UINT32_C(0xd53bd040) | (uint32_t)(rt))
#define A64_MRS_TPIDRRO_EL0(rt) (UINT32_C(0xd53bd060) | (uint32_t)(rt))
#define A64_MRS_MDCCSR_EL0(rt) (UINT32_C(0xd5330100) | (uint32_t)(rt))
#define A64_MSR_CNTP_CTL_EL0(rt) (UINT32_C(0xd51be220) | (uint32_t)(rt))
#define A64_B(imm26) (UINT32_C(0x14000000) | ((uint32_t)(imm26) & UINT32_C(0x03ffffff)))
#define A64_SVC(imm16) (UINT32_C(0xd4000001) | (((uint32_t)(imm16) & UINT32_C(0xffff)) << 5))

#define JE_ARM_EL0_PROOF_OP UINT64_C(0x7a)
#define JE_ARM_EL0_PROOF_ARG0 UINT64_C(0x1000)
#define JE_ARM_EL0_PROOF_STATUS JE_ABI_UNSUPPORTED
#define JE_ARM_EL0_PROOF_RESULT UINT64_C(0x5aa5)
#define JE_EL0_REGISTER_PASS UINT64_C(0x5245)
#define JE_EL0_REGISTER_FAIL UINT64_C(0xbad0)
#define JE_EL0_SP_FAIL UINT64_C(0xbad1)
#define JE_EL0_EXIT_SIGNAL UINT64_C(0x455849545349474e)
#define JE_EL0_SPIN_COUNT UINT16_C(0x1000)
#define JE_EL0_SURVIVOR_SPIN_COUNT UINT16_C(0x4)
#define JE_EL0_TPIDRRO_EXPECTED UINT64_C(0)
#define JE_EL0_SP_POISON (JE_VM_ARENA_BASE + UINT64_C(0x100))
#define JE_EL0_CNTP_CTL_ISS UINT64_C(0x000000000032f804)
#define JE_EL0_DCC_ISS UINT64_C(0x000000000020c003)
#define JE_EL0_COPY_ROUND_COUNT 3u
#define JE_EL0_COPY_SRC0 UINT64_C(0x100)
#define JE_EL0_COPY_DST0 UINT64_C(0x200)
#define JE_EL0_COPY_SRC256 UINT64_C(0xff0)
#define JE_EL0_COPY_DST256 UINT64_C(0x1200)
#define JE_EL0_COPY_SRC255 UINT64_C(0x1300)
#define JE_EL0_COPY_DST255 UINT64_C(0x1e01)

typedef enum {
    JE_EL0_NEG_KERNEL_READ = 0,
    JE_EL0_NEG_KERNEL_WRITE,
    JE_EL0_NEG_KERNEL_EXEC,
    JE_EL0_NEG_BAD_SP,
    JE_EL0_NEG_BAD_ELR,
    JE_EL0_NEG_FP,
    JE_EL0_NEG_CNTVCT,
    JE_EL0_NEG_SVC_IMM,
    JE_EL0_NEG_CNTP_CTL,
    JE_EL0_NEG_DCC,
    JE_EL0_NEG_COUNT
} je_el0_negative;

static je_vm je_el0_vm;
static je_proc_pool je_el0_pool;
static uint8_t je_el0_good_blob[2][4u * JE_PAGE_SIZE]
    __attribute__((aligned(16)));
static uint8_t je_el0_negative_blob[4u * JE_PAGE_SIZE]
    __attribute__((aligned(16)));
static uint8_t je_el0_death_blob[4u * JE_PAGE_SIZE]
    __attribute__((aligned(16)));
static je_vm_image je_el0_image;
static je_proc_id je_el0_good_proc[2];
static uint32_t je_el0_good_index[2];
static uint32_t je_el0_good_yield_mask;
static uint64_t je_el0_good_value[2];
static uint32_t je_el0_good_slot[2];
static je_el0_negative je_el0_active;
static bool je_el0_negative_live;
static uint32_t je_el0_negative_index;
static je_proc_id je_el0_negative_proc;
static bool je_el0_passed[JE_EL0_NEG_COUNT];
static bool je_el0_seen_fault;
static uint32_t je_el0_register_roundtrip_mask;
static bool je_el0_mode_negative_passed;
static bool je_el0_sp_el1_marker[2];
static bool je_el0_irq_preempted;
static bool je_el0_irq_peer_ran;
static uint64_t je_el0_irq_preemptions;
static bool je_el0_survivor_reaped;
static bool je_el0_irq_marker_printed;
static uint64_t je_el0_lone_yields;
static bool je_el0_lone_irq_reselect;
static bool je_el0_survivor_spin_armed;
static unsigned je_el0_survivor_spin_start;
static unsigned je_el0_survivor_spin_end;
static unsigned je_el0_spin_start;
static unsigned je_el0_spin_end;
static uint32_t je_el0_copy_round[2];
static uint32_t je_el0_handle_step[2];
static bool je_el0_copy_fixup_proven;
static bool je_el0_ipc_woken_seen;
static bool je_el0_ipc_timeout_seen;
static bool je_el0_ipc_timer_wake_seen;
static bool je_el0_ipc_peer_closed_seen;
static bool je_el0_ipc_deadcap_seen;
static bool je_el0_ipc_deadcap_verified;
static bool je_el0_ipc_deadcap_pending;
#if defined(JE_ARM_IPC_FAULT) && JE_ARM_IPC_FAULT == 7
static bool je_el0_short_buffer_marker_seen;
#endif
static uint32_t je_el0_ipc_deadcap_slot;
static je_handle je_el0_deadcap_source;
static bool je_el0_deadcap_source_closed;
static bool je_el0_ipc_idle_seen;
static bool je_el0_all_blocked_state_seen;
static bool je_el0_idle_guard_seen;
static bool je_el0_ipc_death_seen;
static bool je_el0_death_live;
#ifndef JE_ARM_EL0_DISABLE_WAKE_KILL
static bool je_el0_death_kill_pending;
static bool je_el0_death_killed;
#endif
static uint32_t je_el0_death_index;
static uint32_t je_el0_death_slot;
static je_proc_id je_el0_death_proc;
static je_proc_id je_el0_reuse_proc;
static uint32_t je_el0_reuse_index;
static uint32_t je_el0_reuse_slot;
static bool je_el0_reuse_live;
static bool je_el0_reuse_proc_done;
static bool je_el0_reuse_marker;
static uint32_t je_el0_death_waiters_before;
static je_ep_channel *je_el0_death_rec;
static uint8_t je_el0_death_side;
static uint32_t je_el0_death_waiter_id;
static uint32_t je_el0_death_generation;
extern volatile uint8_t je_arm_in_handler;
#ifdef JE_ARM_COPY_FAULT_MUTANT_RESTORE_SELECTED
extern volatile uint8_t je_arm_copy_restore_selected_seen;
#endif

static void proof_putc(char c)
{
    for (unsigned i = 0u; i < 100000u; ++i)
        if ((*(volatile uint32_t *)(uintptr_t)UART_FR & UART_FR_TXFF) == 0u)
            break;
    *(volatile uint32_t *)(uintptr_t)UART_DR = (uint32_t)(unsigned char)c;
}

static void proof_write(const char *text)
{
    for (unsigned i = 0u; i < 4096u && text[i] != '\0'; ++i)
        proof_putc(text[i]);
}

static void proof_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (unsigned i = 0u; i < 16u; ++i)
        text[2u + i] = digits[(value >> ((15u - i) * 4u)) & 0xfu];
    text[18] = '\0';
    proof_write(text);
}

static void proof_dec(uint64_t value)
{
    char text[21];
    unsigned pos = 20u;
    text[pos] = '\0';
    do {
        --pos;
        text[pos] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0u);
    proof_write(&text[pos]);
}

static _Noreturn void proof_fatal(const char *reason)
{
    proof_write("JeiOS/aarch64: FATAL el0-test ");
    proof_write(reason);
    proof_write("\r\n");
    for (;;) __asm__ volatile("wfe");
}

static _Noreturn void proof_halt(void)
{
    for (;;) __asm__ volatile("wfe");
}

static void emit(uint8_t *blob, unsigned *pc, uint32_t instruction)
{
    if (*pc > 2u * JE_PAGE_SIZE - sizeof(uint32_t))
        proof_fatal("blob-overflow");
    blob[*pc + 0u] = (uint8_t)(instruction & 0xffu);
    blob[*pc + 1u] = (uint8_t)((instruction >> 8) & 0xffu);
    blob[*pc + 2u] = (uint8_t)((instruction >> 16) & 0xffu);
    blob[*pc + 3u] = (uint8_t)((instruction >> 24) & 0xffu);
    *pc += sizeof(uint32_t);
}

static void patch_u32(uint8_t *blob, unsigned at, uint32_t instruction)
{
    blob[at + 0u] = (uint8_t)(instruction & 0xffu);
    blob[at + 1u] = (uint8_t)((instruction >> 8) & 0xffu);
    blob[at + 2u] = (uint8_t)((instruction >> 16) & 0xffu);
    blob[at + 3u] = (uint8_t)((instruction >> 24) & 0xffu);
}

static void patch_pc_relative(uint8_t *blob, unsigned branch,
                              unsigned target, bool conditional,
                              uint32_t condition)
{
    const int64_t delta = (int64_t)target - (int64_t)branch;
    if ((delta & INT64_C(3)) != 0) proof_fatal("branch-align");
    const int64_t words = delta / INT64_C(4);
    if (conditional) {
        if (words < -INT64_C(262144) || words > INT64_C(262143))
            proof_fatal("cond-branch-range");
        patch_u32(blob, branch,
                  A64_BCOND(condition, (uint32_t)words));
    } else {
        if (words < -INT64_C(33554432) || words > INT64_C(33554431))
            proof_fatal("branch-range");
        patch_u32(blob, branch, A64_B((uint32_t)words));
    }
}

static void emit_load_data_pointer(uint8_t *blob, unsigned *pc)
{
    emit(blob, pc, A64_MOVZ(1u, UINT16_C(0x1000), 2u));
    emit(blob, pc, A64_ADD12(1u, 1u, 2u));
}

static void emit_load_u64(uint8_t *blob, unsigned *pc,
                          unsigned reg, uint64_t value)
{
    emit(blob, pc, A64_MOVZ(reg, (uint16_t)value, 0u));
    for (unsigned hw = 1u; hw < 4u; ++hw)
        emit(blob, pc, A64_MOVK(reg, (uint16_t)(value >> (hw * 16u)), hw));
}

static void emit_yield_args(uint8_t *blob, unsigned *pc)
{
    for (unsigned reg = 0u; reg <= 5u; ++reg)
        emit(blob, pc, A64_MOVZ(reg, 0u, 0u));
    emit(blob, pc, A64_MOVZ(8u, JE_SYS_YIELD, 0u));
    emit(blob, pc, A64_SVC(0u));
}

static void emit_zero_syscall_args(uint8_t *blob, unsigned *pc)
{
    for (unsigned reg = 0u; reg <= 5u; ++reg)
        emit(blob, pc, A64_MOVZ(reg, 0u, 0u));
}

static void emit_expect(uint8_t *blob, unsigned *pc, unsigned reg,
                        uint64_t expected, unsigned *fail_branches,
                        unsigned *fail_branch_count)
{
    if (*fail_branch_count >= 64u) proof_fatal("branch-table-overflow");
    emit_load_u64(blob, pc, 22u, expected);
    emit(blob, pc, A64_CMP(reg, 22u));
    fail_branches[(*fail_branch_count)++] = *pc;
    emit(blob, pc, A64_BCOND(A64_BCOND_NE, 0));
}

static void emit_syscall(uint8_t *blob, unsigned *pc, uint64_t op)
{
    emit(blob, pc, A64_MOVZ(8u, (uint16_t)op, 0u));
    emit(blob, pc, A64_SVC(0u));
}

static void emit_ipc_yields(uint8_t *blob, unsigned *pc, unsigned count)
{
    for (unsigned i = 0u; i < count; ++i) emit_yield_args(blob, pc);
}

static void emit_ipc_seed_load(uint8_t *blob, unsigned *pc, unsigned reg)
{
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(reg, 1u, 0x20u));
}

static void emit_ipc_create(uint8_t *blob, unsigned *pc,
                            unsigned *fail_branches,
                            unsigned *fail_branch_count)
{
    emit_zero_syscall_args(blob, pc);
    emit_syscall(blob, pc, JE_SYSCALL_ENDPOINT_CREATE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit(blob, pc, A64_ADD(19u, 1u, 0u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_STR(19u, 1u, 0x28u)); /* packed c0/c1 */
}

static void emit_ipc_transfer(uint8_t *blob, unsigned *pc,
                              unsigned *fail_branches,
                              unsigned *fail_branch_count)
{
    /* x19 = a SEND|TRANSFER sibling of created c0; x20 = packed {c0,c1}.
     * The real two-cap message carries live c0 first and a separate seeded
     * source handle second. The seeded source is closed before the receiver
     * mints, so the second entry is the A5-2 NULL marker while c0 remains
     * usable end-to-end for B's reply over c1. */
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(20u, 1u, 0x28u));
    emit(blob, pc, A64_UBFM(19u, 20u, 0u, 31u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit(blob, pc, A64_MOVZ(1u, JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_TRANSFER, 0u));
    emit(blob, pc, A64_MOVZ(2u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_DUP);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit(blob, pc, A64_ADD(19u, 1u, 0u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(20u, 1u, 0x28u));
    /*
     * First exercise the source-right attenuation guard through the real
     * send-cap path: x19 has SEND|TRANSFER but not RECV, so SEND|RECV must
     * be denied before anything is enqueued.
     */
    emit(blob, pc, A64_MOVZ(20u,
                             JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV, 2u));
    emit(blob, pc, A64_ORR(20u, 20u, 19u));
    emit_load_data_pointer(blob, pc);
    /*
     * x4 points at the first cap descriptor in the transfer array. Keep the
     * descriptor and the pointer at the same arena offset so the endpoint
     * sees the intended sibling handle and rights.
     */
    emit(blob, pc, A64_STR(20u, 1u, 0x100u));
    emit(blob, pc, A64_MOVZ(22u, UINT16_C(0x3633), 0u));
    emit(blob, pc, A64_MOVK(22u, UINT16_C(0x4d50), 1u));
    emit(blob, pc, A64_STR(22u, 1u, 0x180u));
    emit(blob, pc, A64_MOVZ(2u, 4u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 1u, 0u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(4u, 1u, 0x100u));
    emit_ipc_seed_load(blob, pc, 0u); /* send over A's bootstrap end */
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x80u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_SEND);
    emit_expect(blob, pc, 0u, JE_ABI_H_DENIED,
                fail_branches, fail_branch_count);
    /* The attenuation sibling is no longer needed. Closing it before the
     * real enqueue leaves c0 with exactly one source handle to close below. */
    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);

    /* The attenuated SEND-only descriptors are the message actually delivered:
     * cap[0] = live created c0, cap[1] = the soon-to-be-closed seeded end. */
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(20u, 1u, 0x28u));
    emit(blob, pc, A64_UBFM(20u, 20u, 0u, 31u));
    emit(blob, pc, A64_MOVZ(24u, JE_ABI_RIGHT_SEND, 2u));
    emit(blob, pc, A64_ORR(20u, 20u, 24u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_STR(20u, 1u, 0x100u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(20u, 1u, 0x48u));
    emit(blob, pc, A64_MOVZ(24u, JE_ABI_RIGHT_SEND, 2u));
    emit(blob, pc, A64_ORR(20u, 20u, 24u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_STR(20u, 1u, 0x108u));
    emit(blob, pc, A64_MOVZ(2u, 4u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 2u, 0u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(4u, 1u, 0x100u));
    emit_ipc_seed_load(blob, pc, 0u);
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x80u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_SEND);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);

    /* The kernel test hook closes the separate seeded source immediately
     * after enqueue, before the awakened receiver can be scheduled. */
}

static void emit_ipc_blocking_recv(uint8_t *blob, unsigned *pc,
                                   unsigned *fail_branches,
                                   unsigned *fail_branch_count)
{
    emit_ipc_seed_load(blob, pc, 19u);
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x180u));
#if defined(JE_ARM_IPC_FAULT) && JE_ARM_IPC_FAULT == 7
    emit(blob, pc, A64_MOVZ(2u, 0u, 0u));
#else
    emit(blob, pc, A64_MOVZ(2u, 0x100u, 0u));
#endif
    emit(blob, pc, A64_MOVZ(3u, 0x1000u, 0u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(4u, 1u, 0x1c0u));
    emit(blob, pc, A64_MOVZ(5u, 2u, 0u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x180u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_RECV);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_ipc_reply(uint8_t *blob, unsigned *pc,
                           unsigned *fail_branches,
                           unsigned *fail_branch_count)
{
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(19u, 1u, 0x1c0u));
    emit(blob, pc, A64_UBFM(19u, 19u, 0u, 31u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit(blob, pc, A64_MOVZ(1u, JE_ABI_RIGHT_RECV, 0u));
    emit(blob, pc, A64_MOVZ(2u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_DUP);
    emit_expect(blob, pc, 0u, JE_ABI_H_DENIED,
                fail_branches, fail_branch_count);
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit(blob, pc, A64_MOVZ(1u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(2u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_SEND);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_ipc_expect_empty(uint8_t *blob, unsigned *pc,
                                  unsigned *fail_branches,
                                  unsigned *fail_branch_count)
{
    emit_ipc_seed_load(blob, pc, 19u);
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x1a0u));
    emit(blob, pc, A64_MOVZ(2u, 0x100u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_RECV);
    emit_expect(blob, pc, 0u, JE_ABI_H_EMPTY, fail_branches,
                fail_branch_count);
    emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);
}

static void emit_ipc_timed_out_recv(uint8_t *blob, unsigned *pc,
                                    unsigned *fail_branches,
                                    unsigned *fail_branch_count)
{
    emit_ipc_seed_load(blob, pc, 19u);
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x1a0u));
    emit(blob, pc, A64_MOVZ(2u, 0x100u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0x40u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_RECV);
    emit_expect(blob, pc, 0u, JE_ABI_H_TIMED_OUT, fail_branches,
                fail_branch_count);
    emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);
}

static void emit_ipc_all_blocked_recv(uint8_t *blob, unsigned *pc,
                                      uint64_t expected_status,
                                      bool send_after,
                                      unsigned *fail_branches,
                                      unsigned *fail_branch_count)
{
    emit_ipc_seed_load(blob, pc, 19u);
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x1a0u));
    emit(blob, pc, A64_MOVZ(2u, 0x100u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0x80u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_RECV);
    emit_expect(blob, pc, 0u, expected_status, fail_branches,
                fail_branch_count);
    if (expected_status == JE_ABI_OK)
        emit_expect(blob, pc, 1u, 1u, fail_branches, fail_branch_count);
    else
        emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);
    if (!send_after) return;

    /* A's timeout is the control edge. The one-byte send is deliberately a
     * normal user syscall after the peer waiter has been released. */
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x180u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit(blob, pc, A64_MOVZ(2u, 1u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_SEND);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_ipc_peer_closed_recv(uint8_t *blob, unsigned *pc,
                                      unsigned *fail_branches,
                                      unsigned *fail_branch_count)
{
    emit_ipc_seed_load(blob, pc, 19u);
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x1a0u));
    emit(blob, pc, A64_MOVZ(2u, 0x100u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0x1000u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_RECV);
    emit_expect(blob, pc, 0u, JE_ABI_H_PEER_CLOSED,
                fail_branches, fail_branch_count);
    emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);
}

static void emit_ipc_close_seed(uint8_t *blob, unsigned *pc,
                                unsigned *fail_branches,
                                unsigned *fail_branch_count)
{
    emit_ipc_seed_load(blob, pc, 19u);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_ipc_close_created(uint8_t *blob, unsigned *pc,
                                   unsigned *fail_branches,
                                   unsigned *fail_branch_count)
{
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(20u, 1u, 0x28u));
    emit(blob, pc, A64_UBFM(19u, 20u, 0u, 31u));
    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit(blob, pc, A64_UBFM(19u, 20u, 32u, 63u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_ipc_victim_send(uint8_t *blob, unsigned *pc,
                                 unsigned *fail_branches,
                                 unsigned *fail_branch_count)
{
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(19u, 1u, 0x40u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x180u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit(blob, pc, A64_MOVZ(2u, 1u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_SEND);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
#ifdef JE_ARM_EL0_FORCE_DEATH_RELEASE_RACE
    /*
     * Test-only CPU work between the victim send and its own close. A timer
     * interrupt can therefore reap the victim and run the replacement before
     * this process closes its source end. The kernel release ordering is still
     * checked by the oracle; this only makes the observation window repeatable.
     */
    emit_load_u64(blob, pc, 23u, UINT64_C(8000000));
    const unsigned race_spin_start = *pc;
    emit(blob, pc, A64_SUBS_IMM(23u, 23u, 1u));
    const unsigned race_spin_branch = *pc;
    emit(blob, pc, A64_BCOND(A64_BCOND_NE, 0));
    patch_pc_relative(blob, race_spin_branch, race_spin_start, true,
                      A64_BCOND_NE);
#endif
    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_ipc_reply_recv(uint8_t *blob, unsigned *pc,
                                unsigned *fail_branches,
                                unsigned *fail_branch_count)
{
    unsigned retry_start = *pc;
    unsigned empty_branch;
    unsigned exhausted_branch;
    unsigned retry_branch;
    unsigned after_retry;

    /* The reply is produced by B after the wake/restart.  A bounded retry
     * proves the message rather than relying on UART/timer timing. */
    emit(blob, pc, A64_MOVZ(23u, 64u, 0u));
    retry_start = *pc;
    emit(blob, pc, A64_MOVZ(8u, JE_SYSCALL_TRY_RECV, 0u));
    /* B uses the live c0 cap, whose peer is A's original c1. */
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_LDR(19u, 1u, 0x28u));
    emit(blob, pc, A64_UBFM(19u, 19u, 32u, 63u));
    emit_load_data_pointer(blob, pc);
    emit(blob, pc, A64_ADD(1u, 1u, 0x1a0u));
    emit(blob, pc, A64_MOVZ(2u, 0x100u, 0u));
    emit(blob, pc, A64_MOVZ(3u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, pc, A64_MOVZ(5u, 0u, 0u));
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_TRY_RECV);
    emit_load_u64(blob, pc, 22u, JE_ABI_H_EMPTY);
    emit(blob, pc, A64_CMP(0u, 22u));
    empty_branch = *pc;
    emit(blob, pc, A64_BCOND(A64_BCOND_NE, 0));
    emit(blob, pc, A64_SUBS_IMM(23u, 23u, 1u));
    exhausted_branch = *pc;
    emit(blob, pc, A64_BCOND(UINT32_C(0), 0));
    emit_yield_args(blob, pc);
    retry_branch = *pc;
    emit(blob, pc, A64_B(0));
    after_retry = *pc;
    patch_pc_relative(blob, empty_branch, after_retry, true, A64_BCOND_NE);
    patch_pc_relative(blob, exhausted_branch, after_retry, true, UINT32_C(0));
    patch_pc_relative(blob, retry_branch, retry_start, false, 0u);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_copy_probe(uint8_t *blob, unsigned *pc, uint64_t src,
                            uint64_t dst, uint64_t len,
                            unsigned *fail_branches,
                            unsigned *fail_branch_count)
{
    emit_load_u64(blob, pc, 0u, JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE + src);
    emit_load_u64(blob, pc, 1u, JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE + dst);
    emit_load_u64(blob, pc, 2u, len);
    for (unsigned reg = 3u; reg <= 5u; ++reg)
        emit(blob, pc, A64_MOVZ(reg, 0u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_COPY_PROBE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
}

static void emit_handle_round_trip(uint8_t *blob, unsigned *pc,
                                   unsigned *fail_branches,
                                   unsigned *fail_branch_count)
{
    /* SELF -> dup(SELF, 0): the duplicate is deliberately rights-attenuated. */
    emit_zero_syscall_args(blob, pc);
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_SELF);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit(blob, pc, A64_ADD(19u, 1u, 0u));

    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_DUP);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit(blob, pc, A64_ADD(20u, 1u, 0u));

    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 20u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_QUERY);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit_expect(blob, pc, 1u, JE_ABI_OBJ_SELF,
                fail_branches, fail_branch_count);

    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 20u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_RIGHTS);
    emit_expect(blob, pc, 0u, JE_ABI_H_DENIED,
                fail_branches, fail_branch_count);
    emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);

    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 20u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);

    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 20u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_H_STALE,
                fail_branches, fail_branch_count);
    emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);

    emit_zero_syscall_args(blob, pc);
    emit(blob, pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, pc, JE_SYSCALL_HANDLE_CLOSE);
    emit_expect(blob, pc, 0u, JE_ABI_OK, fail_branches, fail_branch_count);
    emit_expect(blob, pc, 1u, 0u, fail_branches, fail_branch_count);
}

static void emit_user_sp_check(uint8_t *blob, unsigned *pc,
                               uint64_t expected_sp,
                               unsigned *sp_fail_branches,
                               unsigned *sp_fail_count)
{
    emit(blob, pc, A64_ADD(22u, 31u, 0u)); /* x22 = current SP_EL0 */
    emit(blob, pc, A64_CMP(22u, 21u));
    sp_fail_branches[(*sp_fail_count)++] = *pc;
    emit(blob, pc, A64_BCOND(A64_BCOND_NE, 0));
    emit_load_u64(blob, pc, 20u, expected_sp);
    emit(blob, pc, A64_CMP(22u, 20u));
    sp_fail_branches[(*sp_fail_count)++] = *pc;
    emit(blob, pc, A64_BCOND(A64_BCOND_NE, 0));
}

static void make_good_blob(uint8_t *blob, uint16_t value,
                           uint64_t expected_sp, bool spin,
                           bool exits_after_signal)
{
    for (size_t i = 0u; i < 4u * JE_PAGE_SIZE; ++i) blob[i] = 0u;
    unsigned pc = 0u;
    unsigned fail_branches[64];
    unsigned fail_branch_count = 0u;
    unsigned sp_fail_branches[8];
    unsigned sp_fail_count = 0u;
    unsigned spin_start = 0u;
    unsigned spin_branch = 0u;
    unsigned exit_branch = 0u;
    unsigned loop_branch = 0u;
    unsigned yield_target = 0u;
    unsigned sp_fail_target = 0u;
    unsigned survivor_spin_start = 0u;
    unsigned survivor_spin_branch = 0u;
    unsigned survivor_spin_end = 0u;
    unsigned survivor_spin_skip = 0u;
    unsigned survivor_spin_entry = 0u;

    /*
     * Load a distinct non-zero value into every architectural GPR before the
     * proof SVC. x16-x18 are the explicit round-trip canary; x0 starts at the
     * proof op's required non-zero arg0. The proof op consumes the saved x0
     * argument and returns non-zero status/result values.
     */
    for (unsigned reg = 0u; reg < 31u; ++reg) {
        uint16_t value16 = (uint16_t)(UINT16_C(0x1000) + reg);
        if (reg == 16u) value16 = UINT16_C(0x1616);
        if (reg == 17u) value16 = UINT16_C(0x1717);
        if (reg == 18u) value16 = UINT16_C(0x1818);
        emit(blob, &pc, A64_MOVZ(reg, value16, 0u));
    }
    emit(blob, &pc, A64_ADD(21u, 31u, 0u)); /* record SP_EL0 at entry */
    emit_user_sp_check(blob, &pc, expected_sp, sp_fail_branches,
                       &sp_fail_count);
    emit(blob, &pc, A64_MRS_TPIDR_EL0(20u));
    const unsigned tpidr_branch = pc;
    emit(blob, &pc, A64_CMP(20u, 31u));
    emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
    emit(blob, &pc, A64_MOVZ(19u, UINT16_C(0x1919), 0u));
    emit(blob, &pc, A64_MOVZ(20u, UINT16_C(0x1020), 0u));
    emit(blob, &pc, A64_MRS_TPIDRRO_EL0(2u));
    emit(blob, &pc, A64_ADD(6u, 2u, 0u)); /* preserve it across later SVC args */

    if (spin) {
        emit(blob, &pc, A64_MOVZ(30u, JE_EL0_SPIN_COUNT, 1u));
        spin_start = pc;
        emit(blob, &pc, A64_SUBS_IMM(30u, 30u, 1u));
        spin_branch = pc;
        emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
        je_el0_spin_start = spin_start;
        je_el0_spin_end = pc;
    }

    emit_copy_probe(blob, &pc, JE_EL0_COPY_SRC0, JE_EL0_COPY_DST0, 0u,
                    fail_branches, &fail_branch_count);
    emit_copy_probe(blob, &pc, JE_EL0_COPY_SRC256, JE_EL0_COPY_DST256, 256u,
                    fail_branches, &fail_branch_count);
    emit_copy_probe(blob, &pc, JE_EL0_COPY_SRC255, JE_EL0_COPY_DST255, 255u,
                    fail_branches, &fail_branch_count);
    emit_handle_round_trip(blob, &pc, fail_branches, &fail_branch_count);

    emit_load_u64(blob, &pc, 0u, JE_ARM_EL0_PROOF_ARG0);
    for (unsigned reg = 1u; reg <= 5u; ++reg)
        emit(blob, &pc, A64_MOVZ(reg, 0u, 0u));
    emit(blob, &pc, A64_ADD(2u, 6u, 0u));
    emit_syscall(blob, &pc, JE_ARM_EL0_PROOF_OP);

    emit(blob, &pc, A64_MOVZ(20u, (uint16_t)JE_ARM_EL0_PROOF_STATUS, 0u));
    emit(blob, &pc, A64_CMP(0u, 20u));
    fail_branches[fail_branch_count++] = pc;
    emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
    emit(blob, &pc, A64_MOVZ(20u, (uint16_t)JE_ARM_EL0_PROOF_RESULT, 0u));
    emit(blob, &pc, A64_CMP(1u, 20u));
    fail_branches[fail_branch_count++] = pc;
    emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
    emit(blob, &pc, A64_MOVZ(20u, UINT16_C(0x1616), 0u));
    emit(blob, &pc, A64_CMP(16u, 20u));
    fail_branches[fail_branch_count++] = pc;
    emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
    emit(blob, &pc, A64_MOVZ(20u, UINT16_C(0x1717), 0u));
    emit(blob, &pc, A64_CMP(17u, 20u));
    fail_branches[fail_branch_count++] = pc;
    emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
    emit(blob, &pc, A64_MOVZ(20u, UINT16_C(0x1818), 0u));
    emit(blob, &pc, A64_CMP(18u, 20u));
    fail_branches[fail_branch_count++] = pc;
    emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
    emit_user_sp_check(blob, &pc, expected_sp, sp_fail_branches,
                       &sp_fail_count);

    /*
     * ARM IPC proof fixture.  B enters the blocking recv first; A spends a
     * bounded yield window letting the timer dispatch B into PARKED before it
     * creates/sends the transferred endpoint cap.  The endpoint handles are
     * kernel-seeded into each process's data page at offset 0x20.
     */
    emit_load_data_pointer(blob, &pc);
    emit(blob, &pc, A64_MOVZ(3u, value, 0u));
    emit(blob, &pc, A64_STR(3u, 1u, 0u));
    emit(blob, &pc, A64_MOVZ(3u, (uint16_t)JE_EL0_REGISTER_PASS, 0u));
    emit(blob, &pc, A64_STR(3u, 1u, 8u));
    if (spin) {
        emit_ipc_yields(blob, &pc, 64u);
        emit_ipc_create(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_transfer(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_reply_recv(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_close_created(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_all_blocked_recv(
            blob, &pc, JE_ABI_H_TIMED_OUT, true,
            fail_branches, &fail_branch_count);
        /* Let B register the second receive before the peer close. */
        emit_ipc_yields(blob, &pc, 64u);
        emit_ipc_close_seed(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_victim_send(blob, &pc, fail_branches, &fail_branch_count);
    } else {
        emit_ipc_blocking_recv(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_reply(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_expect_empty(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_timed_out_recv(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_all_blocked_recv(
            blob, &pc, JE_ABI_OK,
            false, fail_branches, &fail_branch_count);
        emit_ipc_peer_closed_recv(blob, &pc, fail_branches, &fail_branch_count);
        emit_ipc_close_seed(blob, &pc, fail_branches, &fail_branch_count);
    }
    /*
     * Leave A in a bounded CPU-only window after all IPC sends/closes.  B can
     * finish its final receive and exit while A is in this window; the next
     * timer IRQ must then reselect A as the lone READY/RUNNING survivor.
     * This is deliberately a real IRQ witness, not an SVC-yield substitute.
     */
    if (spin) {
        survivor_spin_skip = pc;
        emit(blob, &pc, A64_B(0));
        survivor_spin_entry = pc;
        emit(blob, &pc, A64_MOVZ(30u, JE_EL0_SURVIVOR_SPIN_COUNT, 1u));
        survivor_spin_start = pc;
        emit(blob, &pc, A64_SUBS_IMM(30u, 30u, 1u));
        survivor_spin_branch = pc;
        emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
        survivor_spin_end = pc;
        if (value == UINT16_C(0x1111)) {
            je_el0_survivor_spin_start = survivor_spin_entry;
            je_el0_survivor_spin_end = survivor_spin_end;
        }
    }

    emit_load_data_pointer(blob, &pc);
    emit(blob, &pc, A64_MOVZ(3u, value, 0u));
    emit(blob, &pc, UINT32_C(0xf9000023)); /* STR x3,[x1] */
    emit(blob, &pc, A64_MOVZ(3u, (uint16_t)JE_EL0_REGISTER_PASS, 0u));
    emit(blob, &pc, UINT32_C(0xf9000423)); /* STR x3,[x1,#8] */
    yield_target = pc;
    emit_yield_args(blob, &pc);
    emit_user_sp_check(blob, &pc, expected_sp, sp_fail_branches,
                       &sp_fail_count);

    if (exits_after_signal) {
        exit_branch = pc;
        emit(blob, &pc, A64_CMP(1u, 31u));
        emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
    }
    loop_branch = pc;
    emit(blob, &pc, A64_B(0));

    const unsigned exit_target = pc;
    if (exits_after_signal) {
        for (unsigned reg = 0u; reg <= 5u; ++reg)
            emit(blob, &pc, A64_MOVZ(reg, 0u, 0u));
        emit(blob, &pc, A64_MOVZ(0u, 7u, 0u));
        emit(blob, &pc, A64_MOVZ(8u, JE_SYS_EXIT, 0u));
        emit(blob, &pc, A64_SVC(0u));
        emit(blob, &pc, A64_B(0));
    }

    const unsigned fail_target = pc;
    emit_load_data_pointer(blob, &pc);
    emit(blob, &pc, A64_MOVZ(3u, (uint16_t)JE_EL0_REGISTER_FAIL, 0u));
    emit(blob, &pc, UINT32_C(0xf9000423)); /* STR x3,[x1,#8] */
    emit_yield_args(blob, &pc);
    emit(blob, &pc, A64_B(0));

    if (spin) patch_pc_relative(blob, spin_branch, spin_start, true,
                                A64_BCOND_NE);
    if (spin)
        patch_pc_relative(blob, survivor_spin_branch, survivor_spin_start,
                          true, A64_BCOND_NE);
    if (spin)
        patch_pc_relative(blob, survivor_spin_skip, survivor_spin_end,
                          false, 0u);
    patch_pc_relative(blob, tpidr_branch + sizeof(uint32_t), fail_target,
                      true, A64_BCOND_NE);
    for (unsigned i = 0u; i < fail_branch_count; ++i)
        patch_pc_relative(blob, fail_branches[i], fail_target, true,
                          A64_BCOND_NE);
    patch_pc_relative(blob, loop_branch, yield_target, false, 0u);
    if (exits_after_signal)
        patch_pc_relative(blob, exit_branch + sizeof(uint32_t), exit_target,
                          true, A64_BCOND_NE);

    sp_fail_target = pc;
    emit_load_data_pointer(blob, &pc);
    emit(blob, &pc, A64_MOVZ(3u, (uint16_t)JE_EL0_SP_FAIL, 0u));
    emit(blob, &pc, UINT32_C(0xf9000423)); /* STR x3,[x1,#8] */
    emit_yield_args(blob, &pc);
    emit(blob, &pc, A64_B(0));

    for (unsigned i = 0u; i < sp_fail_count; ++i)
        patch_pc_relative(blob, sp_fail_branches[i], sp_fail_target, true,
                          A64_BCOND_NE);

    for (unsigned i = 0u; i < 2u * JE_PAGE_SIZE; ++i) {
        uint8_t byte = (uint8_t)((value + (uint16_t)(i * 13u) + 1u) & 0xffu);
        if (byte == 0u) byte = 1u;
        blob[2u * JE_PAGE_SIZE + i] = byte;
    }
    for (unsigned i = 0u; i < sizeof(uint64_t); ++i)
        blob[2u * JE_PAGE_SIZE + i] = (uint8_t)((uint64_t)value >> (i * 8u));
}

static void make_death_waiter_blob(uint8_t *blob)
{
    for (size_t i = 0u; i < 4u * JE_PAGE_SIZE; ++i) blob[i] = 0u;
    unsigned pc = 0u;
    emit_ipc_seed_load(blob, &pc, 19u);
    emit_load_data_pointer(blob, &pc);
    emit(blob, &pc, A64_ADD(1u, 1u, 0x1a0u));
    emit(blob, &pc, A64_MOVZ(2u, 0x100u, 0u));
    emit(blob, &pc, A64_MOVZ(3u, 0x1000u, 0u));
    emit(blob, &pc, A64_MOVZ(4u, 0u, 0u));
    emit(blob, &pc, A64_MOVZ(5u, 0u, 0u));
    emit(blob, &pc, A64_ADD(0u, 19u, 0u));
    emit_syscall(blob, &pc, JE_SYSCALL_TRY_RECV);
    /*
     * The receiver exits normally after the real WOKEN resume.  The proof
     * must exercise lifecycle waiter release without the old test-only
     * kill-at-wake shortcut.
     */
    emit_zero_syscall_args(blob, &pc);
    emit_syscall(blob, &pc, JE_SYS_EXIT);
    const unsigned loop = pc;
    emit_yield_args(blob, &pc);
    const unsigned back = pc;
    emit(blob, &pc, A64_B(0u));
    patch_pc_relative(blob, back, loop, false, 0u);
}

static void make_reuse_blob(uint8_t *blob)
{
    for (size_t i = 0u; i < 4u * JE_PAGE_SIZE; ++i) blob[i] = 0u;
    unsigned pc = 0u;
    emit_yield_args(blob, &pc);
    emit_zero_syscall_args(blob, &pc);
    emit_syscall(blob, &pc, JE_SYS_EXIT);
    const unsigned loop = pc;
    emit_yield_args(blob, &pc);
    const unsigned back = pc;
    emit(blob, &pc, A64_B(0u));
    patch_pc_relative(blob, back, loop, false, 0u);
}

static void make_negative_blob(je_el0_negative kind)
{
    for (size_t i = 0u; i < sizeof(je_el0_negative_blob); ++i)
        je_el0_negative_blob[i] = 0u;
    unsigned pc = 0u;
    switch (kind) {
    case JE_EL0_NEG_KERNEL_READ:
        emit(je_el0_negative_blob, &pc, A64_MOVZ(1u, UINT16_C(0x4000), 1u));
        emit(je_el0_negative_blob, &pc, UINT32_C(0xf9400020)); /* LDR x0,[x1] */
        break;
    case JE_EL0_NEG_KERNEL_WRITE:
        emit(je_el0_negative_blob, &pc, A64_MOVZ(1u, UINT16_C(0x4000), 1u));
        emit(je_el0_negative_blob, &pc, UINT32_C(0xf9000020)); /* STR x0,[x1] */
        break;
    case JE_EL0_NEG_KERNEL_EXEC:
        emit(je_el0_negative_blob, &pc, A64_MOVZ(1u, UINT16_C(0x4000), 1u));
        emit(je_el0_negative_blob, &pc, UINT32_C(0xd63f0020)); /* BLR x1 */
        break;
    case JE_EL0_NEG_BAD_SP:
    case JE_EL0_NEG_BAD_ELR:
        emit(je_el0_negative_blob, &pc, A64_MOVZ(0u, 0u, 0u));
        emit(je_el0_negative_blob, &pc, A64_MOVZ(8u, JE_SYS_YIELD, 0u));
        emit(je_el0_negative_blob, &pc, A64_SVC(0u));
        emit(je_el0_negative_blob, &pc, A64_B(-3));
        break;
    case JE_EL0_NEG_FP:
        emit(je_el0_negative_blob, &pc, UINT32_C(0x1e204000)); /* FMOV s0,s0 */
        break;
    case JE_EL0_NEG_CNTVCT:
        emit(je_el0_negative_blob, &pc, UINT32_C(0xd53be040)); /* MRS x0,CNTVCT_EL0 */
        break;
    case JE_EL0_NEG_SVC_IMM:
        emit(je_el0_negative_blob, &pc, A64_MOVZ(8u, JE_SYS_ABI_QUERY, 0u));
        emit(je_el0_negative_blob, &pc, A64_SVC(1u));
        break;
    case JE_EL0_NEG_CNTP_CTL:
        emit(je_el0_negative_blob, &pc, A64_MOVZ(0u, 0u, 0u));
        emit(je_el0_negative_blob, &pc, A64_MSR_CNTP_CTL_EL0(0u));
        break;
    case JE_EL0_NEG_DCC:
        emit(je_el0_negative_blob, &pc, A64_MRS_MDCCSR_EL0(0u));
        break;
    case JE_EL0_NEG_COUNT:
        proof_fatal("negative-kind");
    }
}

static void image_init(void)
{
    je_el0_image = (je_vm_image){
        .version = JE_VM_IMAGE_VERSION,
        .segment_count = 2u,
        .entry_offset = 0u,
        .reserved = 0u,
        .segments = {
            { 0u, 2u * JE_PAGE_SIZE, 0u, 2u * JE_PAGE_SIZE,
              JE_VM_R | JE_VM_X, 0u },
            { 2u * JE_PAGE_SIZE, 2u * JE_PAGE_SIZE, 2u * JE_PAGE_SIZE,
              2u * JE_PAGE_SIZE,
              JE_VM_R | JE_VM_W, 0u },
        },
    };
}

static je_proc_id allocate_process(void)
{
    je_proc_id id = JE_PROC_ID_NONE;
    /*
     * The bootstrap channel is fixture-owned and uncharged.  A's proof then
     * creates exactly one additional channel, so give each test identity one
     * endpoint-creation credit and let the bridge enforce that charge.
     */
    if (je_proc_alloc(&je_el0_pool, 1u, false, &id) != JE_OK)
        proof_fatal("proc-alloc");
    return id;
}

static void build_user_vm(uint8_t *blob, uint32_t *out_index,
                          je_proc_id *out_proc)
{
    je_proc_id id = allocate_process();
    je_status status = je_vm_build(&je_el0_vm, &je_el0_image, blob,
                                   4u * JE_PAGE_SIZE, 1, out_index);
    if (status != JE_OK) proof_fatal("vm-build");
    *out_proc = id;
}

static void build_user(uint8_t *blob, uint32_t *out_index,
                       je_proc_id *out_proc, uint32_t *out_slot)
{
    build_user_vm(blob, out_index, out_proc);
    const je_status status = je_arm_sched_create_user(
        &je_el0_vm, *out_index, *out_proc, out_slot);
    if (status != JE_OK) proof_fatal("thread-create");
}

static void seed_user_u64(uint32_t index, uint64_t arena_offset,
                          uint64_t value)
{
    if (index >= JE_VM_MAX_PROCESSES ||
        (arena_offset & (sizeof(uint64_t) - 1u)) != 0u ||
        arena_offset >= JE_VM_ARENA_SIZE)
        proof_fatal("fixture-seed-bounds");
    je_vm_process *process = &je_el0_vm.procs[index];
    for (size_t n = 0u; n < process->journal_count; ++n) {
        const je_vm_journal_entry *entry = &process->journal[n];
        if (entry->kind != JE_VM_J_PTE ||
            arena_offset < entry->varena_off ||
            arena_offset >= entry->varena_off + JE_PAGE_SIZE)
            continue;
        const je_vm_ref ref = { index, entry->extent, entry->page };
        uint8_t *mapped = (uint8_t *)je_el0_vm.ops->map(
            je_el0_vm.ops->context, ref);
        if (!mapped) proof_fatal("fixture-seed-map");
        for (unsigned byte = 0u; byte < sizeof(value); ++byte)
            mapped[arena_offset - entry->varena_off + byte] =
                (uint8_t)(value >> (byte * 8u));
        je_el0_vm.ops->unmap(je_el0_vm.ops->context, ref);
        je_el0_vm.ops->flush(je_el0_vm.ops->context);
        return;
    }
    proof_fatal("fixture-seed-page");
}

static void spawn_negative(void)
{
    if (je_el0_negative_live || je_el0_active >= JE_EL0_NEG_COUNT)
        return;
    make_negative_blob(je_el0_active);
    uint32_t slot = UINT32_MAX;
    build_user(je_el0_negative_blob, &je_el0_negative_index,
               &je_el0_negative_proc, &slot);
    je_el0_negative_live = true;
    proof_write("JeiOS/aarch64: EL0 NEGATIVE spawn stage=");
    proof_dec((uint64_t)je_el0_active);
    proof_write("\r\n");
}

static uint8_t copy_pattern(uint32_t index, uint64_t offset)
{
    const uint16_t seed = index == 0u ? UINT16_C(0x1111) : UINT16_C(0x2222);
    uint8_t byte = (uint8_t)((seed + (uint16_t)(offset * 13u) + 1u) &
                             UINT16_C(0xff));
    return byte == 0u ? 1u : byte;
}

static void verify_copy_probe(uint32_t index, uint32_t current_slot,
                              uint64_t status)
{
    static const uint64_t lengths[JE_EL0_COPY_ROUND_COUNT] = { 0u, 256u, 255u };
    static const uint64_t sources[JE_EL0_COPY_ROUND_COUNT] = {
        JE_EL0_COPY_SRC0, JE_EL0_COPY_SRC256, JE_EL0_COPY_SRC255
    };
    static const uint64_t destinations[JE_EL0_COPY_ROUND_COUNT] = {
        JE_EL0_COPY_DST0, JE_EL0_COPY_DST256, JE_EL0_COPY_DST255
    };
    uint8_t source[256];
    uint8_t destination[256];
    const uint32_t round = je_el0_copy_round[index];
    if (round >= JE_EL0_COPY_ROUND_COUNT || status != JE_ABI_OK)
        proof_fatal("copy-probe-status");
    if (lengths[round] != 0u) {
        if (je_copy_from_user(source,
                              JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE +
                                  sources[round],
                              lengths[round]) != JE_ABI_OK ||
            je_copy_from_user(destination,
                              JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE +
                                  destinations[round],
                              lengths[round]) != JE_ABI_OK)
            proof_fatal("copy-probe-readback");
        for (uint64_t i = 0u; i < lengths[round]; ++i) {
            if (source[i] != copy_pattern(index, sources[round] + i) ||
                destination[i] != source[i])
                proof_fatal("copy-probe-bytes");
        }
    }
    ++je_el0_copy_round[index];
    if (round == JE_EL0_COPY_ROUND_COUNT - 1u) {
        proof_write("JeiOS/aarch64: EL0 COPY_PROBE_PASS slot=");
        proof_dec(current_slot);
        proof_write(" lengths=0,256,255\r\n");
    }
}

static void verify_handle_step(uint32_t index, uint32_t current_slot,
                               uint64_t op, const je_arm_trapframe *tf)
{
    const uint32_t step = je_el0_handle_step[index];
    if (tf == NULL) proof_fatal("handle-null");
    switch (step) {
    case 0u:
        if (op != JE_SYSCALL_HANDLE_SELF || tf->gpr[0] != JE_ABI_OK ||
            tf->gpr[1] == 0u)
            proof_fatal("handle-self");
        break;
    case 1u:
        if (op != JE_SYSCALL_HANDLE_DUP || tf->gpr[0] != JE_ABI_OK ||
            tf->gpr[1] == 0u)
            proof_fatal("handle-dup");
        break;
    case 2u:
        if (op != JE_SYSCALL_HANDLE_QUERY || tf->gpr[0] != JE_ABI_OK ||
            tf->gpr[1] != JE_ABI_OBJ_SELF)
            proof_fatal("handle-query");
        break;
    case 3u:
        if (op != JE_SYSCALL_HANDLE_RIGHTS ||
            tf->gpr[0] != JE_ABI_H_DENIED || tf->gpr[1] != 0u)
            proof_fatal("handle-rights");
        break;
    case 4u:
        if (op != JE_SYSCALL_HANDLE_CLOSE || tf->gpr[0] != JE_ABI_OK ||
            tf->gpr[1] != 0u)
            proof_fatal("handle-close");
        break;
    case 5u:
        if (op != JE_SYSCALL_HANDLE_CLOSE ||
            tf->gpr[0] != JE_ABI_H_STALE || tf->gpr[1] != 0u)
            proof_fatal("handle-stale");
        break;
    case 6u:
        if (op != JE_SYSCALL_HANDLE_CLOSE || tf->gpr[0] != JE_ABI_OK ||
            tf->gpr[1] != 0u)
            proof_fatal("handle-self-close");
        proof_write("JeiOS/aarch64: EL0 HANDLE_ROUND_TRIP_PASS slot=");
        proof_dec(current_slot);
        proof_write(" type=SELF attenuated=0 stale=3\r\n");
        break;
    default:
        proof_fatal("handle-step");
    }
    ++je_el0_handle_step[index];
}

static void prove_copy_fixup(uint32_t current_slot)
{
    if (je_el0_copy_fixup_proven) return;
    je_el0_copy_fixup_proven = true;
    if (je_arm_copy_fixup_count() != 0u)
        proof_fatal("copy-count-prevalidation");
#if defined(JE_ARM_COPY_FAULT_STAGE)
    (void)current_slot;
#if JE_ARM_COPY_FAULT_STAGE == 10
    /*
     * Real copy_core proof: keep process 0 current, install process 1's root,
     * and copy from the shared arena VA.  The live-root guard must fail before
     * LDTRB can silently read process 1's distinct data value.
     */
    uint64_t observed = 0u;
    const uint32_t other_slot = je_el0_good_slot[1];
    if (other_slot == 0u || je_arm_user_root[other_slot] == 0u)
        proof_fatal("copy-live-root-setup");
    je_arm_sched_install_ttbr0(je_arm_user_root[other_slot], 0u);
    const je_abi_status status = je_copy_from_user(
        &observed,
        JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE,
        sizeof(observed));
    if (status == JE_ABI_OK && observed == UINT64_C(0x2222))
        proof_fatal("copy-live-root-recovered");
    proof_fatal("copy-live-root-returned");
#elif JE_ARM_COPY_FAULT_STAGE == 12
    /*
     * Guard state 2 is load-bearing: the base build reaches the stack-free
     * double-fault halt for the nested exact-site fault injected inside the C
     * predicate.  The paired guard-state mutant reaches this assertion if it
     * incorrectly recovers that nested fault.
     */
    if (je_arm_copy_fixup_inject() != 0)
        proof_fatal("copy-negative-recovered");
    proof_fatal("copy-negative-returned");
#else
    if (je_arm_copy_fixup_inject() != 0)
        proof_fatal("copy-negative-recovered");
    proof_fatal("copy-negative-returned");
#endif
#elif defined(JE_ARM_COPY_FAULT_MUTANT_RESTORE_SELECTED)
    (void)current_slot;
    if (je_arm_copy_fixup_inject() == 0)
        proof_fatal("copy-fixup");
    if (je_arm_in_handler != 0u)
        proof_fatal("copy-guard-cleared");
#else
    const int recovered = je_arm_copy_fixup_inject();
    if (recovered == 0 ||
        je_arm_copy_fixup_count() != 2u ||
        je_arm_in_handler != 1u) {
        proof_fatal("copy-fixup");
    }
    proof_write("JeiOS/aarch64: EL0 COPY_FIXUP_RECOVERED slot=");
    proof_dec(current_slot);
    proof_write(" count=2 guard=1\r\n");
#endif
}

void je_arm_el0_test_note_kstack(uint32_t slot)
{
    for (unsigned i = 0u; i < 2u; ++i) {
        if (je_el0_good_slot[i] == slot && !je_el0_sp_el1_marker[i]) {
            je_el0_sp_el1_marker[i] = true;
            proof_write("JeiOS/aarch64: EL0 SP_EL1 trap frame verified slot=");
            proof_dec(slot);
            proof_write("\r\n");
        }
    }
}

void je_arm_el0_test_note_wait_terminal(uint32_t index,
                                        je_ep_channel *rec,
                                        uint8_t side,
                                        je_ep_wait_cause cause)
{
    if (cause == JE_EP_WAIT_PEER_CLOSED) {
        if (!je_el0_ipc_peer_closed_seen ||
            index != je_el0_good_index[1] ||
            je_endpoint_end_state(rec, side) != JE_EP_END_PEER_CLOSED ||
            je_endpoint_inbound_count(rec, side) != 0u)
            proof_fatal("peer-closed terminal");
        je_el0_death_live = true;
        return;
    }
    (void)index;
}

void je_arm_el0_test_note_short_buffer(uint32_t index,
                                       je_ep_channel *rec,
                                       uint8_t side,
                                       size_t required_bytes,
                                       uint8_t required_caps)
{
    (void)rec;
    (void)side;
#if defined(JE_ARM_IPC_FAULT) && JE_ARM_IPC_FAULT == 7
    if (index != je_el0_good_index[1] || required_bytes == 0u)
        proof_fatal("short-buffer marker");
    je_el0_short_buffer_marker_seen = true;
    proof_write("JeiOS/aarch64: EL0 IPC SHORT_BUFFER trusted-cause required=");
    proof_dec(required_bytes);
    proof_write(" caps=");
    proof_dec(required_caps);
    proof_write("\r\n");
#else
    (void)index;
    (void)required_bytes;
    (void)required_caps;
#endif
}

uint32_t je_arm_el0_test_short_buffer_slot(void)
{
    return je_el0_good_slot[1];
}

void je_arm_el0_test_note_recv_caps(uint32_t index,
                                    je_ep_channel *rec,
                                    uint8_t side,
                                    uint8_t count,
                                    const je_ep_capdesc *caps,
                                    const je_handle *minted,
                                    const bool *was_live)
{
    (void)rec;
    (void)side;
    if (index != je_el0_good_index[1] || count == 0u) return;
    if (count != 2u || caps == NULL || minted == NULL ||
        was_live == NULL || !was_live[0] || was_live[1] ||
        minted[0] == 0u || minted[1] != 0u ||
        !je_endpoint_kern_capdesc_live(&caps[0]) ||
        je_endpoint_kern_capdesc_live(&caps[1])) {
        proof_fatal("deadcap state");
    }
    je_el0_ipc_deadcap_verified = true;
    uint32_t handle_index = 0u;
    je_handle_decode(minted[0], &handle_index, NULL);
    je_el0_ipc_deadcap_slot = handle_index;
    je_el0_ipc_deadcap_pending = true;
}

void je_arm_el0_test_note_ipc_send(je_ep_channel *rec, uint8_t side)
{
    if (!je_el0_deadcap_source_closed) {
        je_vm *vm = NULL;
        uint32_t index = UINT32_MAX;
        uint32_t slot = UINT32_MAX;
        if (je_arm_sched_current_user(&vm, &index, &slot) == JE_OK &&
            index == je_el0_good_index[0]) {
            if (je_endpoint_kern_close(vm, index, je_el0_deadcap_source)
                    != JE_EP_OK)
                proof_fatal("deadcap-source-close");
            je_el0_deadcap_source_closed = true;
        }
    }
    if (je_el0_ipc_deadcap_pending) {
        je_vm *vm = NULL;
        uint32_t index = UINT32_MAX;
        uint32_t slot = UINT32_MAX;
        if (je_arm_sched_current_user(&vm, &index, &slot) == JE_OK &&
            index == je_el0_good_index[1]) {
            je_el0_ipc_deadcap_pending = false;
            je_el0_ipc_deadcap_seen = true;
            proof_write("JeiOS/aarch64: EL0 IPC DEADCAP_NULL_MARKER slot=");
            proof_dec(je_el0_ipc_deadcap_slot);
            proof_write(" live_sibling=ok\r\n");
        }
    }
    (void)rec;
    (void)side;
}

void je_arm_el0_test_note_ipc_close(je_ep_channel *rec, uint8_t side,
                                    je_handle h)
{
    if (h != je_el0_deadcap_source) return;
    je_vm *vm = NULL;
    uint32_t index = UINT32_MAX;
    uint32_t slot = UINT32_MAX;
    if (je_arm_sched_current_user(&vm, &index, &slot) != JE_OK ||
        index != je_el0_good_index[0])
        return;
    if (!rec || side > 1u ||
        je_endpoint_end_state(rec, side) != JE_EP_END_CLOSED)
        proof_fatal("deadcap-source-close");
    if (je_el0_good_slot[1] == UINT32_MAX)
        proof_fatal("ipc-order-release");
}

void je_arm_el0_test_note_ipc_wake(je_ep_channel *rec, uint8_t side,
                                   uint32_t waiter_id)
{
    if (!je_el0_death_live) return;
    if (!je_arm_sched_ipc_was_blocked(waiter_id))
        proof_fatal("death wake not blocked");
    je_el0_death_rec = rec;
    je_el0_death_side = side;
    je_el0_death_waiter_id = waiter_id;
    je_el0_death_generation = rec->generation;
#ifndef JE_ARM_EL0_DISABLE_WAKE_KILL
    je_el0_death_kill_pending = true;
#endif
}

void je_arm_el0_test_note_peer_closed(je_ep_channel *rec, uint8_t side,
                                      uint32_t waiter_id)
{
    if (!je_arm_sched_ipc_was_blocked(waiter_id) ||
        je_endpoint_end_state(rec, side) != JE_EP_END_PEER_CLOSED ||
        je_endpoint_inbound_count(rec, side) != 0u)
        proof_fatal("peer-closed arbitration");
    je_el0_ipc_peer_closed_seen = true;
    je_el0_death_live = true;
    proof_write("JeiOS/aarch64: EL0 IPC PEER_CLOSED "
                "trusted-cause blocked-before-ready=1\r\n");
}

void je_arm_el0_test_note_all_blocked_state(void)
{
    if (je_el0_good_slot[0] == UINT32_MAX ||
        je_el0_good_slot[1] == UINT32_MAX ||
        !je_arm_sched_ipc_was_blocked(je_el0_good_slot[0]) ||
        !je_arm_sched_ipc_was_blocked(je_el0_good_slot[1]))
        return;
    je_el0_all_blocked_state_seen = true;
}

void je_arm_el0_test_note_all_blocked_idle(uint32_t slot)
{
    if (je_el0_ipc_idle_seen) return;
    if (!je_el0_all_blocked_state_seen ||
        (slot != je_el0_good_slot[0] && slot != je_el0_good_slot[1]))
        return;
    je_el0_ipc_idle_seen = true;
    proof_write("JeiOS/aarch64: EL0 IPC ALL_BLOCKED_IDLE woke=1\r\n");
}

void je_arm_el0_test_note_idle_guard(uint32_t slot)
{
    if (!je_el0_ipc_idle_seen || slot == 0u)
        proof_fatal("idle guard witness");
    if (je_el0_idle_guard_seen) return;
    je_el0_idle_guard_seen = true;
    proof_write("JeiOS/aarch64: EL0 IPC IDLE_STACK_GUARD "
                "bss_overwrites=0 stack_guard=1 resumed=");
    proof_dec(slot);
    proof_write("\r\n");
}

void je_arm_el0_test_note_timer_wake(uint32_t slot)
{
    if (slot == je_el0_good_slot[1] && !je_el0_ipc_timer_wake_seen) {
        je_el0_ipc_timer_wake_seen = true;
        proof_write("JeiOS/aarch64: EL0 IPC TIMER_WAKE "
                    "trusted-cause\r\n");
    }
}

bool je_arm_el0_test_take_wake_death(uint32_t slot)
{
    /*
     * The default proof kills the dedicated death waiter at the real WOKEN
     * boundary. A separate no-kill profile leaves it to resume and exercise
     * the same frame path without hiding a scheduler overwrite.
     */
#ifdef JE_ARM_EL0_DISABLE_WAKE_KILL
    (void)slot;
    return false;
#else
    if (!je_el0_death_kill_pending || slot != je_el0_death_slot)
        return false;
    je_el0_death_killed = true;
    je_el0_death_kill_pending = false;
    return true;
#endif
}

void je_arm_el0_test_note_svc_proof(uint32_t slot,
                                    const je_arm_trapframe *tf)
{
    if (tf == NULL || tf->gpr[0] != JE_ARM_EL0_PROOF_STATUS ||
        tf->gpr[1] != JE_ARM_EL0_PROOF_RESULT ||
        tf->gpr[2] != JE_EL0_TPIDRRO_EXPECTED)
        proof_fatal("svc-proof-result");
    proof_write("JeiOS/aarch64: EL0 SVC register proof slot=");
    proof_dec(slot);
    proof_write(" status=");
    proof_dec(tf->gpr[0]);
    proof_write(" result=");
    proof_hex(tf->gpr[1]);
    proof_write(" x16=");
    proof_hex(tf->gpr[16]);
    proof_write(" x17=");
    proof_hex(tf->gpr[17]);
    proof_write(" x18=");
    proof_hex(tf->gpr[18]);
    proof_write(" tpidrro=");
    proof_hex(tf->gpr[2]);
    proof_write("\r\n");
    proof_write("JeiOS/aarch64: EL0 TPIDRRO_ROUND_TRIP slot=");
    proof_dec(slot);
    proof_write(" value=");
    proof_hex(tf->gpr[2]);
    proof_write("\r\n");
}

void je_arm_el0_test_note_irq_switch(uint32_t from_slot,
                                     uint32_t to_slot, uint64_t elr)
{
    if (je_el0_good_slot[0] == from_slot &&
        je_el0_good_slot[1] == to_slot &&
        elr >= JE_VM_ARENA_BASE + je_el0_spin_start &&
        elr < JE_VM_ARENA_BASE + je_el0_spin_end) {
        je_el0_irq_preempted = true;
        ++je_el0_irq_preemptions;
    }
    if (je_el0_survivor_reaped &&
        from_slot == je_el0_good_slot[0] &&
        to_slot == je_el0_good_slot[0] &&
        !je_el0_lone_irq_reselect) {
        je_el0_lone_irq_reselect = true;
        proof_write("JeiOS/aarch64: EL0 LONE_IRQ_RESELECT slot=");
        proof_dec(to_slot);
        proof_write("\r\n");
    }
}

void je_arm_el0_test_note_svc(uint32_t slot, uint64_t op,
                              je_arm_trapframe *tf)
{
    if (tf == NULL) proof_fatal("svc-null");
#ifdef JE_ARM_COPY_FAULT_MUTANT_RESTORE_SELECTED
    if (je_arm_copy_restore_selected_seen != 0u)
        proof_fatal("copy-guard-cleared");
#endif
    (void)slot;
    if (op == JE_ARM_EL0_PROOF_OP) {
        if (tf->gpr[0] != JE_ARM_EL0_PROOF_STATUS ||
            tf->gpr[1] != JE_ARM_EL0_PROOF_RESULT)
            proof_fatal("svc-proof-status");
        return;
    }
    bool woken_restart = false;
    if (op == JE_SYSCALL_TRY_RECV)
        woken_restart = je_arm_sched_ipc_woken_restart(slot);
#if defined(JE_ARM_IPC_FAULT) && JE_ARM_IPC_FAULT == 7
    if (op == JE_SYSCALL_TRY_RECV && woken_restart &&
        tf->gpr[0] == JE_ABI_H_SHORT_BUFFER) {
        if (!je_el0_short_buffer_marker_seen)
            proof_fatal("short-buffer trusted marker missing");
        proof_fatal("short-buffer resume");
    }
#endif
    if (op == JE_SYSCALL_TRY_RECV &&
        slot == je_el0_good_slot[1]) {
        if (tf->gpr[0] == JE_ABI_OK &&
            (tf->gpr[1] & UINT64_C(0xffffffff)) == 4u &&
            (tf->gpr[1] >> 32) == 2u &&
            !je_el0_ipc_woken_seen) {
            if (!woken_restart)
                proof_fatal("woken restart witness");
            je_el0_ipc_woken_seen = true;
            proof_write("JeiOS/aarch64: EL0 IPC WOKEN_RESTART "
                        "trusted-cause\r\n");
        }
        if (tf->gpr[0] == JE_ABI_H_TIMED_OUT &&
            tf->gpr[1] == 0u &&
            !je_el0_ipc_timeout_seen) {
            je_el0_ipc_timeout_seen = true;
            proof_write("JeiOS/aarch64: EL0 IPC TIMED_OUT "
                        "trusted-cause\r\n");
        }
        if (tf->gpr[0] == JE_ABI_H_PEER_CLOSED &&
            slot == je_el0_good_slot[1]) {
            je_el0_death_live = true;
        }
    }
    je_vm *vm = NULL;
    uint32_t index = UINT32_MAX;
    uint32_t current_slot = UINT32_MAX;
    const bool copy_op = op == JE_SYSCALL_COPY_PROBE;
    const bool handle_op = op >= JE_SYSCALL_HANDLE_SELF &&
                           op <= JE_SYSCALL_HANDLE_DUP;
    const bool close_op = op == JE_SYSCALL_HANDLE_CLOSE;
    const bool reuse_probe = je_el0_reuse_live &&
                             (op == JE_SYSCALL_TRY_RECV ||
                              op == JE_SYS_YIELD);
    if (copy_op || handle_op || close_op || op == JE_SYS_YIELD ||
        op == JE_SYS_EXIT || reuse_probe) {
        if (je_arm_sched_current_user(&vm, &index, &current_slot) != JE_OK)
            proof_fatal("current-user");
    }
    if (reuse_probe &&
        index == je_el0_reuse_index &&
        current_slot == je_el0_reuse_slot) {
        uint32_t stale_waiter_id = 0u;
        const bool rec_freed = je_el0_death_rec != NULL &&
                               !je_el0_death_rec->allocated;
        const je_ep_end_state death_state =
            je_el0_death_rec == NULL
                ? JE_EP_END_FREE
                : je_endpoint_end_state(je_el0_death_rec,
                                        je_el0_death_side);
        const uint8_t survivor_side =
            (uint8_t)(je_el0_death_side ^ 1u);
        const bool victim_waiter_present =
            je_el0_death_rec != NULL &&
            je_el0_death_rec->end[je_el0_death_side].waiter.present;
        const bool bad_release =
            je_el0_death_rec == NULL ||
            ((!rec_freed) &&
             (death_state != JE_EP_END_CLOSED ||
              je_el0_death_rec->generation != je_el0_death_generation ||
              je_endpoint_end_state(je_el0_death_rec, survivor_side) !=
                  JE_EP_END_PEER_CLOSED ||
              victim_waiter_present ||
              je_el0_death_rec->end[0].inflight_refs != 0u ||
              je_el0_death_rec->end[1].inflight_refs != 0u)) ||
            (rec_freed &&
             je_el0_death_rec->generation == je_el0_death_generation) ||
            (rec_freed && death_state != JE_EP_END_FREE) ||
            je_endpoint_wait_id(je_el0_death_rec, je_el0_death_side,
                                &stale_waiter_id) ||
            je_endpoint_kern_waiters_released() <=
                je_el0_death_waiters_before ||
            je_sched_wait_pending(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                  NULL);
        if (bad_release) {
            proof_fatal("death-release");
        }
        je_el0_reuse_marker = true;
        je_el0_ipc_death_seen = true;
        je_el0_reuse_live = false;
        je_el0_reuse_proc_done = true;
        proof_write("JeiOS/aarch64: EL0 IPC DEATH_RELEASED_WAITER "
                    "rec_freed=");
        proof_dec(rec_freed ? 1u : 0u);
        proof_write(" no_stale_id=1\r\n");
        return;
    }
    if (copy_op) {
        if (index >= 2u) proof_fatal("copy-index");
        verify_copy_probe(index, current_slot, tf->gpr[0]);
        if (index == 0u &&
            je_el0_copy_round[index] == JE_EL0_COPY_ROUND_COUNT)
            prove_copy_fixup(current_slot);
        return;
    }
    if (handle_op) {
        if (index >= 2u) proof_fatal("handle-index");
        if (je_el0_handle_step[index] >= 7u)
            return;
        verify_handle_step(index, current_slot, op, tf);
        return;
    }
    if (op != JE_SYS_YIELD) return;
    if (je_el0_negative_live && index == je_el0_negative_index) return;
    if (index < 2u) {
        uint64_t marker;
        if (je_copy_from_user(&marker,
                              JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE + 8u,
                              sizeof(marker)) != JE_ABI_OK)
            proof_fatal("marker-copy");
        if (marker == JE_EL0_SP_FAIL)
            proof_fatal("user-sp");
        if (marker != JE_EL0_REGISTER_PASS)
            proof_fatal("register-round-trip");
        if ((je_el0_register_roundtrip_mask & (UINT32_C(1) << index)) == 0u) {
            je_el0_register_roundtrip_mask |= UINT32_C(1) << index;
            proof_write("JeiOS/aarch64: EL0 REGISTER_ROUND_TRIP slot=");
            proof_dec(current_slot);
            proof_write(" marker=");
            proof_hex(marker);
            proof_write("\r\n");
        }
        if (je_el0_irq_preempted && current_slot == je_el0_good_slot[1])
            je_el0_irq_peer_ran = true;
        if (je_el0_irq_preempted && je_el0_irq_peer_ran &&
            !je_el0_irq_marker_printed) {
            je_el0_irq_marker_printed = true;
            proof_write("JeiOS/aarch64: EL0 TIMER_PREEMPT from=");
            proof_dec(je_el0_good_slot[0]);
            proof_write(" to=");
            proof_dec(je_el0_good_slot[1]);
            proof_write(" switches=");
            proof_dec(je_el0_irq_preemptions);
            proof_write("\r\n");
        }
        if (je_copy_from_user(&je_el0_good_value[index],
                              JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE,
                              sizeof(je_el0_good_value[index])) != JE_ABI_OK)
            proof_fatal("value-copy");
        je_el0_good_yield_mask |= UINT32_C(1) << index;
        proof_write("JeiOS/aarch64: EL0 RUN slot=");
        proof_dec(current_slot);
        proof_write(" data=");
        proof_hex(je_el0_good_value[index]);
        proof_write(" arena=");
        proof_hex(JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE);
        proof_write("\r\n");
        if (je_el0_good_yield_mask == 3u &&
            je_el0_good_value[0] != je_el0_good_value[1])
            spawn_negative();
        if (je_el0_survivor_reaped && current_slot == je_el0_good_slot[0]) {
            ++je_el0_lone_yields;
            proof_write("JeiOS/aarch64: EL0 LONE_SURVIVOR_YIELD slot=");
            proof_dec(current_slot);
            proof_write(" count=");
            proof_dec(je_el0_lone_yields);
            proof_write("\r\n");
        }
        if (je_el0_good_yield_mask == 3u &&
            je_el0_active == JE_EL0_NEG_COUNT &&
            !je_el0_negative_live && je_el0_survivor_reaped &&
            je_el0_register_roundtrip_mask == 3u &&
            je_el0_irq_marker_printed && je_el0_ipc_timer_wake_seen &&
            je_el0_mode_negative_passed &&
            je_el0_lone_yields >= 2u &&
            je_el0_ipc_woken_seen && je_el0_ipc_timeout_seen &&
            je_el0_ipc_peer_closed_seen && je_el0_ipc_deadcap_seen &&
            je_el0_ipc_deadcap_verified &&
            je_el0_ipc_idle_seen &&
#ifndef JE_ARM_EL0_DISABLE_WAKE_KILL
            je_el0_ipc_death_seen && je_el0_reuse_marker &&
#endif
            je_el0_lone_irq_reselect) {
            proof_write("JeiOS/aarch64: EL0 proof PASS "
                        "reviewed-cache-tlb=not-observed\r\n");
            proof_halt();
        }
    }
}

void je_arm_el0_test_before_return(uint32_t slot, je_arm_trapframe *tf)
{
    if (tf == NULL) return;
    je_vm *vm = NULL;
    uint32_t index = UINT32_MAX;
    uint32_t current_slot = UINT32_MAX;
    if (je_arm_sched_current_user(&vm, &index, &current_slot) != JE_OK ||
        current_slot != slot)
        return;
    if (je_el0_negative_live && index == je_el0_negative_index &&
        je_el0_active == JE_EL0_NEG_BAD_SP) {
        *je_arm_tf_user_sp(tf) = JE_VM_ARENA_BASE;
        proof_write("JeiOS/aarch64: EL0 BAD_RETURN sp-mutated slot=");
        proof_dec(slot);
        proof_write("\r\n");
    } else if (je_el0_negative_live && index == je_el0_negative_index &&
               je_el0_active == JE_EL0_NEG_BAD_ELR) {
        tf->elr = (uint64_t)(uintptr_t)&je_arm_el0_test_before_return;
        proof_write("JeiOS/aarch64: EL0 BAD_RETURN elr-mutated slot=");
        proof_dec(slot);
        proof_write("\r\n");
    }
    if (index == je_el0_good_index[1] &&
        je_el0_active == JE_EL0_NEG_COUNT && !je_el0_survivor_reaped)
        tf->gpr[1] = JE_EL0_EXIT_SIGNAL;
    if (index == je_el0_good_index[0] &&
        je_el0_active == JE_EL0_NEG_COUNT &&
        !je_el0_survivor_spin_armed &&
        je_el0_survivor_spin_end > je_el0_survivor_spin_start) {
        tf->elr = JE_VM_ARENA_BASE + je_el0_survivor_spin_start;
        je_el0_survivor_spin_armed = true;
    }
}

void je_arm_el0_test_note_fault(uint32_t slot, uint32_t reason,
                                uint64_t ec, uint64_t far, uint64_t il,
                                uint64_t iss, uint64_t fsc, uint64_t wnr)
{
    if (je_el0_reuse_proc_done && slot == je_el0_reuse_slot &&
        reason == JE_ABI_DEATH_EXIT)
        return;
    if (!je_el0_negative_live) return;
    if (je_el0_active == JE_EL0_NEG_CNTP_CTL) {
        if (ec != UINT64_C(0x18) || iss != JE_EL0_CNTP_CTL_ISS)
            proof_fatal("cntkctl-trap-syndrome");
        proof_write("JeiOS/aarch64: EL0 CNTKCTL_PREEMPT_WRITE_TRAP stage=");
        proof_dec((uint64_t)je_el0_active);
        proof_write("\r\n");
    } else if (je_el0_active == JE_EL0_NEG_DCC) {
        if (ec != UINT64_C(0x18) || iss != JE_EL0_DCC_ISS)
            proof_fatal("dcc-trap-syndrome");
        proof_write("JeiOS/aarch64: EL0 DCC_CHANNEL_TRAP stage=");
        proof_dec((uint64_t)je_el0_active);
        proof_write("\r\n");
    }
    je_el0_seen_fault = true;
    proof_write("JeiOS/aarch64: EL0 EXPECTED_DEATH stage=");
    proof_dec((uint64_t)je_el0_active);
    proof_write(" slot=");
    proof_dec(slot);
    proof_write(" reason=");
    proof_dec(reason);
    proof_write(" ec=");
    proof_hex(ec);
    proof_write(" far=");
    proof_hex(far);
    proof_write(" il=");
    proof_dec(il);
    proof_write(" iss=");
    proof_hex(iss);
    proof_write(" fsc=");
    proof_hex(fsc);
    proof_write(" wnr=");
    proof_dec(wnr);
    proof_write("\r\n");
}

void je_arm_el0_test_note_reaped(uint32_t index, je_proc_id proc_id)
{
#ifdef JE_ARM_EL0_DISABLE_WAKE_KILL
    /*
     * This profile exists solely to prove that the wake-resumed context can
     * return through the scheduler without the old kill-at-wake shortcut.
     * Leave lifecycle reuse to the default profile, whose real death path
     * proves waiter release at the teardown boundary.
     */
    if (je_el0_death_live &&
        index == je_el0_death_index &&
        proc_id.index == je_el0_death_proc.index &&
        proc_id.gen == je_el0_death_proc.gen) {
        je_el0_death_live = false;
        return;
    }
#endif
    if (je_el0_death_live &&
        index == je_el0_death_index &&
        proc_id.index == je_el0_death_proc.index &&
        proc_id.gen == je_el0_death_proc.gen) {
        if (je_el0_death_rec == NULL ||
            je_el0_death_waiter_id != je_el0_death_slot ||
            je_el0_death_side > 1u)
            proof_fatal("death-record");
        je_el0_death_live = false;
        je_el0_reuse_live = false;
        make_reuse_blob(je_el0_death_blob);
        uint32_t reuse_slot = UINT32_MAX;
        build_user(je_el0_death_blob, &je_el0_reuse_index,
                   &je_el0_reuse_proc, &reuse_slot);
        if (je_el0_reuse_index != je_el0_death_index ||
            reuse_slot != je_el0_death_slot ||
            je_el0_reuse_proc.index != je_el0_death_proc.index ||
            je_el0_reuse_proc.gen == je_el0_death_proc.gen)
            proof_fatal("death-slot-reuse");
        je_el0_reuse_slot = reuse_slot;
        je_el0_reuse_live = true;
        return;
    }
    if (je_el0_reuse_proc_done &&
        index == je_el0_reuse_index &&
        proc_id.index == je_el0_reuse_proc.index &&
        proc_id.gen == je_el0_reuse_proc.gen) {
        je_el0_reuse_proc_done = false;
        return;
    }
    if (!je_el0_negative_live) {
        if (index != je_el0_good_index[1] ||
            proc_id.index != je_el0_good_proc[1].index ||
            proc_id.gen != je_el0_good_proc[1].gen ||
            je_el0_survivor_reaped)
            proof_fatal("reap-mismatch");
        je_el0_survivor_reaped = true;
        proof_write("JeiOS/aarch64: EL0 REAP_WITH_SURVIVOR lone slot=");
        proof_dec(je_el0_good_slot[1]);
        proof_write("\r\n");
        return;
    }
    if (index != je_el0_negative_index ||
        proc_id.index != je_el0_negative_proc.index ||
        proc_id.gen != je_el0_negative_proc.gen || !je_el0_seen_fault)
        proof_fatal("reap-mismatch");
    je_el0_passed[je_el0_active] = true;
    je_el0_negative_live = false;
    je_el0_seen_fault = false;
    ++je_el0_active;
    proof_write("JeiOS/aarch64: EL0 REAP_WITH_SURVIVOR stage=");
    proof_dec((uint64_t)(je_el0_active - 1u));
    proof_write("\r\n");
}

void je_arm_el0_test_poison_tpidr(void)
{
    const uint64_t poison = UINT64_C(0x5a5a5a5a5a5a5a5a);
    const uint64_t sp_poison = JE_EL0_SP_POISON;
    __asm__ volatile("msr tpidr_el0, %0\n\t"
                     "msr tpidrro_el0, %0\n\t"
                     "msr sp_el0, %1\n\t"
                     "isb"
                     : : "r"(poison), "r"(sp_poison) : "memory");
}

_Noreturn void je_arm_el0_test_run(void)
{
    image_init();
    je_proc_pool_init(&je_el0_pool);
    je_sched_set_proc_pool(&je_el0_pool);
    je_arm_sched_set_proc_pool(&je_el0_pool);
    const je_vm_ops *ops = je_arch_vm_ops(je_arm_scheduler_pages(), 1);
    if (je_vm_init(&je_el0_vm, ops, je_arm_scheduler_pages(), 0u) != JE_OK)
        proof_fatal("vm-init");
    je_endpoint_kern_set_proc_pool(&je_el0_pool);
    je_endpoint_kern_install(&je_el0_vm);
    make_good_blob(je_el0_good_blob[0], UINT16_C(0x1111),
                   JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE, true, false);
    make_good_blob(je_el0_good_blob[1], UINT16_C(0x2222),
                   JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE - UINT64_C(0x40),
                   false, true);
    uint32_t slots[2];
    build_user(je_el0_good_blob[0], &je_el0_good_index[0],
               &je_el0_good_proc[0], &slots[0]);
    build_user(je_el0_good_blob[1], &je_el0_good_index[1],
               &je_el0_good_proc[1], &slots[1]);
    je_el0_good_slot[0] = slots[0];
    je_el0_good_slot[1] = slots[1];
    je_handle seed_a = 0u;
    je_handle seed_b = 0u;
    if (je_endpoint_kern_seed(&je_el0_vm, je_el0_good_index[0],
                              je_el0_good_index[1], &seed_a, &seed_b)
            != JE_EP_OK ||
        seed_a == 0u || seed_b == 0u)
        proof_fatal("ipc-seed");
    seed_user_u64(je_el0_good_index[0], 2u * JE_PAGE_SIZE + 0x20u,
                  seed_a);
    seed_user_u64(je_el0_good_index[1], 2u * JE_PAGE_SIZE + 0x20u,
                  seed_b);
    const uint32_t cap_endpoint_baseline =
        je_endpoint_kernel_allocated();
    if (cap_endpoint_baseline != 1u)
        proof_fatal("cap-baseline");
    je_handle dead_source = 0u;
    je_handle dead_peer = 0u;
    if (je_endpoint_kern_seed(&je_el0_vm, je_el0_good_index[0],
                              je_el0_good_index[1], &dead_source, &dead_peer)
            != JE_EP_OK ||
        dead_source == 0u || dead_peer == 0u)
        proof_fatal("deadcap-seed");
    je_el0_deadcap_source = dead_source;
    seed_user_u64(je_el0_good_index[0], 2u * JE_PAGE_SIZE + 0x48u,
                  dead_source);
    seed_user_u64(je_el0_good_index[1], 2u * JE_PAGE_SIZE + 0x28u,
                  dead_peer);
    make_death_waiter_blob(je_el0_death_blob);
    uint32_t death_slot = UINT32_MAX;
    build_user(je_el0_death_blob, &je_el0_death_index,
               &je_el0_death_proc, &death_slot);
    je_el0_death_slot = death_slot;
    je_handle death_a = 0u;
    je_handle death_b = 0u;
    if (je_endpoint_kern_seed(&je_el0_vm, je_el0_good_index[0],
                              je_el0_death_index, &death_a, &death_b)
            != JE_EP_OK ||
        death_a == 0u || death_b == 0u)
        proof_fatal("death-seed");
    seed_user_u64(je_el0_good_index[0], 2u * JE_PAGE_SIZE + 0x40u,
                  death_a);
    seed_user_u64(je_el0_death_index, 2u * JE_PAGE_SIZE + 0x20u,
                  death_b);
    /* The bootstrap and dead-cap records are live at this point; G1 closes
     * the first, B closes the second after minting, and the transferred
     * endpoint is closed/released by the G2 commit. */
    je_el0_death_waiters_before =
        je_endpoint_kern_waiters_released();
    const uint32_t mode_negative =
        je_arm_el0_test_mode_rejection(slots[0]);
    if (mode_negative != UINT32_C(0xf))
        proof_fatal("mode-negative");
    je_el0_mode_negative_passed = true;
    proof_write("JeiOS/aarch64: EL0 MODE_CHECK "
                "EL1h-frame-rejected-as-EL0\r\n");
    proof_write("JeiOS/aarch64: EL0 MODE_CHECK "
                "vector9-EL1h-rejected\r\n");
    proof_write("JeiOS/aarch64: EL0 MODE_CHECK "
                "vector5-EL0t-rejected\r\n");
    proof_write("JeiOS/aarch64: EL0 MODE_CHECK "
                "aarch32-M4-composite-rejected\r\n");
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= UINT64_C(3) << 20;
    __asm__ volatile("msr cpacr_el1, %0\n\tisb" : : "r"(cpacr) : "memory");
    proof_write("JeiOS/aarch64: EL0 setup two-processes real-code timer-live\r\n");
    if (je_timer_arm(NULL, NULL) != JE_OK)
        proof_fatal("timer-rearm");
    je_arm_sched_start_user();
}
