/* Multiboot2 enters 32-bit protected mode. This small assembly bridge is
 * necessary even though the OS core is C. Bootstrap identity maps the first
 * GiB with 2 MiB RWX supervisor pages. Replace these mappings in milestone M1.
 * Boot CPU must implement CPUID, PAE, PSE and long mode. GRUB supplies A20.
 */
.section .multiboot2,"a"
.balign 8
header_start:
.long 0xe85250d6
.long 0
.long header_end - header_start
.long -(0xe85250d6 + (header_end - header_start))
.short 0, 0
.long 8
header_end:

.section .text.boot,"ax"
.code32
.global _start
.type _start, @function
_start:
    cli
    cld
    movl $stack_top, %esp
    movl %eax, saved_magic
    movl %ebx, saved_info
    /* AMD64 implies CPUID support. Check long mode before using its MSR. */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb boot_fail
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz boot_fail
    movl $pdpt, %eax
    orl $3, %eax
    movl %eax, pml4
    movl $pd, %eax
    orl $3, %eax
    movl %eax, pdpt
    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, pd(,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4
    movl $pml4, %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr
    lgdt gdt_pointer
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0
    ljmp $8, $long_entry
boot_fail:
    hlt
    jmp boot_fail
.code64
long_entry:
    movw $16, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movq $stack_top, %rsp
    xorq %rbp, %rbp
    movl saved_magic(%rip), %edi
    movl saved_info(%rip), %esi
    call je_kernel_main
2:  cli
    hlt
    jmp 2b
.size _start, . - _start

.section .rodata,"a"
.balign 8
gdt:
.quad 0
.quad 0x00af9a000000ffff
.quad 0x00cf92000000ffff
gdt_pointer:
.word 23
.long gdt

.section .bss,"aw",@nobits
.balign 4096
pml4: .skip 4096
pdpt: .skip 4096
pd: .skip 4096
saved_magic: .skip 4
saved_info: .skip 4
.balign 4096
.global je_boot_guard
je_boot_guard: .skip 4096
stack_bottom: .skip 32768
stack_top:
.section .note.GNU-stack,"",@progbits
