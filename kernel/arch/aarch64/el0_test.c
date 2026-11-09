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
#include <jeios/lifecycle.h>
#include <jeios/proc.h>
#include <jeios/status.h>
#include <jeios/vm.h>

#include "paging.h"
#include "trapframe.h"

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
extern _Noreturn void je_arm_sched_start_user(void);
extern const je_vm_ops *je_arch_vm_ops(je_pages *pages, int nx_available);

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
#define A64_CMP(rn, rm) \
    (UINT32_C(0xeb00001f) | ((uint32_t)(rm) << 16) | ((uint32_t)(rn) << 5))
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
#define JE_EL0_SPIN_COUNT UINT16_C(0x0100)
#define JE_EL0_TPIDRRO_EXPECTED UINT64_C(0)
#define JE_EL0_SP_POISON (JE_VM_ARENA_BASE + UINT64_C(0x100))
#define JE_EL0_CNTP_CTL_ISS UINT64_C(0x000000000032f804)
#define JE_EL0_DCC_ISS UINT64_C(0x000000000020c003)

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
static uint8_t je_el0_good_blob[2][2u * JE_PAGE_SIZE]
    __attribute__((aligned(16)));
static uint8_t je_el0_negative_blob[2u * JE_PAGE_SIZE]
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
static unsigned je_el0_spin_start;
static unsigned je_el0_spin_end;

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
    if (*pc > JE_PAGE_SIZE - sizeof(uint32_t)) proof_fatal("blob-overflow");
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
    for (size_t i = 0u; i < 2u * JE_PAGE_SIZE; ++i) blob[i] = 0u;
    unsigned pc = 0u;
    unsigned fail_branches[8];
    unsigned fail_branch_count = 0u;
    unsigned sp_fail_branches[8];
    unsigned sp_fail_count = 0u;
    unsigned spin_start = 0u;
    unsigned spin_branch = 0u;
    unsigned exit_branch = 0u;
    unsigned loop_branch = 0u;
    unsigned yield_target = 0u;
    unsigned sp_fail_target = 0u;

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

    if (spin) {
        emit(blob, &pc, A64_MOVZ(30u, JE_EL0_SPIN_COUNT, 1u));
        spin_start = pc;
        emit(blob, &pc, A64_SUBS_IMM(30u, 30u, 1u));
        spin_branch = pc;
        emit(blob, &pc, A64_BCOND(A64_BCOND_NE, 0));
        je_el0_spin_start = spin_start;
        je_el0_spin_end = pc;
    }

    emit(blob, &pc, A64_MOVZ(8u, JE_ARM_EL0_PROOF_OP, 0u));
    emit(blob, &pc, A64_SVC(0u));

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

    for (unsigned i = 0u; i < sizeof(uint64_t); ++i)
        blob[JE_PAGE_SIZE + i] = (uint8_t)((uint64_t)value >> (i * 8u));
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
            { 0u, JE_PAGE_SIZE, 0u, JE_PAGE_SIZE, JE_VM_R | JE_VM_X, 0u },
            { JE_PAGE_SIZE, JE_PAGE_SIZE, 2u * JE_PAGE_SIZE, JE_PAGE_SIZE,
              JE_VM_R | JE_VM_W, 0u },
        },
    };
}

static je_proc_id allocate_process(void)
{
    je_proc_id id = JE_PROC_ID_NONE;
    if (je_proc_alloc(&je_el0_pool, 0u, false, &id) != JE_OK)
        proof_fatal("proc-alloc");
    return id;
}

static void build_user(uint8_t *blob, uint32_t *out_index,
                       je_proc_id *out_proc, uint32_t *out_slot)
{
    je_proc_id id = allocate_process();
    je_status status = je_vm_build(&je_el0_vm, &je_el0_image, blob,
                                   2u * JE_PAGE_SIZE, 1, out_index);
    if (status != JE_OK) proof_fatal("vm-build");
    status = je_arm_sched_create_user(&je_el0_vm, *out_index, id, out_slot);
    if (status != JE_OK) proof_fatal("thread-create");
    *out_proc = id;
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
    (void)slot;
    if (op == JE_ARM_EL0_PROOF_OP) {
        if (tf->gpr[0] != JE_ARM_EL0_PROOF_STATUS ||
            tf->gpr[1] != JE_ARM_EL0_PROOF_RESULT)
            proof_fatal("svc-proof-status");
        return;
    }
    if (op != JE_SYS_YIELD) return;
    je_vm *vm = NULL;
    uint32_t index = UINT32_MAX;
    uint32_t current_slot = UINT32_MAX;
    if (je_arm_sched_current_user(&vm, &index, &current_slot) != JE_OK)
        proof_fatal("current-user");
    if (je_el0_negative_live && index == je_el0_negative_index) return;
    if (index < 2u) {
        const volatile uint64_t *marker =
            (const volatile uint64_t *)(uintptr_t)
                (JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE + 8u);
        if (*marker == JE_EL0_SP_FAIL)
            proof_fatal("user-sp");
        if (*marker != JE_EL0_REGISTER_PASS)
            proof_fatal("register-round-trip");
        if ((je_el0_register_roundtrip_mask & (UINT32_C(1) << index)) == 0u) {
            je_el0_register_roundtrip_mask |= UINT32_C(1) << index;
            proof_write("JeiOS/aarch64: EL0 REGISTER_ROUND_TRIP slot=");
            proof_dec(current_slot);
            proof_write(" marker=");
            proof_hex(*marker);
            proof_write("\r\n");
        }
        if (je_el0_irq_preempted && current_slot == je_el0_good_slot[1])
            je_el0_irq_peer_ran = true;
        const volatile uint64_t *value =
            (const volatile uint64_t *)(uintptr_t)
                (JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE);
        je_el0_good_value[index] = *value;
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
            je_el0_irq_marker_printed && je_el0_mode_negative_passed &&
            je_el0_lone_yields >= 2u && je_el0_lone_irq_reselect) {
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
}

void je_arm_el0_test_note_fault(uint32_t slot, uint32_t reason,
                                uint64_t ec, uint64_t far, uint64_t il,
                                uint64_t iss, uint64_t fsc, uint64_t wnr)
{
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
    je_arm_sched_start_user();
}
