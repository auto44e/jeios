/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_TRAP_H
#define JEIOS_TRAP_H
#include <stdint.h>
/* Exact assembly push order; IA-32e always saves SS:RSP, including CPL0.
 * No user transitions, SIMD, swapgs, nesting/recovery or SMP supported yet. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, rflags, rsp, ss;
} je_trap_frame;
void je_arch_traps_init(void);
je_trap_frame *je_trap_dispatch(je_trap_frame *frame);
void je_arch_test_double_fault(void);
_Noreturn void je_arch_test_stack_overflow(uintptr_t guard_end);
extern unsigned char je_df_stack[], je_nmi_stack[];
#endif

