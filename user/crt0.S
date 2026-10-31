/* SPDX-License-Identifier: Apache-2.0 */
.section .text.start,"ax",@progbits
.globl _start
.type _start,@function
_start:
    /* Startup v1 supplies only the six C argument registers. */
    orq %rax, %r11
    orq %rbx, %r11
    orq %rbp, %r11
    orq %r10, %r11
    orq %r12, %r11
    orq %r13, %r11
    orq %r14, %r11
    orq %r15, %r11
    jnz .Lstartup_bad
    call je_main
    movq %rax, %rdi
    movq $2, %rax
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    xorq %r8, %r8
    xorq %r9, %r9
    int $0x80
.Lstartup_bad:
    ud2
.size _start, .-_start
