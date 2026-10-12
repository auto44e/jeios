/* GPR preservation stress: busy until PIT tick 64, no cooperative yield. */
.section .text,"ax"
.global je_arch_register_probe
je_arch_register_probe:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq $16,%rax
    movq $17,%rbx
    movq $18,%rcx
    movq $19,%rdx
    movq $20,%rbp
    movq $21,%rsi
    movq $22,%rdi
    movq $23,%r8
    movq $24,%r9
    movq $25,%r10
    movq $26,%r11
    movq $27,%r12
    movq $28,%r13
    movq $29,%r14
    movq $30,%r15
1:
    cmpq $16,%rax
    jne 2f
    cmpq $17,%rbx
    jne 2f
    cmpq $18,%rcx
    jne 2f
    cmpq $19,%rdx
    jne 2f
    cmpq $20,%rbp
    jne 2f
    cmpq $21,%rsi
    jne 2f
    cmpq $22,%rdi
    jne 2f
    cmpq $23,%r8
    jne 2f
    cmpq $24,%r9
    jne 2f
    cmpq $25,%r10
    jne 2f
    cmpq $26,%r11
    jne 2f
    cmpq $27,%r12
    jne 2f
    cmpq $28,%r13
    jne 2f
    cmpq $29,%r14
    jne 2f
    cmpq $30,%r15
    jne 2f
    cmpq $64,je_timer_ticks(%rip)
    jae 3f
    pause
    jmp 1b
2:
    movl $1,%eax
    jmp 4f
3:
    xorl %eax,%eax
4:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.section .note.GNU-stack,"",@progbits
