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
#include "trapframe.h"

/* MP1 reentrancy guard (vectors.S): 0 outside a handler, 1 inside. A nested
 * synchronous fault (fault while handling) trips je_arm_double_fault. Plain byte,
 * single CPU, no atomics (ADR 0002: no exclusives pre-MMU). In .data so it is a
 * defined 0 at boot without depending on the BSS clear order. */
volatile uint8_t je_arm_in_handler __attribute__((section(".data"))) = 0u;

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
    console_write("JeiOS/aarch64: exception handled; halted\r\n");
    for (;;) __asm__ volatile("wfe");
}

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
extern uint8_t _text[], _end[];   /* IMAGE extent [_text, _end), linker.ld */

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
}

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
