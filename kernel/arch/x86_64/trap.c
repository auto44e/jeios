#include <stddef.h>
#include <stdint.h>
#include <jeios/arch.h>
#include <jeios/trap.h>
#include <jeios/sched.h>
/* Intel SDM Vol.3A: 64-bit interrupt handling, IST and TSS; Vol.2: LIDT/LTR.
 * Source references and ABI rationale are in docs/IMPLEMENTATION.md. */
struct __attribute__((packed)) gate {
    uint16_t low, selector; uint8_t ist, attributes;
    uint16_t mid; uint32_t high, reserved;
};
struct __attribute__((packed)) table_pointer { uint16_t limit; uint64_t base; };
struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
};
_Static_assert(sizeof(struct gate) == 16, "IDT entry layout");
_Static_assert(sizeof(struct tss64) == 104, "TSS layout");
_Static_assert(sizeof(je_trap_frame) == 176, "trap frame size");
_Static_assert(offsetof(je_trap_frame, vector) == 120, "assembly frame offset");
static struct gate idt[256] __attribute__((aligned(16)));
static uint64_t gdt[5] __attribute__((aligned(16)));
static struct tss64 tss;
/* First page becomes unmapped guard in M1b; next four pages hold the stack. */
unsigned char je_df_stack[5 * 4096] __attribute__((aligned(4096)));
unsigned char je_nmi_stack[5 * 4096] __attribute__((aligned(4096)));
extern void (*je_isr_table[256])(void);
void je_arch_traps_init(void)
{
    gdt[1] = UINT64_C(0x00af9a000000ffff);
    gdt[2] = UINT64_C(0x00cf92000000ffff);
    tss.ist[0] = (uintptr_t)je_df_stack + sizeof(je_df_stack);
    tss.ist[1] = (uintptr_t)je_nmi_stack + sizeof(je_nmi_stack);
    tss.iomap = (uint16_t)sizeof(tss); /* No I/O bitmap within the TSS limit. */
    uint64_t base = (uintptr_t)&tss;
    gdt[3] = UINT64_C(103) | ((base & 0xffffff) << 16) |
             (UINT64_C(0x89) << 40) | (((base >> 24) & 0xff) << 56);
    gdt[4] = base >> 32;
    struct table_pointer gp = {(uint16_t)(sizeof(gdt)-1), (uintptr_t)gdt};
    __asm__ volatile("lgdt %0; ltr %1" : : "m"(gp), "r"((uint16_t)24) : "memory");
    for (size_t i = 0; i < 256; ++i) {
        uint64_t address = (uintptr_t)je_isr_table[i];
        idt[i] = (struct gate){(uint16_t)address, 8,
            (uint8_t)(i == 8 ? 1 : (i == 2 ? 2 : 0)), 0x8e,
            (uint16_t)(address >> 16), (uint32_t)(address >> 32), 0};
    }
    struct table_pointer ip = {(uint16_t)(sizeof(idt)-1), (uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(ip) : "memory");
}
static volatile unsigned panicking;
je_trap_frame *je_trap_dispatch(je_trap_frame *f)
{
    if (f->vector==32) {
        je_arch_timer_eoi(); return je_sched_interrupt(f,true);
    }
    if (f->vector==0x81) return je_sched_interrupt(f,false);
    if (panicking++) je_arch_halt();
    uint64_t cr2 = 0;
    if (f->vector == 14) __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    je_arch_console_write("PANIC vector="); je_arch_console_hex(f->vector);
    je_arch_console_write(" error="); je_arch_console_hex(f->error);
    je_arch_console_write(" rip="); je_arch_console_hex(f->rip);
    je_arch_console_write(" cr2="); je_arch_console_hex(cr2);
    je_arch_console_write("\r\nJeiOS fatal: exception; halted\r\n");
    je_arch_halt();
}
void je_arch_test_double_fault(void)
{
    /* #PF while its gate is absent raises #NP during delivery, escalating to
     * #DF. Unlike `int $8`, this creates the CPU's real error-code frame. */
    idt[14].attributes = 0;
    __asm__ volatile("movq (%%rax),%%rax" : : "a"(UINT64_C(0x40000000)) : "memory");
}
