/* SPDX-License-Identifier: Apache-2.0 */
/* Intel IA-32e frames are normalized to [vector,error,RIP,CS,RFLAGS,RSP,SS].
 * A CPU error code is already present only for the listed architectural vectors.
 * Software yield uses vector 0x81, never an error-code exception vector. */
.section .text,"ax"
.code64
.altmacro
.macro JE_ISR number
je_isr_\number:
.if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
    pushq $0
.endif
    pushq $\number
    jmp je_trap_common
.endm
.set vector,0
.rept 256
JE_ISR %vector
.set vector,vector+1
.endr
je_trap_common:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp,%rdi
    andq $-16,%rsp
    call je_trap_dispatch
    movq %rax,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16,%rsp
    iretq
.section .rodata,"a"
.balign 8
.global je_isr_table
je_isr_table:
.macro JE_ISR_POINTER number
    .quad je_isr_\number
.endm
.set vector,0
.rept 256
JE_ISR_POINTER %vector
.set vector,vector+1
.endr
.section .text,"ax"
.global je_arch_test_stack_overflow
je_arch_test_stack_overflow:
    movq %rdi,%rsp
    pushq $0
    ud2
.section .note.GNU-stack,"",@progbits

