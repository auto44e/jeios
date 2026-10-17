/* SPDX-License-Identifier: Apache-2.0 */
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
static uint64_t gdt[7] __attribute__((aligned(16)));
static struct tss64 tss;
/* First page becomes unmapped guard in M1b; next four pages hold the stack. */
unsigned char je_df_stack[5 * 4096] __attribute__((aligned(4096)));
unsigned char je_nmi_stack[5 * 4096] __attribute__((aligned(4096)));
extern void (*je_isr_table[256])(void);
void je_arch_traps_init(void)
{
    gdt[1] = UINT64_C(0x00af9a000000ffff);
    gdt[2] = UINT64_C(0x00cf92000000ffff);
    /* Ring-3 descriptors (DPL=3), appended after the kernel/TSS selectors so the
     * existing kernel selectors keep their values (ADR 0007 D6). No user task
     * executes yet (M3a #4 step 1); these only make the selectors available.
     *   gdt[5] user data  (selector 0x2b = index 5, RPL 3): present, DPL3, R/W.
     *   gdt[6] user code  (selector 0x33 = index 6, RPL 3): present, DPL3, L, exec/read. */
    gdt[5] = UINT64_C(0x00cff2000000ffff);
    gdt[6] = UINT64_C(0x00affa000000ffff);
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
    /* M3a #4 step 4 (ADR 0007 D6): one dedicated DPL3 interrupt gate at vector
     * 0x80, IST=0, selector 8 — the only user-reachable gate. attributes 0xee =
     * present, DPL3, 64-bit INTERRUPT gate (clears IF on entry; NOT 0xef/trap).
     * There is no syscall handler yet, so a user int 0x80 is an unsupported op
     * routed to fault-local termination; only the DPL differs from every other
     * gate. Kernel-only yield stays at 0x81 DPL0. */
    idt[0x80].attributes = 0xee;
    struct table_pointer ip = {(uint16_t)(sizeof(idt)-1), (uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(ip) : "memory");
}
static volatile unsigned panicking;
/* M3a #4 step 3: TSS.RSP0 is the kernel stack the CPU loads on a CPL3->CPL0
 * trap. The scheduler sets it to the next user thread's per-process kernel stack
 * top before returning that thread's frame; a kernel thread sets 0 (never
 * consulted for CPL0->CPL0, fail-closed if ever wrongly used). */
void je_arch_tss_set_rsp0(uint64_t rsp0) { tss.rsp[0] = rsp0; }
uint64_t je_arch_tss_get_rsp0(void) { return tss.rsp[0]; }

/* M3a #4 step 3 (ADR 0007 D7): architectural denial of x87/MMX/SSE/AVX and
 * extended state, so hostile ring-3 FP/SIMD traps (#NM/#UD) instead of executing
 * or leaking register state. CR0.EM=1 (no x87, SSE #UDs), MP=1, TS=1 (FP use
 * #NMs); CR4.OSFXSR/OSXMMEXCPT/OSXSAVE=0 (SSE not OS-enabled). The kernel is
 * built -mgeneral-regs-only and emits no FP/SIMD, so this is safe for kernel
 * code. Called once after paging init, before any user thread exists. */
void je_arch_cpu_policy_init(void)
{
    uintptr_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 |= (uintptr_t)1 << 2;   /* EM */
    cr0 |= (uintptr_t)1 << 1;   /* MP */
    cr0 |= (uintptr_t)1 << 3;   /* TS */
    __asm__ volatile("mov %0,%%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 &= ~((uintptr_t)1 << 9);   /* OSFXSR */
    cr4 &= ~((uintptr_t)1 << 10);  /* OSXMMEXCPT */
    cr4 &= ~((uintptr_t)1 << 18);  /* OSXSAVE */
    cr4 &= ~((uintptr_t)1 << 16);  /* FSGSBASE: no user FS/GS-base API (ADR D6 no-TLS) */
    __asm__ volatile("mov %0,%%cr4" : : "r"(cr4) : "memory");
    /* Normalize data selectors for the no-TLS profile (ADR 0007 D6): IRETQ to
     * CPL3 loads SS from the frame but leaves DS/ES/FS/GS at their current
     * values, so clear them to the null selector now. The kernel never reads
     * through these in 64-bit mode (flat model); a user process gets no inherited
     * kernel data selector. */
    __asm__ volatile("mov %0,%%ds; mov %0,%%es; mov %0,%%fs; mov %0,%%gs"
                     : : "r"((uint16_t)0) : "memory");
}
/* Test-only readback so the step-3 boot check can prove the policy took. */
uint64_t je_arch_cpu_policy_cr0(void) { uintptr_t v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
uint64_t je_arch_cpu_policy_cr4(void) { uintptr_t v; __asm__ volatile("mov %%cr4,%0":"=r"(v)); return v; }

/* M3a #4 step 4: synchronous exception vectors that terminate the offending
 * USER process (fault-local). NMI(2), #DF(8), #MC(18) and reserved vectors are
 * NOT here — they stay fatal even from ring 3. */
static int user_terminable_vector(uint64_t v)
{
    switch (v) {
    case 0: case 1: case 3: case 4: case 5: case 6: case 7:
    case 10: case 11: case 12: case 13: case 14: case 16: case 17: case 19:
        return 1;
    default:
        return 0;
    }
}

je_trap_frame *je_trap_dispatch(je_trap_frame *f)
{
    if (f->vector==32) {
        je_arch_timer_eoi(); return je_sched_interrupt(f,true);
    }
    if (f->vector==0x81) return je_sched_interrupt(f,false);
    uint64_t cr2 = 0;
    if (f->vector == 14) __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    /* Fault-local death: a synchronous fault from ring 3, or a user int 0x80
     * (the DPL3 gate; no syscall handler yet), terminates ONLY that process.
     * User origin requires the saved CS to be the user selector — a kernel-origin
     * fault on any of these vectors stays fatal. je_sched_fault_terminate adds
     * the kind/stack-location cross-checks. */
    if ((f->cs & 3) == 3 && (f->vector==0x80 || user_terminable_vector(f->vector)))
        return je_sched_fault_terminate(f, cr2);
    if (panicking++) je_arch_halt();
    je_arch_console_write("PANIC vector="); je_arch_console_hex(f->vector);
    je_arch_console_write(" error="); je_arch_console_hex(f->error);
    je_arch_console_write(" rip="); je_arch_console_hex(f->rip);
    je_arch_console_write(" cs="); je_arch_console_hex(f->cs);
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

