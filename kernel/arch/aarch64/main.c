/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP1 step 1 (ADR 0002): the C entry for the ARM64 (QEMU virt-8.2) bring-up.
 * Reached from boot.S after EL1 is confirmed, the stack is set, TCR_EL1 is
 * initialized (TBI0=0 interim) and BSS is cleared. This file drives a bounded
 * PL011 polling console and emits the ordered step-1 markers, then halts.
 *
 * Scope boundaries (ADR 0002 / MP1 decomposition, locked by the design pass):
 *   - VBAR_EL1 exception vectors + the D5-3 trap frame ARE now installed (this
 *     PR): a deliberate BRK is proven to vector with the exact syndrome;
 *   - bounded DTB intake (issue #52, ADR 0002 D3 copy model): after vectors
 *     are installed, read the firmware header with byte loads, copy the planned
 *     prefix ONCE into an in-image .bss buffer, parse ONLY the copy with the
 *     shared parser (kernel/core/fdt.c, contract include/jeios/fdt.h), then
 *     validate the result against arch facts. Any rejection is a bounded FATAL
 *     marker + halt. Nothing consumes the parsed context yet (MP2 allocator);
 *   - NO MMU, no atomics; single CPU (MMU/timer/scheduling is MP2).
 * The handoff records that the DTB validation and the MMU/fixup paths are not
 * yet enforced.
 *
 * Built -mstrict-align -mgeneral-regs-only -ffreestanding -fno-builtin, so the
 * compiler emits neither unaligned accesses (Device memory pre-MMU faults them)
 * nor FP/SIMD (which would trap into absent vectors and hang silently).
 */
#include <stdbool.h>
#include <stdint.h>
#include <jeios/fdt.h>
#if defined(JE_ARM_USER_TEST) || defined(JE_ARM_EL0_TEST)
#include <jeios/vm.h>
#endif
#ifdef JE_ARM_EL0_TEST
#include <jeios/usercopy.h>
#endif
#include "gic.h"
#include "paging.h"
#include "physmem.h"
#include "trapframe.h"

#ifdef JE_ARM_SCHED_TEST
bool je_arm_sched_guard_fault_check(const je_arm_trapframe *tf, uint64_t ec);
#endif
#ifdef JE_ARM_EL0_TEST
extern _Noreturn void je_arm_el0_test_run(void);
#endif

/* MP3b reentrancy guard (vectors.S): 0 outside a handler, 1 inside an SVC or
 * ordinary returning handler, 2 inside the copy-fault C check. Any state other
 * than the one exact copy-site transition is stack-free double-fault fatal.
 * Plain byte, single CPU, no atomics. In .data so it is defined before BSS
 * clearing. */
volatile uint8_t je_arm_in_handler __attribute__((section(".data"))) = 0u;

#ifdef JE_ARM_EL0_TEST
/* Slot-4 saves these before using x16-x18 as its guard/vector scratch. */
uint64_t je_arm_copy_saved_gprs[3] __attribute__((section(".data"))) = {0u, 0u, 0u};
#ifdef JE_ARM_COPY_FAULT_MUTANT_RESTORE_SELECTED
volatile uint8_t je_arm_copy_restore_selected_seen
    __attribute__((section(".data"))) = 0u;
#endif
#endif

/* DTB physical address, stashed by boot.S into .data BEFORE the BSS clear so it
 * survives (a .bss slot would have been zeroed). boot.S has already checked it
 * is nonzero, 8-aligned and that [ptr, ptr + JE_FDT_MAX_TOTAL) is disjoint from
 * the image; dtb_intake() below reads/copies through it. */
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

/* Write a 64-bit value in unsigned decimal, no padding. Bounded: UINT64_MAX has
 * 20 digits, so the loop runs at most 20 times and never underflows buf. */
static void console_dec(uint64_t v)
{
    char buf[21];
    unsigned i = 20u;
    buf[20] = '\0';
    do {
        buf[--i] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    console_write(&buf[i]);
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
static inline uint64_t read_vbar_el1(void)
{
    uint64_t v; __asm__ volatile("mrs %0, VBAR_EL1" : "=r"(v)); return v;
}

/* ---- MP1 (ADR 0002 D5-3): exception vectors. je_arm_vectors is the 2 KiB-aligned
 * 16-slot table in vectors.S; install it into VBAR_EL1 and ISB so the write takes
 * before any subsequent exception. No I-cache maintenance: the table is static
 * image content nothing modifies. ---- */
extern uint8_t je_arm_vectors[];
static void install_vectors(void)
{
    __asm__ volatile("msr VBAR_EL1, %0; isb" :: "r"((uint64_t)(uintptr_t)je_arm_vectors) : "memory");
}

/* AArch64 ESR_EL1 field extracts (DDI0487): EC[31:26], IL[25], ISS[24:0]. */
#define ESR_EC(esr)  (((esr) >> 26) & 0x3fu)
#define ESR_IL(esr)  (((esr) >> 25) & 0x1u)
#define ESR_ISS(esr) ((esr) & 0x1ffffffu)
/* EC values this diagnostic names explicitly. */
#define EC_UNKNOWN   0x00u   /* undefined/unknown instruction */
#define EC_SVC64     0x15u   /* SVC from AArch64 (syscall; not a fault) */
#define EC_IABT_LOW  0x20u   /* instruction abort, lower EL */
#define EC_IABT_CUR  0x21u   /* instruction abort, current EL */
#define EC_PC_ALIGN  0x22u   /* PC alignment fault */
#define EC_DABT_LOW  0x24u   /* data abort, lower EL */
#define EC_DABT_CUR  0x25u   /* data abort, current EL */
#define EC_SP_ALIGN  0x26u   /* SP alignment fault */
#define EC_BRK64     0x3cu   /* BRK instruction (AArch64) */
/* Whether FAR_EL1 holds a meaningful VA for this exception. Aborts (I/D, cur or
 * lower EL) and PC-alignment define FAR, BUT for aborts FAR is UNKNOWN when
 * ISS.FnV (bit 10) is set (e.g. a synchronous external abort). Check both. */
static int ec_far_valid(uint64_t ec, uint64_t esr)
{
    uint64_t fnv = (esr >> 10) & 0x1u;   /* ISS[10] FnV (abort classes) */
    switch (ec) {
    case EC_IABT_LOW: case EC_IABT_CUR:
    case EC_DABT_LOW: case EC_DABT_CUR:
        return fnv == 0u;                /* FAR valid only when FnV==0 */
    case EC_PC_ALIGN:
        return 1;                        /* PC alignment: FAR always valid */
    default:
        return 0;
    }
}

#ifdef JE_ARM_MMU_FAULT
/* ---- MP2 PR-1b deliberate MMU fault proofs (ADR 0011 D4/D7). ----
 *
 * One build per mode (mk/aarch64.mk ARM_MMU_FAULT=<name>, mutually exclusive
 * with the other test toggles, own build dir). After the default MMU markers,
 * mmu_fault_trigger() performs exactly ONE access the W^X / guard contract must
 * refuse; je_arm_exception() then calls mmu_fault_check(), which asserts the
 * EXACT syndrome (EC, IL, FSC = ISS[5:0], WnR, and S1PTW/CM/EA/FnV == 0) plus
 * FAR, ELR and SPSR, and prints a distinctive PASS line (or a FATAL). If the
 * access does NOT fault, the trigger prints FATAL and halts: a broken property
 * can never reach the PASS line. Fault class is pinned so an UNMAPPED target
 * (translation fault, FSC 0b000111) cannot masquerade as the permission proof
 * (FSC 0b001111, level 3 — DDI0487 D17.2 ESR_ELx DFSC/IFSC encodings).
 *
 *   1 write-to-text       STR to a .text word       -> DABT_CUR perm L3, WnR=1
 *   2 execute-from-data   BLR to a .data `ret`      -> IABT_CUR perm L3
 *   3 execute-from-rodata BLR to a .rodata `ret`    -> IABT_CUR perm L3
 *   4 stack-guard         STR to __stack_guard      -> DABT_CUR xlate L3, WnR=1
 *
 * The rodata case matters separately from the data case: SCTLR.WXN only forces
 * WRITABLE pages XN, so a rodata leaf with PXN=0 would be executable while the
 * data test still passed. Targets/sites are GLOBAL symbols so smoke_arm.py
 * resolves them from the ELF and checks FAR/ELR against them independently.
 * TCG observes translation + permission faults; nothing here is cache evidence. */
#define JE_ARM_MMU_FAULT_TEXT   1
#define JE_ARM_MMU_FAULT_DATA   2
#define JE_ARM_MMU_FAULT_RODATA 3
#define JE_ARM_MMU_FAULT_GUARD  4
#if JE_ARM_MMU_FAULT < JE_ARM_MMU_FAULT_TEXT || JE_ARM_MMU_FAULT > JE_ARM_MMU_FAULT_GUARD
#error "JE_ARM_MMU_FAULT must be 1..4 (see mk/aarch64.mk ARM_MMU_FAULT)"
#endif
#define A64_RET 0xd65f03c0u   /* RET (x30), DDI0487 C6.2 */

/* .text target: a dedicated never-called word, so a (broken) successful write
 * corrupts nothing that runs. */
__asm__(".pushsection .text\n"
        ".balign 16\n"
        ".globl je_arm_mmu_fault_text_target\n"
        ".type je_arm_mmu_fault_text_target, %function\n"
        "je_arm_mmu_fault_text_target:\n"
        "    ret\n"
        ".size je_arm_mmu_fault_text_target, .-je_arm_mmu_fault_text_target\n"
        ".popsection");
extern uint8_t je_arm_mmu_fault_text_target[];
/* Non-const, initialized => .data (RW, must be XN). */
uint32_t je_arm_mmu_fault_data_insn[4] __attribute__((aligned(16))) =
    { A64_RET, A64_RET, A64_RET, A64_RET };
/* const => .rodata (R, must be XN; NOT covered by WXN). */
const uint32_t je_arm_mmu_fault_rodata_insn[4] __attribute__((aligned(16))) =
    { A64_RET, A64_RET, A64_RET, A64_RET };
extern uint8_t je_arm_mmu_fault_store_site[];
extern uint8_t __stack_guard[];

static uint64_t mmu_fault_target(void)
{
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_TEXT
    return (uint64_t)(uintptr_t)je_arm_mmu_fault_text_target;
#elif JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_DATA
    return (uint64_t)(uintptr_t)je_arm_mmu_fault_data_insn;
#elif JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_RODATA
    return (uint64_t)(uintptr_t)je_arm_mmu_fault_rodata_insn;
#else
    return (uint64_t)(uintptr_t)__stack_guard;
#endif
}

static const char *mmu_fault_name(void)
{
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_TEXT
    return "write-to-text";
#elif JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_DATA
    return "execute-from-data";
#elif JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_RODATA
    return "execute-from-rodata";
#else
    return "stack-guard";
#endif
}

static void mmu_fault_check(const je_arm_trapframe *tf, uint64_t ec)
{
    const uint64_t iss = ESR_ISS(tf->esr);
    const uint64_t fsc = iss & 0x3fu;          /* DFSC/IFSC */
    const uint64_t wnr = (iss >> 6) & 1u;      /* DABT WnR; RES0 for IABT */
    const uint64_t s1ptw = (iss >> 7) & 1u;    /* fault on a table walk */
    const uint64_t cm = (iss >> 8) & 1u;       /* cache maintenance; RES0 IABT */
    const uint64_t ea = (iss >> 9) & 1u;       /* external abort */
    const uint64_t fnv = (iss >> 10) & 1u;     /* FAR not valid */
    const uint64_t target = mmu_fault_target();
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_TEXT || JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_GUARD
    const uint64_t want_ec = EC_DABT_CUR, want_wnr = 1u;
    const uint64_t want_elr = (uint64_t)(uintptr_t)je_arm_mmu_fault_store_site;
#else
    const uint64_t want_ec = EC_IABT_CUR, want_wnr = 0u;
    const uint64_t want_elr = target;          /* the fetch itself faulted */
#endif
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_GUARD
    const uint64_t want_fsc = 0x07u;           /* translation fault, level 3 */
#else
    const uint64_t want_fsc = 0x0fu;           /* permission fault, level 3 */
#endif
    const bool ok = tf->vector == 4u && ec == want_ec &&
                    ESR_IL(tf->esr) == 1u && fsc == want_fsc &&
                    wnr == want_wnr && s1ptw == 0u && cm == 0u && ea == 0u &&
                    fnv == 0u && tf->far == target && tf->elr == want_elr &&
                    (tf->spsr & 0x1fu) == 0x5u /* EL1h */ &&
                    ((tf->spsr >> 6) & 0xfu) == 0xfu /* DAIF masked */;
    if (!ok) {
        console_write("JeiOS/aarch64: FATAL mmu fault self-test MISMATCH ");
        console_write(mmu_fault_name());
        console_write("\r\n");
        return;
    }
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_TEXT
    console_write("JeiOS/aarch64: mmu fault self-test PASS write-to-text (DABT_CUR ec=0x25 dfsc=0x0f permission L3 WnR=1; FAR==.text target; ELR==store site)\r\n");
#elif JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_DATA
    console_write("JeiOS/aarch64: mmu fault self-test PASS execute-from-data (IABT_CUR ec=0x21 ifsc=0x0f permission L3; FAR==ELR==.data buffer proven RW)\r\n");
#elif JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_RODATA
    console_write("JeiOS/aarch64: mmu fault self-test PASS execute-from-rodata (IABT_CUR ec=0x21 ifsc=0x0f permission L3; FAR==ELR==.rodata target)\r\n");
#else
    console_write("JeiOS/aarch64: mmu fault self-test PASS stack-guard (DABT_CUR ec=0x25 dfsc=0x07 translation L3 WnR=1; FAR==__stack_guard; ELR==store site)\r\n");
#endif
}
#endif /* JE_ARM_MMU_FAULT */

/* MP1 common C exception handler: bounded redacted diagnostic + halt. No
 * recovery, no allocation (ARM64.md). "Redacted" = we print the syndrome fields
 * (vector, EC, IL, ISS, ELR, SPSR, FAR-if-valid) and a VBAR readback, NOT the
 * full GPR dump — a fault diagnostic names WHERE and WHAT, not register
 * contents. Marker strings are fixed so the smoke harness can assert them. */
_Noreturn void je_arm_exception(je_arm_trapframe *tf)
{
    uint64_t ec = ESR_EC(tf->esr);
    console_write("JeiOS/aarch64: EXCEPTION vector=");
    console_hex(tf->vector);
    console_write(" ec=");
    console_hex(ec);
    console_write(" il=");
    console_hex(ESR_IL(tf->esr));
    console_write(" iss=");
    console_hex(ESR_ISS(tf->esr));
    console_write("\r\n");
    console_write("JeiOS/aarch64:   esr=");
    console_hex(tf->esr);
    console_write(" elr=");
    console_hex(tf->elr);
    console_write(" spsr=");
    console_hex(tf->spsr);
    console_write("\r\n");
    console_write("JeiOS/aarch64:   far=");
    if (ec_far_valid(ec, tf->esr)) console_hex(tf->far);
    else                  console_write("n/a");
    console_write(" vbar=");
    console_hex(read_vbar_el1());        /* evidence the table was installed */
    console_write("\r\n");
#ifdef JE_ARM_VECTOR_TEST
    /* Deliberate-exception self-test: the ONLY exception we ever expect is the
     * planted BRK #0xA55A. Assert the exact syndrome end-to-end:
     *   EC == 0x3c (BRK64), IL == 1, ISS[15:0] == 0xA55A  => ESR == 0xF200A55A,
     *   vector == 4 (Current EL SP_ELx, Synchronous), ELR == &je_arm_brk_site.
     * A distinctive PASS marker precedes the halt so the harness asserts success
     * (not timeout). SPSR is config-specific (M[4:0]=0b0101 EL1h + DAIF masked =
     * 0x3c5 here) so we check M[4:0] and DAIF separately, not the whole word. */
    extern uint8_t je_arm_brk_site[];
    uint64_t spsr_m = tf->spsr & 0x1fu;          /* mode field */
    uint64_t spsr_daif = (tf->spsr >> 6) & 0xfu; /* D,A,I,F */
    if (tf->vector == 4u && tf->esr == 0xF200A55Aull &&
        tf->elr == (uint64_t)(uintptr_t)je_arm_brk_site &&
        spsr_m == 0x5u /* EL1h */ && spsr_daif == 0xfu /* all masked */) {
        console_write("JeiOS/aarch64: vector self-test PASS (planted BRK #0xa55a vectored to EL1h sync slot 4 with exact ESR 0xf200a55a; ELR==brk site; SPSR EL1h+DAIF-masked)\r\n");
    } else {
        console_write("JeiOS/aarch64: FATAL vector self-test MISMATCH\r\n");
    }
#endif
#ifdef JE_ARM_TIMER_TEST
    /* Negative control: after the returning IRQ proof, this is a synchronous
     * BRK in slot 4. It must retain the MP1 fatal diagnostic and must not take
     * the transient-guard double-fault path. */
    extern uint8_t je_arm_irq_brk_site[];
    if (tf->vector != 4u || tf->esr != UINT64_C(0xF200B2B2) ||
        tf->elr != (uint64_t)(uintptr_t)je_arm_irq_brk_site ||
        (tf->spsr & 0x1fu) != 0x5u ||
        ((tf->spsr >> 6) & 0xfu) != 0xfu) {
        console_write("JeiOS/aarch64: FATAL IRQ-path BRK control mismatch\r\n");
    }
#endif
#ifdef JE_ARM_MMU_FAULT
    mmu_fault_check(tf, ec);
#endif
#ifdef JE_ARM_SCHED_TEST
    if (je_arm_sched_guard_fault_check(tf, ec)) {
        console_write("JeiOS/aarch64: sched guard PASS (thread guard translation fault; FAR==guard; ELR==store site)\r\n");
    } else {
        console_write("JeiOS/aarch64: FATAL sched guard mismatch\r\n");
    }
#endif
    console_write("JeiOS/aarch64: exception handled; halted\r\n");
    for (;;) __asm__ volatile("wfe");
}

#ifdef JE_ARM_EL0_TEST
/*
 * The only C entry reachable from the returning slot-4 copy branch.  Rejects
 * stay on the existing fatal diagnostic; acceptance returns the frame pointer
 * so the dedicated assembly tail can assert that the C seam did not redirect
 * the restore target.
 */
je_arm_trapframe *je_arm_copy_fixup_dispatch(je_arm_trapframe *tf)
{
    if (!je_arm_copy_fault(tf)) {
        console_write("JeiOS/aarch64: FATAL copy fault fixup rejected\r\n");
        je_arm_exception(tf);
    }
    return tf;
}
#endif

/* je_arm_double_fault lives in vectors.S (pure asm, guaranteed stack-free — a C
 * function could grow a stack-touching prologue under -O0/flag changes and then
 * fault again on an already-corrupt/misaligned SP, looping with no marker). */

#ifdef JE_ARM_VECTOR_TEST
/* The planted deliberate exception. je_arm_brk_site is the exact faulting PC the
 * handler asserts ELR_EL1 equals. BRK is a synchronous, DAIF-unmaskable software
 * breakpoint (DDI0487): it vectors to the EL1h sync slot even though we never
 * unmasked interrupts. imm16 = 0xA55A lands in ESR ISS[15:0]. */
__attribute__((noinline, noclone))
static void trigger_brk(void)
{
    /* .globl so the label has external binding + a single definition even if the
     * toolchain were to relocate the asm; noinline/noclone prevent duplication
     * (a duplicated label is a hard assembler error, not a silent bug). */
    __asm__ volatile(".globl je_arm_brk_site\nje_arm_brk_site: brk #0xA55A");
}
#endif

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

/* ---- Bounded DTB intake (issue #52; contract include/jeios/fdt.h). ----
 *
 * Ownership/lifetime: je_arm_dtb_copy is the kernel's own copy of the firmware
 * DTB prefix. It lives in .bss (so inside [_text, _end), the kernel_image fact;
 * linker.ld asserts placement + 16-byte alignment), is written exactly once by
 * dtb_intake() and is never modified afterwards except by the JE_ARM_DTB_BAD
 * test corruption. je_arm_boot_ctx is the parsed result (fixed size, no
 * pointers into the blob). Single CPU, no concurrency, caller context = boot.
 *
 * CACHE NOTE: the copy is written with the MMU and D-cache OFF (non-cacheable).
 * MP2 MUST `DC IVAC` the whole buffer (and je_arm_boot_ctx) before enabling the
 * D-cache, or later cacheable reads may hit stale lines (mp1-dtb debate item 32).
 */
_Static_assert(JE_FDT_MAX_TOTAL == 0x200000u,
               "boot.S JE_ARM_DTB_GUARD_SPAN must equal JE_FDT_MAX_TOTAL");
_Static_assert(JE_FDT_MAX_COPY == 0x10000u,
               "linker.ld copy-buffer ASSERT hard-codes 64 KiB");
uint8_t je_arm_dtb_copy[JE_FDT_MAX_COPY] __attribute__((aligned(16)));
static je_boot_ctx je_arm_boot_ctx;
static je_boot_facts je_arm_facts;
static je_pages je_arm_pages;
extern je_arm_mmu je_arm_mmu_context;
extern uint64_t je_arm_mmu_last_audit_leaves;
extern uint8_t _text[], _end[];   /* IMAGE extent [_text, _end), linker.ld */
extern uint8_t __text_end[], __rodata_end[], __data_end[];
extern uint8_t __stack_guard[], __stack_bottom[];
#if defined(JE_ARM_USER_TEST) || defined(JE_ARM_EL0_TEST)
extern void je_arm_sched_install_ttbr0(uint64_t root_pa, uint16_t asid);
extern uint64_t je_arm_sched_read_ttbr0(void);
extern je_status je_arch_vm_decorate_kernel_root(void);
#endif
#ifdef JE_ARM_USER_TEST
static je_vm je_arm_user_vm;
static uint8_t je_arm_user_blob[3u * JE_PAGE_SIZE]
    __attribute__((aligned(16)));
static const je_vm_image je_arm_user_image = {
    .version = JE_VM_IMAGE_VERSION,
    .segment_count = 3u,
    .entry_offset = 0u,
    .reserved = 0u,
    .segments = {
        { 0u, JE_PAGE_SIZE, 0u, JE_PAGE_SIZE, JE_VM_R | JE_VM_X, 0u },
        { JE_PAGE_SIZE, JE_PAGE_SIZE, JE_PAGE_SIZE, JE_PAGE_SIZE,
          JE_VM_R, 0u },
        { 2u * JE_PAGE_SIZE, JE_PAGE_SIZE, 2u * JE_PAGE_SIZE, JE_PAGE_SIZE,
          JE_VM_R | JE_VM_W, 0u },
    },
};
#endif

je_pages *je_arm_scheduler_pages(void)
{
    return &je_arm_pages;
}

void je_arm_sched_init(void);
void *je_arm_sched_irq(je_arm_trapframe *tf);
void *je_arm_sched_no_tick(je_arm_trapframe *tf);
#ifdef JE_ARM_SCHED_TEST
_Noreturn void je_arm_sched_test(void);
#endif

static inline uint64_t read_id_aa64mmfr0_el1(void)
{
    uint64_t v; __asm__ volatile("mrs %0, ID_AA64MMFR0_EL1" : "=r"(v)); return v;
}

/* ID_AA64MMFR0_EL1.PARange [3:0] (DDI0487 D19): 0..6 -> 32,36,40,42,44,48,52
 * bits. Every other encoding is reserved; return 0 = "no valid width". */
static uint32_t pa_bits_from_mmfr0(uint64_t mmfr0)
{
    switch (mmfr0 & 0xfu) {
    case 0u: return 32u;
    case 1u: return 36u;
    case 2u: return 40u;
    case 3u: return 42u;
    case 4u: return 44u;
    case 5u: return 48u;
    case 6u: return 52u;
    default: return 0u;
    }
}

/* Big-endian u32 from a buffer the caller has bounds-checked (byte loads only,
 * so alignment never matters under -mstrict-align). */
static uint32_t be32_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* je_fdt_status_name() comes from another translation unit; never hand a NULL
 * to console_write (it would read address 0). */
static const char *status_name(je_fdt_status st)
{
    const char *n = je_fdt_status_name(st);
    return n != NULL ? n : "UNKNOWN";
}

_Noreturn static void dtb_reject(je_fdt_status st, uint64_t off, bool window)
{
    console_write("JeiOS/aarch64: FATAL dtb rejected status=");
    console_write(status_name(st));
    console_write(" off=");
    console_hex(off);
    if (window) console_write(" (live data exceeds 64 KiB copy window)");
    console_write("\r\n");
    halt();
}

_Noreturn static void ctx_reject(je_fdt_status st, uint64_t idx)
{
    console_write("JeiOS/aarch64: FATAL boot context rejected status=");
    console_write(status_name(st));
    console_write(" idx=");
    console_hex(idx);
    console_write("\r\n");
    halt();
}

/* Steps 1-2 of the fdt.h model: header read + plan + single copy. Returns
 * copy_len (in [JE_FDT_HEADER_SIZE, JE_FDT_MAX_COPY]) and sets *totalsize; halts
 * on any rejection. Reads at most JE_FDT_HEADER_SIZE + copy_len firmware bytes,
 * all with volatile BYTE loads (pre-MMU RAM is Device memory; -mstrict-align;
 * volatile also stops the compiler turning the loop into a memcpy call). A
 * fault on the firmware range (e.g. x0 beyond RAM) vectors to the installed
 * diagnostic handler, which is why this runs only after install_vectors(). */
static uint32_t dtb_copy_in(uint32_t *totalsize)
{
    const volatile uint8_t *src =
        (const volatile uint8_t *)(uintptr_t)je_arm_dtb_phys;
    uint8_t hdr[JE_FDT_HEADER_SIZE];
    for (uint32_t i = 0u; i < JE_FDT_HEADER_SIZE; ++i) hdr[i] = src[i];

    uint32_t copy_len = 0u;
    *totalsize = 0u;
    je_fdt_status st = je_fdt_copy_plan(hdr, &copy_len, totalsize);
    if (st != JE_FDT_OK) dtb_reject(st, 0u, false);
    /* Do not trust the planner with our buffer bound: fail closed if it ever
     * returns a length the buffer cannot hold or that cannot contain a header. */
    if (copy_len < JE_FDT_HEADER_SIZE || copy_len > JE_FDT_MAX_COPY) {
        console_write("JeiOS/aarch64: FATAL dtb copy plan out of bounds len=");
        console_hex(copy_len);
        console_write("\r\n");
        halt();
    }
    /* The ONE copy. Everything after this reads only je_arm_dtb_copy. */
    for (uint32_t i = 0u; i < copy_len; ++i) je_arm_dtb_copy[i] = src[i];
    return copy_len;
}

/* Diagnostic CRC extent: the header-claimed strings-block end READ FROM THE
 * COPY, clamped to copy_len. Under the contract's canonical block order
 * (rsvmap < struct < strings) this is exactly the live extent `used` for any
 * blob the parser accepts, which is what the committed fixture contains (the
 * fixture is the live extent, not the 1 MiB zero-padded totalsize). Computed
 * before the parse, from untrusted header fields, but only ever used as a
 * length bounded by copy_len — never as an offset into anything else. */
static uint32_t dtb_crc_len(uint32_t copy_len)
{
    uint64_t end = (uint64_t)be32_at(&je_arm_dtb_copy[12]) +   /* off_dt_strings */
                   (uint64_t)be32_at(&je_arm_dtb_copy[32]);    /* size_dt_strings */
    return end < copy_len ? (uint32_t)end : copy_len;
}

#ifdef JE_ARM_DTB_BAD
/* Test-only parser fault injection (mp1-dtb debate item 30): overwrite the
 * first struct token IN THE COPY with the unknown token 0x0000000A, after the
 * copy and before the parse, so the PARSER (not the glue) must reject with
 * BAD_TOKEN at off_dt_struct. The offset is read from the copy and
 * bounds-checked so the injection itself can never write outside the buffer. */
static void dtb_bad_inject(uint32_t copy_len)
{
    uint32_t off = be32_at(&je_arm_dtb_copy[8]);   /* off_dt_struct */
    if (off > copy_len || copy_len - off < 4u) {
        console_write("JeiOS/aarch64: FATAL dtb-bad test: off_dt_struct outside copy\r\n");
        halt();
    }
    je_arm_dtb_copy[off + 0u] = 0x00u;
    je_arm_dtb_copy[off + 1u] = 0x00u;
    je_arm_dtb_copy[off + 2u] = 0x00u;
    je_arm_dtb_copy[off + 3u] = 0x0au;
    console_write("JeiOS/aarch64: dtb-bad test: wrote token 0x0000000a at off=");
    console_hex(off);
    console_write("\r\n");
}
#endif

static void dtb_print_ctx(const je_boot_ctx *c)
{
    console_write("JeiOS/aarch64: dtb parsed ram=");
    console_dec(c->ram_count);
    console_write(" rsv=");
    console_dec(c->rsv_count);
    console_write(" ignored=");
    console_dec(c->rsv_ignored);
    console_write(" dynamic=");
    console_dec(c->rsv_dynamic);
    console_write(" initrd=");
    console_putc(c->has_initrd ? '1' : '0');
    console_write("\r\n");
    /* Bound by the array capacity even if a parser bug reported more. */
    for (uint32_t i = 0u; i < c->ram_count && i < JE_FDT_MAX_RAM; ++i) {
        console_write("JeiOS/aarch64:   ram[");
        console_dec(i);
        console_write("] base=");
        console_hex(c->ram[i].base);
        console_write(" size=");
        console_hex(c->ram[i].size);
        console_write("\r\n");
    }
}

/* Steps 1-4 of the fdt.h model. Returns only when the boot context has been
 * parsed from the copy AND validated against the arch facts; every failure
 * prints exactly one FATAL marker and halts. */
static void dtb_intake(void)
{
    uint32_t totalsize = 0u;
    uint32_t copy_len = dtb_copy_in(&totalsize);
    uint32_t crc_len = dtb_crc_len(copy_len);

    console_write("JeiOS/aarch64: dtb copied len=");
    console_hex(copy_len);
    console_write(" totalsize=");
    console_hex(totalsize);
    console_write(" crc32=");
    console_hex(je_fdt_crc32(je_arm_dtb_copy, crc_len));
    console_write(" crc_len=");
    console_hex(crc_len);
    console_write("\r\n");

#ifdef JE_ARM_DTB_BAD
    dtb_bad_inject(copy_len);
#endif

    uint32_t err_off = 0u;
    je_fdt_status st = je_fdt_parse(je_arm_dtb_copy, copy_len,
                                    &je_arm_boot_ctx, &err_off);
    if (st != JE_FDT_OK) {
        bool window = st == JE_FDT_TRUNCATED && copy_len == JE_FDT_MAX_COPY &&
                      totalsize > copy_len;
        dtb_reject(st, err_off, window);
    }
    dtb_print_ctx(&je_arm_boot_ctx);

    uint32_t pa_bits = pa_bits_from_mmfr0(read_id_aa64mmfr0_el1());
    if (pa_bits == 0u) {
        /* Reserved PARange encoding: the "facts" are invalid. Refuse here rather
         * than rely on the checker rejecting pa_bits == 0. */
        ctx_reject(JE_FDT_BAD_RANGE, JE_BOOT_ERR_FACTS);
    }
    /* dtb_src: conservative FULL firmware range [x0, x0 + totalsize). Use the
     * larger of the firmware-header totalsize (which sized the copy) and the
     * copy's re-derived one so a header changing under us can only widen it. */
    uint32_t src_size = totalsize > je_arm_boot_ctx.totalsize
                            ? totalsize : je_arm_boot_ctx.totalsize;
    je_boot_facts facts = {
        .kernel_image = { (uint64_t)(uintptr_t)_text,
                          (uint64_t)((uintptr_t)_end - (uintptr_t)_text) },
        .dtb_src      = { je_arm_dtb_phys, src_size },
        .pa_bits      = pa_bits,
    };
    uint32_t err_idx = 0u;
    st = je_boot_ctx_check(&je_arm_boot_ctx, &facts, &err_idx);
    if (st != JE_FDT_OK) ctx_reject(st, err_idx);

    console_write("JeiOS/aarch64: boot context validated pa_bits=");
    console_dec(pa_bits);
    console_write("\r\n");
    je_arm_facts.kernel_image = facts.kernel_image;
    je_arm_facts.dtb_src = facts.dtb_src;
    je_arm_facts.pa_bits = facts.pa_bits;
}

/*
 * Marker sequence emitted by every successful physmem bring-up:
 *   JeiOS/aarch64: physmem window base=<hex> limit=<hex>
 *   JeiOS/aarch64: physmem reserve kind=kernel base=<hex> size=<hex>
 *   JeiOS/aarch64: physmem reserve kind=dtb base=<hex> size=<hex>
 *   JeiOS/aarch64: physmem reserve kind=initrd base=<hex> size=<hex>
 *   JeiOS/aarch64: physmem ram base=<hex> size=<hex>
 *   JeiOS/aarch64: physmem reserve kind=rsv base=<hex> size=<hex> nomap=<0|1>
 *   JeiOS/aarch64: physmem unmanaged ram bytes=<hex>
 *   JeiOS/aarch64: physmem free pages=<hex> bytes=<hex>
 *   JeiOS/aarch64: physmem alloc1 pa=<hex>
 *   JeiOS/aarch64: physmem free1 ok
 *   JeiOS/aarch64: physmem allocmax pa=<hex>
 *   JeiOS/aarch64: physmem overcap rejected pages=<hex>
 *
 * The initrd line is conditional, and RAM/reservation lines repeat in input
 * order. The exact marker order is window, kernel, DTB, optional initrd, RAM,
 * kept reservations, free summary, alloc/free proof, max-run alloc, over-cap reject.
 */
static void physmem_print_range(const char *kind, je_range range)
{
    console_write("JeiOS/aarch64: physmem reserve kind=");
    console_write(kind);
    console_write(" base=");
    console_hex(range.base);
    console_write(" size=");
    console_hex(range.size);
    console_write("\r\n");
}

_Noreturn static void physmem_fatal(const char *operation, je_status status)
{
    console_write("JeiOS/aarch64: FATAL physmem ");
    console_write(operation);
    console_write(" status=");
    console_dec((uint64_t)status);
    console_write("\r\n");
    halt();
}

static void physmem_bringup(void)
{
    je_arm_physmem_view view = {
        .ctx = &je_arm_boot_ctx,
        .kernel = je_arm_facts.kernel_image,
        .dtb = je_arm_facts.dtb_src,
    };
    je_status status = je_arm_physmem_init(&je_arm_pages, &view);
    if (status != JE_OK) physmem_fatal("init", status);

    console_write("JeiOS/aarch64: physmem window base=");
    console_hex(JE_PHYSICAL_BASE);
    console_write(" limit=");
    console_hex(JE_PHYSICAL_LIMIT);
    console_write("\r\n");
    physmem_print_range("kernel", view.kernel);
    physmem_print_range("dtb", view.dtb);
    if (view.ctx->has_initrd) physmem_print_range("initrd", view.ctx->initrd);
    for (uint32_t i = 0u; i < view.ctx->ram_count && i < JE_FDT_MAX_RAM; ++i) {
        console_write("JeiOS/aarch64: physmem ram base=");
        console_hex(view.ctx->ram[i].base);
        console_write(" size=");
        console_hex(view.ctx->ram[i].size);
        console_write("\r\n");
    }
    for (uint32_t i = 0u; i < view.ctx->rsv_count && i < JE_FDT_MAX_RSV; ++i) {
        console_write("JeiOS/aarch64: physmem reserve kind=rsv base=");
        console_hex(view.ctx->rsv[i].base);
        console_write(" size=");
        console_hex(view.ctx->rsv[i].size);
        console_write(" nomap=");
        console_putc(view.ctx->rsv_nomap[i] != 0u ? '1' : '0');
        console_write("\r\n");
    }

    /*
     * Degraded-profile honesty (AGENTS.md rule 5): the allocator window is fixed
     * at [JE_PHYSICAL_BASE, JE_PHYSICAL_LIMIT). Any DTB RAM lying OUTSIDE that
     * window (e.g. booting with -m > 256M, or RAM below the base) is not managed
     * by this PR. Report those bytes EXPLICITLY rather than silently dropping
     * them, so an operator sees the kernel is using less RAM than the board has.
     * Widening the window to 64-bit dynamic sizing is a recorded ADR 0011 flex.
     */
    uint64_t unmanaged = 0u;
    for (uint32_t i = 0u; i < view.ctx->ram_count && i < JE_FDT_MAX_RAM; ++i) {
        uint64_t b = view.ctx->ram[i].base;
        uint64_t e = b + view.ctx->ram[i].size;      /* parser guarantees no overflow */
        if (b < JE_PHYSICAL_BASE)  unmanaged += (e < JE_PHYSICAL_BASE  ? e : JE_PHYSICAL_BASE)  - b;
        if (e > JE_PHYSICAL_LIMIT) unmanaged += e - (b > JE_PHYSICAL_LIMIT ? b : JE_PHYSICAL_LIMIT);
    }
    console_write("JeiOS/aarch64: physmem unmanaged ram bytes=");
    console_hex(unmanaged);
    console_write("\r\n");

    size_t free_count = je_arm_pages.free_count;
    console_write("JeiOS/aarch64: physmem free pages=");
    console_hex((uint64_t)free_count);
    console_write(" bytes=");
    console_hex((uint64_t)free_count * JE_PAGE_SIZE);
    console_write("\r\n");

    uint64_t physical = 0u;
    status = je_pages_alloc(&je_arm_pages, 1u, &physical);
    if (status != JE_OK) physmem_fatal("alloc1", status);
    console_write("JeiOS/aarch64: physmem alloc1 pa=");
    console_hex(physical);
    console_write("\r\n");
    status = je_pages_free(&je_arm_pages, physical, 1u);
    if (status != JE_OK) physmem_fatal("free1", status);
    console_write("JeiOS/aarch64: physmem free1 ok\r\n");

    free_count = je_arm_pages.free_count;

    /*
     * Real multi-page allocation over the scan path: request the maximum legal
     * run (JE_ALLOC_MAX_PAGES), which actually walks state[] and commits a
     * contiguous extent, then free it. This exercises the base-relative
     * alloc/free arithmetic on a run > 1 (not just the single page above) and
     * must restore free_count exactly.
     */
    uint64_t run = 0u;
    status = je_pages_alloc(&je_arm_pages, JE_ALLOC_MAX_PAGES, &run);
    if (status != JE_OK || je_arm_pages.free_count != free_count - JE_ALLOC_MAX_PAGES)
        physmem_fatal("allocmax", status);
    console_write("JeiOS/aarch64: physmem allocmax pa=");
    console_hex(run);
    console_write("\r\n");
    status = je_pages_free(&je_arm_pages, run, JE_ALLOC_MAX_PAGES);
    if (status != JE_OK || je_arm_pages.free_count != free_count)
        physmem_fatal("freemax", status);

    /*
     * Over-cap bounds proof (NOT a rollback claim): a request larger than
     * JE_ALLOC_MAX_PAGES is rejected as INVALID input by the allocator's tested
     * contract BEFORE any scan, and must not touch state. The exhaustive
     * "failed multi-page alloc frees nothing" rollback property is proven on a
     * small fragmented window by the hosted test (tests/test_physmem.c), which
     * can force free_count>=n with no contiguous run of n — impossible to set up
     * cheaply on this 65k-page live window. Keeping this marker honestly labeled
     * avoids a vacuous "rollback ok" that would pass even if rollback were broken.
     */
    status = je_pages_alloc(&je_arm_pages, JE_ALLOC_MAX_PAGES + 1u, &run);
    if (status != JE_ERR_INVALID || je_arm_pages.free_count != free_count)
        physmem_fatal("overcap", status);
    console_write("JeiOS/aarch64: physmem overcap rejected pages=");
    console_hex((uint64_t)je_arm_pages.free_count);
    console_write("\r\n");
}

static uint64_t read_sctlr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(value));
    return value;
}

static bool par_fault(uint64_t par)
{
    return (par & UINT64_C(1)) != 0u;
}

static bool par_pa_is(uint64_t par, uint64_t physical)
{
    return !par_fault(par) &&
           (par & UINT64_C(0x0000fffffffff000)) ==
           (physical & UINT64_C(0x0000fffffffff000));
}

static bool par_attr_is(uint64_t par, uint64_t attr)
{
    return !par_fault(par) &&
           ((par >> 56) & UINT64_C(0xff)) == attr;
}

#ifdef JE_ARM_USER_TEST
static bool par_permission_l3(uint64_t par)
{
    return (par & UINT64_C(0xfff)) == UINT64_C(0x81f);
}

static uint64_t je_arm_user_audit_cycles;
static uint64_t je_arm_user_audit_reads;

static _Noreturn void user_test_fatal(const char *reason)
{
    console_write("JeiOS/aarch64: FATAL user-test ");
    console_write(reason);
    console_write("\r\n");
    halt();
}

static void user_test_prepare(void)
{
    for (size_t i = 0u; i < JE_PAGE_SIZE; ++i) {
        je_arm_user_blob[i] = UINT8_C(0xa1);
        je_arm_user_blob[JE_PAGE_SIZE + i] = UINT8_C(0xb2);
        je_arm_user_blob[2u * JE_PAGE_SIZE + i] = UINT8_C(0xc3);
    }

    const je_vm_ops *ops = je_arch_vm_ops(&je_arm_pages, 1);
    je_status status = je_vm_init(&je_arm_user_vm, ops, &je_arm_pages, 0u);
    if (status != JE_OK) user_test_fatal("vm-init");

    uint32_t p1 = UINT32_MAX;
    uint32_t p2 = UINT32_MAX;
    status = je_vm_build(&je_arm_user_vm, &je_arm_user_image,
                         je_arm_user_blob, sizeof(je_arm_user_blob), 1, &p1);
    if (status != JE_OK || p1 != 0u) user_test_fatal("build-p1");
    status = je_vm_build(&je_arm_user_vm, &je_arm_user_image,
                         je_arm_user_blob, sizeof(je_arm_user_blob), 1, &p2);
    if (status != JE_OK || p2 != 1u) user_test_fatal("build-p2");

    status = je_vm_audit(&je_arm_user_vm, p1);
    if (status != JE_OK) user_test_fatal("audit-p1");
    status = je_vm_audit(&je_arm_user_vm, p2);
    if (status != JE_OK) user_test_fatal("audit-p2");
}

static void user_test_probe(const char *name, uint64_t va, int write,
                            uint64_t *out_par)
{
    const uint64_t par = je_arm_at_s1e0(va, write);
    console_write("JeiOS/aarch64: user-test ");
    console_write(name);
    console_write(" va=");
    console_hex(va);
    console_write(" par=");
    console_hex(par);
    console_write("\r\n");
    if (out_par != NULL) *out_par = par;
}

static uint64_t user_test_install(const char *name, uint64_t root)
{
    je_arm_sched_install_ttbr0(root, 1u);
    const uint64_t ttbr0 = je_arm_sched_read_ttbr0();
    console_write("JeiOS/aarch64: user-test install ");
    console_write(name);
    console_write(" root=");
    console_hex(root);
    console_write(" ttbr0=");
    console_hex(ttbr0);
    console_write(" tlbi=");
    console_hex(je_arm_tlbi_hosted_last_operand);
    console_write("\r\n");
    return ttbr0;
}

static _Noreturn void user_test_run(void)
{
    const uint64_t kernel_text = (uint64_t)(uintptr_t)_text;
    const uint64_t kernel_data = (uint64_t)(uintptr_t)&je_arm_bss_probe;
    const uint64_t code = JE_VM_ARENA_BASE;
    const uint64_t rodata = JE_VM_ARENA_BASE + JE_PAGE_SIZE;
    const uint64_t data = JE_VM_ARENA_BASE + 2u * JE_PAGE_SIZE;
    const uint64_t p1_root = (uint64_t)je_arm_user_vm.procs[0].root;
    const uint64_t p2_root = (uint64_t)je_arm_user_vm.procs[1].root;
    uint64_t p1_kernel_read_par = 0u;
    uint64_t p1_kernel_write_par = 0u;
    uint64_t p1_code_read_par = 0u;
    uint64_t p1_code_write_par = 0u;
    uint64_t p1_data_par = 0u;
    uint64_t p1_rodata_read_par = 0u;
    uint64_t p1_rodata_write_par = 0u;
    uint64_t p1_uart_par = 0u;
    uint64_t p1_gicd_par = 0u;
    uint64_t p2_data_par = 0u;
    uint64_t p1_ttbr0;
    uint64_t p2_ttbr0;
    uint64_t p1_tlbi_operand;
    uint64_t p2_tlbi_operand;

    p1_ttbr0 = user_test_install("p1", p1_root);
    p1_tlbi_operand = je_arm_tlbi_hosted_last_operand;
    user_test_probe("p1 kernel-read", kernel_text, 0, &p1_kernel_read_par);
    user_test_probe("p1 kernel-write", kernel_data, 1, &p1_kernel_write_par);
    user_test_probe("p1 code-read", code, 0, &p1_code_read_par);
    user_test_probe("p1 code-write", code, 1, &p1_code_write_par);
    user_test_probe("p1 data-write", data, 1, &p1_data_par);
    user_test_probe("p1 rodata-read", rodata, 0, &p1_rodata_read_par);
    user_test_probe("p1 rodata-write", rodata, 1, &p1_rodata_write_par);
    user_test_probe("p1 uart-read", PL011_BASE, 0, &p1_uart_par);
    user_test_probe("p1 gicd-read", JE_GICD_BASE, 0, &p1_gicd_par);

    const uint64_t code_pa = p1_code_read_par & UINT64_C(0x0000fffffffff000);
    if (je_arm_mmu_map_page(&je_arm_mmu_context, code_pa, code_pa,
                            JE_MAP_RW) != JE_ERR_DENIED) {
        user_test_fatal("wx-ledger");
    }
    console_write("JeiOS/aarch64: user-test wx-refuse code-pa=");
    console_hex(code_pa);
    console_write(" status=DENIED\r\n");

    p2_ttbr0 = user_test_install("p2", p2_root);
    p2_tlbi_operand = je_arm_tlbi_hosted_last_operand;
    user_test_probe("p2 data-read", data, 0, &p2_data_par);
    console_write("JeiOS/aarch64: user-test isolate p1-par=");
    console_hex(p1_data_par);
    console_write(" p2-par=");
    console_hex(p2_data_par);
    console_write("\r\n");

    const uint64_t root_mask = UINT64_C(0x0000fffffffff000);
    if ((p1_ttbr0 & root_mask) != p1_root ||
        (p2_ttbr0 & root_mask) != p2_root ||
        ((p1_ttbr0 >> 48) & UINT64_C(0xff)) != 1u ||
        ((p2_ttbr0 >> 48) & UINT64_C(0xff)) != 1u ||
        !par_permission_l3(p1_kernel_read_par) ||
        !par_permission_l3(p1_kernel_write_par) ||
        !par_permission_l3(p1_uart_par) ||
        !par_permission_l3(p1_gicd_par) ||
        par_fault(p1_code_read_par) ||
        !par_permission_l3(p1_code_write_par) ||
        par_fault(p1_data_par) ||
        par_fault(p1_rodata_read_par) ||
        !par_permission_l3(p1_rodata_write_par) ||
        par_fault(p2_data_par) ||
        (p1_data_par & UINT64_C(0x0000fffffffff000)) ==
            (p2_data_par & UINT64_C(0x0000fffffffff000))) {
        user_test_fatal("probe-mismatch");
    }

    console_write("JeiOS/aarch64: user-test audit PASS\r\n");
    console_write("JeiOS/aarch64: user-test tlbi p1 operand=");
    console_hex(p1_tlbi_operand);
    console_write("\r\n");
    console_write("JeiOS/aarch64: user-test tlbi p2 operand=");
    console_hex(p2_tlbi_operand);
    console_write("\r\n");
    /* The barrier/TLBI effects are reviewed, not observed under QEMU TCG. */
    je_arm_sched_install_ttbr0(je_arm_mmu_root(&je_arm_mmu_context), 0u);
    console_write("JeiOS/aarch64: user-test PASS reviewed-tlb=not-observed\r\n");
    halt();
}

static void user_test_prepare_l3(void)
{
    const je_status status = je_arm_mmu_prebuild_ram_l3(&je_arm_mmu_context);
    if (status != JE_OK) user_test_fatal("l3-prebuild");
}
#endif

/*
 * Exact default-path marker order (complete lines; smoke_arm.py matches these):
 *   JeiOS/aarch64: mmu build root=<hex>
 *   JeiOS/aarch64: mmu map text=[<hex>,<hex>) rodata=[<hex>,<hex>) data=[<hex>,<hex>) device=<hex> gicd=<hex> gicc=<hex>
 *   JeiOS/aarch64: mmu audit ok leaves=<hex>
 *   JeiOS/aarch64: mmu enabled sctlr=<hex>
 *   JeiOS/aarch64: mmu xlate mapped va=<hex> par=<hex>
 *   JeiOS/aarch64: mmu xlate guard va=<hex> par=<hex>
 *   JeiOS/aarch64: mmu attr uart va=<hex> par=<hex>
 *   JeiOS/aarch64: mmu xlate gicd va=<hex> par=<hex>
 *   JeiOS/aarch64: mmu xlate gicc va=<hex> par=<hex>
 *   JeiOS/aarch64: mmu xlate gicc_dir va=<hex> par=<hex>
 *
 * The three GIC probes (MP2 PR-2a, ADR 0012 D2) are independent AT S1E1R
 * checks that GICD/GICC translate identity as Device-nGnRnE (MAIR Attr 0x00)
 * and that GICC_DIR (GICC + 0x1000, the second GICC page) is deliberately
 * UNMAPPED (PAR.F=1), so an EOImode=1 / GICC_DIR write would fault rather than
 * silently deactivate. smoke_arm.py re-decodes each PAR field by field.
 */
static void mmu_bringup(void)
{
    /* PARange gate (ADR 0011 D3/D7): the MMU pins TCR.IPS to the 44-bit PA
     * profile. A part reporting a NARROWER PARange than 44 cannot honor that
     * IPS, so refuse rather than enable a translation regime wider than the
     * implemented PA (AGENTS.md rule 5: refuse when a guarantee cannot be
     * delivered). pa_bits_from_mmfr0 already maps the encoding to a bit count
     * (0 for the reserved encoding, rejected in dtb_intake). cortex-a72 reports
     * 44, so this does not fire on the pinned target; a fault-injection build
     * could force a lower value. */
    if (je_arm_facts.pa_bits < 44u) {
        console_write("JeiOS/aarch64: FATAL mmu PARange below 44-bit profile pa_bits=");
        console_dec((uint64_t)je_arm_facts.pa_bits);
        console_write("\r\n");
        halt();
    }
    const uint64_t text_start = (uint64_t)(uintptr_t)_text;
    const uint64_t text_end = (uint64_t)(uintptr_t)__text_end;
    const uint64_t rodata_start = text_end;
    const uint64_t rodata_end = (uint64_t)(uintptr_t)__rodata_end;
    const uint64_t data_start = rodata_end;
    const uint64_t data_end = (uint64_t)(uintptr_t)__data_end;
    const uint64_t stack_start = (uint64_t)(uintptr_t)__stack_bottom;
    const uint64_t image_end = (uint64_t)(uintptr_t)_end;
    const uint64_t guard = (uint64_t)(uintptr_t)__stack_guard;
    const uint64_t device = (uint64_t)PL011_BASE;
    const uint64_t gicd = JE_GICD_BASE;
    const uint64_t gicc = JE_GICC_BASE;
    const je_map_region regions[] = {
        { text_start, text_end - text_start, JE_MAP_TEXT },
        { rodata_start, rodata_end - rodata_start, JE_MAP_RODATA },
        { data_start, data_end - data_start, JE_MAP_RW },
        { stack_start, image_end - stack_start, JE_MAP_RW },
        { device, JE_PAGE_SIZE, JE_MAP_DEVICE },
        { gicd, JE_GIC_PAGE, JE_MAP_DEVICE },
        { gicc, JE_GIC_PAGE, JE_MAP_DEVICE },
    };
    uint64_t root = 0u;
    je_status status = je_arm_mmu_build(&je_arm_mmu_context, &je_arm_pages,
                                         regions, sizeof(regions) / sizeof(regions[0]),
                                         &root);
    if (status != JE_OK) {
        console_write("JeiOS/aarch64: FATAL mmu build status=");
        console_dec((uint64_t)status);
        console_write("\r\n");
        halt();
    }
    console_write("JeiOS/aarch64: mmu build root=");
    console_hex(root);
    console_write("\r\n");
    console_write("JeiOS/aarch64: mmu map text=[");
    console_hex(text_start);
    console_write(",");
    console_hex(text_end);
    console_write(") rodata=[");
    console_hex(rodata_start);
    console_write(",");
    console_hex(rodata_end);
    console_write(") data=[");
    console_hex(data_start);
    console_write(",");
    console_hex(data_end);
    console_write(") device=");
    console_hex(device);
    console_write(" gicd=");
    console_hex(gicd);
    console_write(" gicc=");
    console_hex(gicc);
    console_write("\r\n");

#if defined(JE_ARM_USER_TEST) || defined(JE_ARM_EL0_TEST)
#ifdef JE_ARM_USER_TEST
    user_test_prepare_l3();
#else
    status = je_arm_mmu_prebuild_ram_l3(&je_arm_mmu_context);
    if (status != JE_OK) {
        console_write("JeiOS/aarch64: FATAL el0-test l3-prebuild\r\n");
        halt();
    }
#endif
#endif

#ifdef JE_ARM_USER_TEST
    const uint64_t audit_start = je_arm_read_cntpct();
#endif
    status = je_arm_mmu_audit(&je_arm_mmu_context, regions,
                              sizeof(regions) / sizeof(regions[0]));
#ifdef JE_ARM_USER_TEST
    je_arm_user_audit_cycles = je_arm_read_cntpct() - audit_start;
    je_arm_user_audit_reads = je_arm_mmu_last_audit_reads;
#endif
    if (status != JE_OK) {
        console_write("JeiOS/aarch64: FATAL mmu audit status=");
        console_dec((uint64_t)status);
        console_write("\r\n");
        halt();
    }
    console_write("JeiOS/aarch64: mmu audit ok leaves=");
    console_hex(je_arm_mmu_last_audit_leaves);
    console_write("\r\n");

#if defined(JE_ARM_USER_TEST) || defined(JE_ARM_EL0_TEST)
    /* MP3a-1 changes the kernel-root L0 attributes only after the frozen MP2
     * audit. Do this once, explicitly, before any process root is built; a
     * lazy decoration during link_tables would be a live table mutation. */
    status = je_arch_vm_decorate_kernel_root();
    if (status != JE_OK) {
        console_write("JeiOS/aarch64: FATAL user-test kernel-decoration\r\n");
        halt();
    }
#ifdef JE_ARM_USER_TEST
    user_test_prepare();
#endif
#endif

    status = je_arm_mmu_enable(&je_arm_mmu_context);
    if (status != JE_OK) {
        console_write("JeiOS/aarch64: FATAL mmu enable status=");
        console_dec((uint64_t)status);
        console_write("\r\n");
        halt();
    }
    uint64_t sctlr = read_sctlr_el1();
    console_write("JeiOS/aarch64: mmu enabled sctlr=");
    console_hex(sctlr);
    console_write("\r\n");

    uint64_t mapped_par = je_arm_at_s1e1(text_start, 0);
    console_write("JeiOS/aarch64: mmu xlate mapped va=");
    console_hex(text_start);
    console_write(" par=");
    console_hex(mapped_par);
    console_write("\r\n");
    uint64_t guard_par = je_arm_at_s1e1(guard, 0);
    console_write("JeiOS/aarch64: mmu xlate guard va=");
    console_hex(guard);
    console_write(" par=");
    console_hex(guard_par);
    console_write("\r\n");
    uint64_t uart_par = je_arm_at_s1e1(device, 0);
    console_write("JeiOS/aarch64: mmu attr uart va=");
    console_hex(device);
    console_write(" par=");
    console_hex(uart_par);
    console_write("\r\n");
    const uint64_t gicc_dir = gicc + JE_GIC_PAGE;
    uint64_t gicd_par = je_arm_at_s1e1(gicd, 0);
    console_write("JeiOS/aarch64: mmu xlate gicd va=");
    console_hex(gicd);
    console_write(" par=");
    console_hex(gicd_par);
    console_write("\r\n");
    uint64_t gicc_par = je_arm_at_s1e1(gicc, 0);
    console_write("JeiOS/aarch64: mmu xlate gicc va=");
    console_hex(gicc);
    console_write(" par=");
    console_hex(gicc_par);
    console_write("\r\n");
    uint64_t gicc_dir_par = je_arm_at_s1e1(gicc_dir, 0);
    console_write("JeiOS/aarch64: mmu xlate gicc_dir va=");
    console_hex(gicc_dir);
    console_write(" par=");
    console_hex(gicc_dir_par);
    console_write("\r\n");

    if ((sctlr & JE_ARM_SCTLR_SET) != JE_ARM_SCTLR_SET ||
        !par_pa_is(mapped_par, text_start) || !par_fault(guard_par) ||
        !par_attr_is(uart_par, UINT64_C(0x00)) ||
        !par_pa_is(gicd_par, gicd) || !par_attr_is(gicd_par, UINT64_C(0x00)) ||
        !par_pa_is(gicc_par, gicc) || !par_attr_is(gicc_par, UINT64_C(0x00)) ||
        !par_fault(gicc_dir_par)) {
        console_write("JeiOS/aarch64: FATAL mmu probe mismatch\r\n");
        halt();
    }

#ifdef JE_ARM_USER_TEST
    /*
     * The user profile prebuilds an L3 path for every managed 2 MiB RAM slot,
     * so this records the enlarged pre-enable audit input. CNTVCT is a
     * coarse QEMU execution counter, not a wall-clock or hardware timing
     * qualification; the exact descriptor-read count is the stable evidence.
     */
    console_write("JeiOS/aarch64: user-test pre-enable-audit cycles=");
    console_hex(je_arm_user_audit_cycles);
    console_write(" reads=");
    console_hex(je_arm_user_audit_reads);
    console_write("\r\n");
#endif
}

static uint64_t je_arm_timer_freq;
static uint32_t je_arm_timer_interval;
volatile uint64_t je_arm_irq_tick_count;

typedef struct {
    uint64_t vector;
    uint64_t intid;
    uint64_t isr;
    uint64_t spsr;
    uint64_t elr;
    uint64_t sp;
    uint64_t cntpct;
    uint64_t heartbeat;
} je_arm_irq_record;

#define JE_ARM_IRQ_EVIDENCE_CAP 4u
static je_arm_irq_record je_arm_irq_records[JE_ARM_IRQ_EVIDENCE_CAP];
static je_arm_irq_record je_arm_irq_last_busy_wakeup;
static volatile uint64_t je_arm_irq_busy_wakeups;
static volatile uint64_t je_arm_irq_invalid_wakeups;

#ifdef JE_ARM_TIMER_TEST
volatile uint64_t je_arm_canary_heartbeat;
volatile uint64_t je_arm_canary_fail_reg = UINT64_MAX;
volatile uint64_t je_arm_canary_deadline;
extern void je_arm_canary_loop(void);
extern void je_arm_canary_hi_loop(void);
#endif

/*
 * GIC/timer marker order (complete lines, after all MMU markers):
 *   JeiOS/aarch64: gic init gicd_typer=<hex> gicd_iidr=<hex> gicc_iidr=<hex>
 *   JeiOS/aarch64: gic config gicc_ctlr=<hex> gicc_pmr=<hex> gicd_ctlr=<hex> isenabler0=<hex> igroupr0=<hex> prio30=<hex>
 *   JeiOS/aarch64: timer cntfrq=<hex> interval=<hex> intid=0x000000000000001e
 *   [JE_ARM_TIMER_POLL only] JeiOS/aarch64: timer poll n=<hex> intid=0x000000000000001e cntpct=<hex> ticks=<hex>
 *   [JE_ARM_TIMER_POLL only] JeiOS/aarch64: timer poll PASS cycles=0x0000000000000004
 *   [JE_ARM_TIMER_TEST only] JeiOS/aarch64: irq tick n=<hex> vector=<hex> intid=0x..1e isr=<hex> spsr=<hex> elr=<hex> sp=<hex> cntpct=<hex> hb=<hex>
 *   [JE_ARM_TIMER_TEST only] JeiOS/aarch64: irq timer PASS ticks=0x...4
 *   JeiOS/aarch64: initialization complete; halted
 *
 * PR-2a intentionally has no spurious injection in the live QEMU path: the
 * test author covers the IAR==1023/no-EOIR case with the hosted fake backend.
 */
#ifdef JE_ARM_TIMER_POLL
static void timer_poll_fatal(const char *reason)
{
    console_write("JeiOS/aarch64: FATAL timer poll ");
    console_write(reason);
    console_write("\r\n");
    halt();
}

static void timer_poll_proof(void)
{
    const uint32_t cycles = 4u;
    const uint64_t max_wait = (uint64_t)je_arm_timer_interval * 2u;
    uint64_t previous_ticks = je_arm_timer_ticks();
    uint64_t previous_cntpct = 0u;

    for (uint32_t n = 1u; n <= cycles; ++n) {
        if (!je_timer_poll_pending(max_wait)) timer_poll_fatal("no-tick");

        bool was_spurious = false;
        uint64_t cntpct = 0u;
        je_status status = je_gic_handle_one(je_arm_timer_interval,
                                             &was_spurious, &cntpct);
        if (status != JE_OK) timer_poll_fatal("handle-failed");
        if (was_spurious) timer_poll_fatal("unexpected-spurious");

        const uint64_t ticks = je_arm_timer_ticks();
        if (ticks <= previous_ticks ||
            (n > 1u && cntpct - previous_cntpct <
                       (uint64_t)je_arm_timer_interval))
            timer_poll_fatal("invariant");

        console_write("JeiOS/aarch64: timer poll n=");
        console_hex((uint64_t)n);
        console_write(" intid=");
        console_hex((uint64_t)JE_TIMER_INTID);
        console_write(" cntpct=");
        console_hex(cntpct);
        console_write(" ticks=");
        console_hex(ticks);
        console_write("\r\n");
        previous_ticks = ticks;
        previous_cntpct = cntpct;
    }

    console_write("JeiOS/aarch64: timer poll PASS cycles=");
    console_hex((uint64_t)cycles);
    console_write("\r\n");
}
#endif

/* Returning IRQ handler (ADR 0012 D5/D6/D8). Caller context is normally EL1h
 * with I masked by exception entry; the MP3a-2 lower-EL IRQ saver reaches the
 * same bounded handler with an SP_EL0-backed frame. This path performs no
 * allocation, blocking, console I/O or unmasking. The assembly saver owns the
 * transient guard and restores ELR/SPSR/GPRs after this function returns. */
void *je_arm_irq(je_arm_trapframe *tf)
{
    /* Sample at handler entry, before ACK/EOI can change interrupt status. */
    const uint64_t isr = je_arm_read_isr_el1();
    bool was_spurious = false;
    uint64_t cntpct = 0u;
    const je_status status = je_gic_handle_one(je_arm_timer_interval,
                                               &was_spurious, &cntpct);
    const uint32_t iar = je_gic_last_iar();

    if (status == JE_OK && !was_spurious) {
        ++je_arm_irq_tick_count;
        const uint64_t tick = je_arm_irq_tick_count;
        if (tick <= (uint64_t)JE_ARM_IRQ_EVIDENCE_CAP) {
            je_arm_irq_record *const evidence =
                &je_arm_irq_records[tick - UINT64_C(1)];
            evidence->vector = tf->vector;
            evidence->intid = (uint64_t)(iar & UINT32_C(0x3ff));
            evidence->isr = isr;
            evidence->spsr = tf->spsr;
            evidence->elr = tf->elr;
            evidence->sp = ((tf->spsr & JE_ARM_SPSR_M_MASK) ==
                            JE_ARM_SPSR_M_EL0T)
                               ? *je_arm_tf_user_sp(tf)
                               : *je_arm_tf_kernel_sp(tf);
            evidence->cntpct = cntpct;
#ifdef JE_ARM_TIMER_TEST
            evidence->heartbeat = je_arm_canary_heartbeat;
#else
            evidence->heartbeat = 0u;
#endif
        }
        return je_arm_sched_irq(tf);
    } else if (status == JE_ERR_BUSY) {
        /* A timer line that re-pended benignly was acknowledged and EOIRed by
         * gic.c but did not produce a tick. Retain the last wakeup evidence
         * without pretending that the scheduler-visible counter advanced. */
        je_arm_irq_last_busy_wakeup.vector = tf->vector;
        je_arm_irq_last_busy_wakeup.intid =
            (uint64_t)(iar & UINT32_C(0x3ff));
        je_arm_irq_last_busy_wakeup.isr = isr;
        je_arm_irq_last_busy_wakeup.spsr = tf->spsr;
        je_arm_irq_last_busy_wakeup.elr = tf->elr;
        je_arm_irq_last_busy_wakeup.sp =
            ((tf->spsr & JE_ARM_SPSR_M_MASK) == JE_ARM_SPSR_M_EL0T)
                ? *je_arm_tf_user_sp(tf)
                : *je_arm_tf_kernel_sp(tf);
        je_arm_irq_last_busy_wakeup.cntpct = cntpct;
#ifdef JE_ARM_TIMER_TEST
        je_arm_irq_last_busy_wakeup.heartbeat = je_arm_canary_heartbeat;
#else
        je_arm_irq_last_busy_wakeup.heartbeat = 0u;
#endif
        ++je_arm_irq_busy_wakeups;
        return je_arm_sched_no_tick(tf);
    } else if (status != JE_OK) {
        /* status == JE_ERR_INVALID: an UNEXPECTED non-spurious INTID. Only
         * INTID 30 is ever enabled (je_gic_init), so this is a hard invariant
         * break (gic.c already EOIR'd it to avoid a stuck-active line). ADR 0012
         * D6 requires FATAL. Halting from IRQ context is correct for an
         * unrecoverable break; emit one bounded FATAL line and halt (this is a
         * terminal line, not a torn proof marker). */
        ++je_arm_irq_invalid_wakeups;
        console_write("JeiOS/aarch64: FATAL irq unexpected intid=");
        console_hex((uint64_t)(iar & UINT32_C(0x3ff)));
        console_write("\r\n");
        halt();
    }
    return je_arm_sched_no_tick(tf);
}

#ifdef JE_ARM_TIMER_TEST
static void arm_canary_set_deadline(void)
{
    const uint64_t budget = (uint64_t)je_arm_timer_interval * UINT64_C(1024);
    const uint64_t now = je_arm_read_cntpct();
    je_arm_canary_deadline = now + (budget != 0u ? budget : UINT64_C(1));
}

_Noreturn void je_arm_timer_stalled(void)
{
    console_write("JeiOS/aarch64: FATAL timer stalled\r\n");
    halt();
}

static void irq_timer_test_fatal(const char *reason)
{
    console_write("JeiOS/aarch64: FATAL irq timer test ");
    console_write(reason);
    console_write("\r\n");
    halt();
}

static void print_irq_timer_evidence(void)
{
    const uint64_t ticks = je_arm_irq_tick_count;
    if (ticks < UINT64_C(4) ||
        je_arm_irq_busy_wakeups != 0u ||
        je_arm_irq_invalid_wakeups != 0u) {
        irq_timer_test_fatal("invariant");
    }
    /* The PASS line reports the number of RECORDED+verified evidence ticks
     * (capped at the evidence array size), not the live counter — which keeps
     * the marker deterministic even though the (b) canary-hi phase advances the
     * live counter by JE_ARM_CANARY_HI_TICKS more ticks after these were
     * recorded. */
    const uint64_t recorded = ticks < (uint64_t)JE_ARM_IRQ_EVIDENCE_CAP
                                  ? ticks : (uint64_t)JE_ARM_IRQ_EVIDENCE_CAP;

    for (uint64_t i = 0u; i < ticks &&
                           i < (uint64_t)JE_ARM_IRQ_EVIDENCE_CAP; ++i) {
        const je_arm_irq_record *const evidence = &je_arm_irq_records[i];
        console_write("JeiOS/aarch64: irq tick n=");
        console_hex(i + UINT64_C(1));
        console_write(" vector=");
        console_hex(evidence->vector);
        console_write(" intid=");
        console_hex(evidence->intid);
        console_write(" isr=");
        console_hex(evidence->isr);
        console_write(" spsr=");
        console_hex(evidence->spsr);
        console_write(" elr=");
        console_hex(evidence->elr);
        console_write(" sp=");
        console_hex(evidence->sp);
        console_write(" cntpct=");
        console_hex(evidence->cntpct);
        console_write(" hb=");
        console_hex(evidence->heartbeat);
        console_write("\r\n");
    }
    console_write("JeiOS/aarch64: irq timer PASS ticks=");
    console_hex(recorded);
    console_write("\r\n");
}

__attribute__((noinline, noclone))
static void trigger_irq_brk(void)
{
    __asm__ volatile(".globl je_arm_irq_brk_site\n"
                     "je_arm_irq_brk_site: brk #0xB2B2");
}

static void irq_timer_test(void)
{
    je_arm_irq_tick_count = 0u;
    je_arm_canary_heartbeat = 0u;
    je_arm_canary_fail_reg = UINT64_MAX;
    je_arm_irq_busy_wakeups = 0u;
    je_arm_irq_invalid_wakeups = 0u;
    arm_canary_set_deadline();
    je_arm_canary_loop();
    if (je_arm_canary_fail_reg != UINT64_MAX) {
        console_write("JeiOS/aarch64: FATAL irq canary mismatch reg=");
        console_dec(je_arm_canary_fail_reg);
        console_write("\r\n");
        halt();
    }
    /* (b) Rotating-scratch phase: the primary loop proved x0..x28 survive an IRQ
     * return but cannot check its own scratch/LR (x29,x30). This second phase
     * checks all x0..x30 while using x0..x2 from the saved canary set as
     * scratch, over JE_ARM_CANARY_HI_TICKS more live ticks, with a final
     * I-masked re-check. */
    je_arm_canary_fail_reg = UINT64_MAX;
    arm_canary_set_deadline();
    je_arm_canary_hi_loop();
    if (je_arm_canary_fail_reg != UINT64_MAX) {
        console_write("JeiOS/aarch64: FATAL irq canary-hi mismatch reg=");
        console_dec(je_arm_canary_fail_reg);
        console_write("\r\n");
        halt();
    }
    console_write("JeiOS/aarch64: irq canary-hi PASS (x29,x30)\r\n");
    print_irq_timer_evidence();
    /* Exact ordered negative-control marker literals for the smoke author:
     *   JeiOS/aarch64: triggering deliberate IRQ-path BRK #0xb2b2 ...
     *   JeiOS/aarch64: EXCEPTION vector=0x0000000000000004 ec=0x000000000000003c ...
     *   JeiOS/aarch64: exception handled; halted */
    console_write("JeiOS/aarch64: triggering deliberate IRQ-path BRK #0xb2b2 ...\r\n");
    trigger_irq_brk();
    console_write("JeiOS/aarch64: FATAL IRQ-path BRK did not vector\r\n");
    halt();
}
#endif

static void gic_timer_bringup(void)
{
    je_gic_evidence evidence = { 0 };
    je_status status = je_gic_init(&evidence);
    if (status != JE_OK) {
        console_write("JeiOS/aarch64: FATAL gic init status=");
        console_dec((uint64_t)status);
        console_write("\r\n");
        halt();
    }

    console_write("JeiOS/aarch64: gic init gicd_typer=");
    console_hex((uint64_t)evidence.gicd_typer);
    console_write(" gicd_iidr=");
    console_hex((uint64_t)evidence.gicd_iidr);
    console_write(" gicc_iidr=");
    console_hex((uint64_t)evidence.gicc_iidr);
    console_write("\r\n");
    console_write("JeiOS/aarch64: gic config gicc_ctlr=");
    console_hex((uint64_t)evidence.gicc_ctlr);
    console_write(" gicc_pmr=");
    console_hex((uint64_t)evidence.gicc_pmr);
    console_write(" gicd_ctlr=");
    console_hex((uint64_t)evidence.gicd_ctlr);
    console_write(" isenabler0=");
    console_hex((uint64_t)evidence.isenabler0);
    console_write(" igroupr0=");
    console_hex((uint64_t)evidence.igroupr0);
    console_write(" prio30=");
    console_hex((uint64_t)evidence.ipriority30);
    console_write("\r\n");

    status = je_timer_arm(&je_arm_timer_freq, &je_arm_timer_interval);
    if (status != JE_OK) {
        console_write("JeiOS/aarch64: FATAL timer arm status=");
        console_dec((uint64_t)status);
        console_write("\r\n");
        halt();
    }
    console_write("JeiOS/aarch64: timer cntfrq=");
    console_hex(je_arm_timer_freq);
    console_write(" interval=");
    console_hex((uint64_t)je_arm_timer_interval);
    console_write(" intid=");
    console_hex((uint64_t)JE_TIMER_INTID);
    console_write("\r\n");

#ifdef JE_ARM_TIMER_POLL
    timer_poll_proof();
#endif
}

#ifdef JE_ARM_MMU_FAULT
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_TEXT || JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_GUARD
/* The one store both data-abort modes use; ELR must equal this label. */
__attribute__((noinline, noclone))
static void mmu_fault_store(uint64_t va)
{
    __asm__ volatile(".globl je_arm_mmu_fault_store_site\n"
                     "je_arm_mmu_fault_store_site: str xzr, [%0]"
                     :: "r"(va) : "memory");
}

#else
/* Branch-with-link to `va`. If the target WERE executable it holds `ret`, so
 * control returns here and the caller reports FATAL (no silent pass). */
__attribute__((noinline, noclone))
static void mmu_fault_exec(uint64_t va)
{
    __asm__ volatile("blr %0" :: "r"(va) : "x30", "memory");
}
#endif

/* Marker (complete line, asserted by smoke_arm.py --mmu-fault):
 *   JeiOS/aarch64: mmu fault test <name> target=<hex> par_r=<hex> par_w=<hex>
 * par_r/par_w are AT S1E1R/S1E1W probes of the target taken BEFORE the access:
 * independent translation-table evidence of the expected R/W state (e.g. the
 * .data buffer must be R+W so its execute fault cannot be an unmapped-page false
 * pass; the guard must not translate at all). */
static void mmu_fault_trigger(void)
{
    const uint64_t target = mmu_fault_target();
    const uint64_t par_r = je_arm_at_s1e1(target, 0);
    const uint64_t par_w = je_arm_at_s1e1(target, 1);
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_DATA
    /* Prove the buffer is ordinary readable+writable memory by an actual
     * write/read-back round trip (not only the AT probe), then restore RET. */
    volatile uint32_t *buf = (volatile uint32_t *)je_arm_mmu_fault_data_insn;
    buf[0] = 0u;
    const uint32_t zero_back = buf[0];
    buf[0] = A64_RET;
    const uint32_t ret_back = buf[0];
    if (zero_back != 0u || ret_back != A64_RET) {
        console_write("JeiOS/aarch64: FATAL mmu fault test .data buffer not RW\r\n");
        halt();
    }
#endif
    console_write("JeiOS/aarch64: mmu fault test ");
    console_write(mmu_fault_name());
    console_write(" target=");
    console_hex(target);
    console_write(" par_r=");
    console_hex(par_r);
    console_write(" par_w=");
    console_hex(par_w);
    console_write("\r\n");
#if JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_TEXT || JE_ARM_MMU_FAULT == JE_ARM_MMU_FAULT_GUARD
    mmu_fault_store(target);
#else
    mmu_fault_exec(target);
#endif
    /* Reached only if the access did NOT fault: the property is broken. */
    console_write("JeiOS/aarch64: FATAL mmu fault test ");
    console_write(mmu_fault_name());
    console_write(" did not fault\r\n");
    halt();
}
#endif

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

    /* MP1 (ADR 0002 D5-3): install the EL1 exception vectors and prove it with a
     * VBAR readback. From here an unexpected exception is a bounded diagnostic
     * halt instead of a jump into an UNKNOWN VBAR. */
    install_vectors();
    console_write("JeiOS/aarch64: vectors installed vbar=");
    console_hex(read_vbar_el1());
    console_write("\r\n");

    /* Issue #52: bounded DTB copy + parse + boot-context validation. Placed
     * after install_vectors() so a fault while reading firmware memory is a
     * diagnostic, and before the vector self-test so every build exercises it.
     * Halts (FATAL marker) on any rejection. */
    dtb_intake();
    physmem_bringup();
    mmu_bringup();
#ifdef JE_ARM_USER_TEST
    user_test_run();
#endif
    je_arm_sched_init();
    gic_timer_bringup();
#ifdef JE_ARM_EL0_TEST
    je_arm_el0_test_run();
#endif
#ifdef JE_ARM_SCHED_TEST
    je_arm_sched_test();
#endif
#ifdef JE_ARM_TIMER_TEST
    /* gic_timer_bringup has configured and armed the source; vectors were
     * installed before DTB/MMU/GIC work above. The canary loop performs the
     * first and only DAIF.I unmask, then prints evidence after it returns. */
    irq_timer_test();
#endif
#ifdef JE_ARM_MMU_FAULT
    mmu_fault_trigger();   /* faults into je_arm_exception, or FATAL-halts */
#endif
#ifdef JE_ARM_VECTOR_TEST
    /* Deliberate synchronous exception: trigger the planted BRK. It vectors into
     * je_arm_exception, which asserts the exact syndrome, prints PASS, and halts
     * — so control never returns here. */
    console_write("JeiOS/aarch64: triggering deliberate BRK #0xa55a ...\r\n");
    trigger_brk();
    /* Unreachable: the BRK handler halts. If we get here the BRK did not vector. */
    console_write("JeiOS/aarch64: FATAL BRK did not vector\r\n");
    halt();
#endif

    console_write("JeiOS/aarch64: initialization complete; halted\r\n");
    halt();
}
