/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP1 step 1 (ADR 0002): the C entry for the ARM64 (QEMU virt-8.2) bring-up.
 * Reached from boot.S after EL1 is confirmed, the stack is set, TCR_EL1 is
 * initialized (TBI0=0 interim) and BSS is cleared. This file drives a bounded
 * PL011 polling console and emits the ordered step-1 markers, then halts.
 *
 * Scope boundaries (ADR 0002 / MP1 decomposition, locked by the design pass):
 *   - NO VBAR / exception vectors yet (next PR);
 *   - NO DTB *parse* — the pointer is only RECORDED, never dereferenced;
 *   - NO MMU, no atomics; single CPU.
 * The handoff records that the D5 trap-frame fields, VBAR, fault fixups, and DTB
 * validation are not yet enforced.
 *
 * Built -mstrict-align -mgeneral-regs-only -ffreestanding -fno-builtin, so the
 * compiler emits neither unaligned accesses (Device memory pre-MMU faults them)
 * nor FP/SIMD (which would trap into absent vectors and hang silently).
 */
#include <stdint.h>

/* DTB physical address, stashed by boot.S into .data BEFORE the BSS clear so it
 * survives (a .bss slot would have been zeroed). Recorded, not parsed. */
uint64_t je_arm_dtb_phys __attribute__((section(".data"))) = 0;

/* ---- PL011 UART @ 0x09000000 (confirmed from the QEMU virt DTB). Bounded,
 * DR-write + TXFF-poll console. No IBRD/FBRD/LCR/CR init: on this pinned target
 * QEMU accepts DR writes without programming, which is a documented virt-8.2
 * support-tier assumption, not a portable UART driver. The TXFF poll is bounded
 * so a wedged UART can never block the boot forever. ---- */
#define PL011_BASE 0x09000000UL
#define PL011_DR   (PL011_BASE + 0x00)
#define PL011_FR   (PL011_BASE + 0x18)
#define PL011_FR_TXFF (1u << 5)   /* transmit FIFO full */

static inline void mmio_w32(uint64_t addr, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)addr = v;
}
static inline uint32_t mmio_r32(uint64_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void console_putc(char c)
{
    /* Bounded wait for TX space: spin at most N times on TXFF. If it never
     * clears (a wedged UART), fall through and write DR anyway — a real PL011
     * would then drop the byte, so the expected marker would be missing and the
     * smoke harness FAILs. The point of the bound is only that a wedged device
     * can never block the boot forever (no success-via-timeout). */
    for (unsigned i = 0; i < 100000u; ++i) {
        if ((mmio_r32(PL011_FR) & PL011_FR_TXFF) == 0u) break;
    }
    mmio_w32(PL011_DR, (uint32_t)(unsigned char)c);
}

static void console_write(const char *s)
{
    /* Bounded length so a missing NUL cannot run off into unmapped MMIO. */
    for (unsigned i = 0; i < 4096u && s[i] != '\0'; ++i) console_putc(s[i]);
}

/* Write a 64-bit value as 0x-prefixed hex (fixed 16 digits). */
static void console_hex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; ++i) buf[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
    buf[18] = '\0';
    console_write(buf);
}

_Noreturn static void halt(void)
{
    for (;;) __asm__ volatile("wfe");
}

/* Read-back helpers to make the markers evidence, not assertion. */
static inline uint64_t read_currentel(void)
{
    uint64_t v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return (v >> 2) & 3u;
}
static inline uint64_t read_tcr_el1(void)
{
    uint64_t v; __asm__ volatile("mrs %0, TCR_EL1" : "=r"(v)); return v;
}
static inline uint64_t read_sp(void)
{
    uint64_t v; __asm__ volatile("mov %0, sp" : "=r"(v)); return v;
}

/* Optional patterned-BSS self-check: in the dedicated test build the boot.S
 * entry fills BSS with a nonzero pattern BEFORE the production clear runs, and
 * this probe (defined below in .bss) is then checked to confirm the clear zeroed
 * it — rather than relying on QEMU's already-zero RAM (which would make the
 * claim tautological). */

/* A probe in .bss so the section is non-empty in EVERY build (otherwise the
 * clear loop runs zero times and "bss cleared" proves nothing). In the
 * patterned-BSS build (boot.S pre-fills BSS) this same probe is checked to be
 * zero after the production clear. Linker symbols bound the cleared range, which
 * we print as evidence. */
volatile uint64_t je_arm_bss_probe;   /* in .bss */
extern uint8_t __bss_start[], __bss_end[];

_Noreturn void je_arm_main(void)
{
    uint64_t el = read_currentel();
    /* Single-digit EL so the marker reads "entry EL1" exactly (EL is 0..3). */
    char elc = (char)('0' + (int)(el & 3u));
    console_write("JeiOS/aarch64: entry EL");
    console_putc(elc);
    console_write("\r\n");

    console_write("JeiOS/aarch64: stack ready sp=");
    console_hex(read_sp());
    console_write("\r\n");

    console_write("JeiOS/aarch64: tcr_el1 initialized (tbi0=0) tcr=");
    console_hex(read_tcr_el1());
    console_write("\r\n");

    /* Evidence: print the cleared range so "bss cleared" is not vacuous even in
     * the default build (the probe above guarantees the range is non-empty). */
    console_write("JeiOS/aarch64: bss range [");
    console_hex((uint64_t)(uintptr_t)__bss_start);
    console_write(",");
    console_hex((uint64_t)(uintptr_t)__bss_end);
    console_write(")\r\n");
#ifdef JE_ARM_BSS_CHECK
    /* The clear has already run (boot.S) after the stub pre-filled BSS; prove it
     * actually zeroed the .bss probe that was pattern-filled. */
    if (je_arm_bss_probe != 0u) {
        console_write("JeiOS/aarch64: FATAL bss not cleared\r\n");
        halt();
    }
    console_write("JeiOS/aarch64: bss clear verified\r\n");
#else
    /* Default build: the probe is in .bss and must read zero after the clear. */
    if (je_arm_bss_probe != 0u) {
        console_write("JeiOS/aarch64: FATAL bss probe nonzero\r\n");
        halt();
    }
    console_write("JeiOS/aarch64: bss cleared\r\n");
#endif

    console_write("JeiOS/aarch64: dtb pointer recorded dtb=");
    console_hex(je_arm_dtb_phys);
    console_write("\r\n");

    console_write("JeiOS/aarch64: initialization complete; halted\r\n");
    halt();
}
