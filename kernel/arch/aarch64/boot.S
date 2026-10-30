/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP1 step 1 (ADR 0002): ARM64 entry for the QEMU virt-8.2 / cortex-a72 target.
 * Direct -kernel boot, no firmware. On entry (ADR 0002 D3/D4, validated on QEMU):
 * EL1, MMU off, D-cache off, DAIF masked, x0 = DTB physical address, x1=x2=x3=0.
 *
 * This file does the irreducible arch bring-up and hands to je_arm_main():
 *   - preserve x0 (DTB) across BSS clear by parking it in a callee-saved reg;
 *   - assert we really are at EL1 (halt with a marker path otherwise);
 *   - select SPSel=1 and set SP_EL1 to the top of a reserved in-image stack;
 *   - initialize TCR_EL1 to a complete known interim value with TBI0=0
 *     (ADR 0002 D5 interim: tagged user pointers rejected once the MMU is on;
 *     this only sets the field — behavioral enforcement is the MMU PR);
 *   - write SCTLR_EL1 to a DEFINED value (MMU/caches off, SP-alignment SA=1,
 *     RES1 bits preserved) so SP-alignment faulting is deterministic;
 *   - guard the DTB pointer (nonzero, 8-aligned, conservative extent disjoint
 *     from the image) BEFORE BSS is touched, since BSS holds the copy buffer;
 *   - clear BSS with plain aligned stores (NO DC ZVA: pre-MMU RAM is
 *     Device-nGnRnE, so unaligned / cache-maintenance ops fault);
 *   - stash the DTB pointer into .data (survives, unlike a .bss slot);
 *   - call the C entry, which drives the console + markers.
 *
 * Constraints (ADR 0002 D2/D5, locked by the design pass): no atomics/exclusives
 * and no DC ZVA before the MMU is on; ISB after system-register writes; built
 * -mstrict-align -mgeneral-regs-only so the compiler emits neither unaligned
 * nor FP/SIMD code (a stray SIMD op would trap into absent vectors = silent hang).
 */
.section .text.boot, "ax"
.globl _start
_start:
    /* ---- 64-byte arm64 Linux Image header (Documentation/arm64/booting.rst).
     * QEMU reads this to place and enter the image. ---- */
    b    real_start              /* code0: branch to real entry */
    .long 0                      /* code1 */
    .quad 0x80000                /* text_offset: 0x80000 — matches our link base
                                  * (RAM 0x40000000 + 0x80000 = 0x40080000), so
                                  * the load address equals the link address and
                                  * absolute references (future VBAR/page tables)
                                  * are correct, not luck. */
    .quad __image_size           /* image_size: whole in-memory extent (linker) */
    .quad 0x0000000000000001     /* flags: bits[2:1]=0b01 -> 4 KiB pages, LE */
    .quad 0                      /* res2 */
    .quad 0                      /* res3 */
    .quad 0                      /* res4 */
    .long 0x644d5241             /* magic "ARM\x64" */
    .long 0                      /* res5 */

real_start:
    /* x0 holds the DTB phys addr on entry. Park it in a callee-saved register
     * so the BSS clear below cannot clobber it (x0 is caller-saved and the clear
     * uses low registers). */
    mov     x19, x0

    /* Enforce the ADR 0002 D3 entry assumption by construction: mask all
     * exceptions (DAIF). With no vectors installed yet, an unmasked exception
     * would jump to an UNKNOWN VBAR; masking makes "DAIF masked" true rather
     * than merely assumed. */
    msr     daifset, #0xf

    /* ---- EL check: we require EL1. If not EL1, jump to a bounded halt that
     * still writes a FATAL marker via the DR-only console (safe at any EL on
     * this target). ---- */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #1
    b.ne    el_fatal

    /* ---- Load-address self-check: confirm QEMU placed us where we are linked.
     * adr gives the RUN-time address of _text; compare to the LINK-time address
     * (loaded via a movz/movk immediate, which is absolute). A mismatch means a
     * future absolute reference would be wrong, so halt with a FATAL marker now
     * rather than miscompute a page table later. ---- */
    adr     x0, _text
    movz    x1, #0x4008, lsl #16    /* linked _text = 0x40080000 */
    cmp     x0, x1
    b.ne    load_fatal

    /* ---- Stack: select SP_EL1 and point it at the top of the reserved stack.
     * SPSel=1 makes SP resolve to SP_EL1 at EL1. ---- */
    mov     x0, #1
    msr     SPSel, x0
    isb
    adrp    x0, __stack_top
    add     x0, x0, #:lo12:__stack_top
    mov     sp, x0

    /* ---- TCR_EL1 interim init (ADR 0002 D5): a complete known value with
     * TBI0=0. MMU stays OFF (SCTLR_EL1.M stays 0; SCTLR is written explicitly
     * below). Value 0x80000000: this sets TCR_EL1[31]=TG1[1] (a defined TTBR1
     * granule selector) with TBI0=0 and everything else 0 — a defined interim,
     * NOT a behavioral TBI/translation claim (no TTBR/MAIR, MMU off). The MMU PR
     * (MP2) replaces this with the full MAIR/TCR/TTBR/SCTLR.M config. ---- */
    movz    x0, #0x8000, lsl #16    /* 0x80000000: TBI0=0, interim TG1 bits */
    msr     TCR_EL1, x0
    isb

    /* ---- SCTLR_EL1 explicit init (MP1 review gap): several SCTLR_EL1 bits reset
     * to an architecturally UNKNOWN value, notably SA (bit3, SP-alignment check)
     * and A (bit1, alignment check). With the MMU off we need a DEFINED value so
     * whether a misaligned stp faults is deterministic, not luck. Write a known
     * value: M=0 (MMU off), C=0 (D-cache off), I=0 (I-cache off), A=0 (no strict
     * element alignment — we rely on -mstrict-align + aligned code, and A=1 would
     * also fault legitimate Device accesses), SA=1 (SP must be 16-aligned at EL1),
     * SA0 irrelevant (no EL0 yet). Preserve the architecturally RES1 bits for
     * ARMv8.0 SCTLR_EL1 (bits 11, 20, 22, 23, 28, 29) so we don't write a
     * reserved-0 into a reserved-1 field. Value = RES1 mask | SA.
     *   RES1 bits: (1<<11)|(1<<20)|(1<<22)|(1<<23)|(1<<28)|(1<<29) = 0x30D00800
     *   SA       : (1<<3)                                         = 0x00000008
     *   total                                                     = 0x30D00808 */
    movz    x0, #0x0808
    movk    x0, #0x30D0, lsl #16
    msr     SCTLR_EL1, x0
    isb

    /* ---- DTB pointer guard (issue #52; mp1-dtb debate item 8). MUST run before
     * the BSS clear (and before the JE_ARM_BSS_CHECK pattern fill): BSS now holds
     * the 64 KiB DTB copy buffer, so a firmware DTB lying inside our image extent
     * would be destroyed by the clear before C could copy it. Conservative test
     * on the saved x0 (x19), WITHOUT dereferencing it (we do not read firmware
     * memory here; the header read happens in C once vectors are installed):
     *   - x0 != 0                   (boot protocol: a DTB is always passed);
     *   - x0 8-byte aligned         (arm64 booting.rst requirement);
     *   - x0 <= UINT64_MAX - JE_FDT_MAX_TOTAL: the conservative extent
     *     [x0, x0 + MAX_TOTAL) must not wrap the 64-bit address space. Checked
     *     BEFORE any sum is formed (see "Overflow" below);
     *   - NOT (x0 < IMAGE_END && x0 + JE_FDT_MAX_TOTAL > _text): the largest DTB
     *     the parser will ever accept (totalsize cap, 2 MiB) placed at x0 must be
     *     disjoint from the whole image extent [_text, _end) — _end covers .data,
     *     .bss (incl. the copy buffer) and the boot stack (linker.ld). Using the
     *     cap instead of the real totalsize is deliberately conservative: it
     *     rejects some valid-but-close placements, never accepts an overlap.
     * Overflow (round-3 review P1): x0 + MAX_TOTAL wraps for every
     * x0 > UINT64_MAX - MAX_TOTAL, and a wrapped sum would defeat the "> _text"
     * half of the overlap test. The overlap branch happens to form the sum only
     * when x0 < _end (a small link-time address), but correctness must not hang
     * on that branch order surviving future edits, and an extent wrapping past
     * 2^64 is never a valid DTB placement anyway (C forms ptr + off for offsets
     * up to totalsize). So such x0 is rejected outright, same FATAL marker,
     * before any addition. Bound = UINT64_MAX - 0x200000 = 0xFFFFFFFFFFDFFFFF
     * = ~0x200000, a single MOVN. Test is unsigned "x0 > bound -> fatal" (b.hi):
     * x0 == bound passes this check (its sum is exactly UINT64_MAX, no wrap).
     * With x0 already 8-aligned, the largest x0 reaching the overlap test is
     * 0xFFFFFFFFFFDFFFF8; 0xFFFFFFFFFFE00000 (+8) is rejected here.
     * JE_ARM_DTB_GUARD_SPAN must equal JE_FDT_MAX_TOTAL (fdt.h is C-only, so it
     * is duplicated here; main.c _Static_asserts the C side of that equality).
     * On violation: bounded DR-only FATAL marker + halt (el_fatal pattern). ---- */
#define JE_ARM_DTB_GUARD_SPAN 0x200000   /* == JE_FDT_MAX_TOTAL (2 MiB) */
    cbz     x19, dtb_fatal
    tst     x19, #7
    b.ne    dtb_fatal
    movn    x0, #(JE_ARM_DTB_GUARD_SPAN >> 16), lsl #16  /* UINT64_MAX - SPAN */
    cmp     x19, x0
    b.hi    dtb_fatal                    /* extent would wrap 2^64: reject */
    adrp    x0, _end
    add     x0, x0, #:lo12:_end          /* IMAGE_END (exclusive) */
    cmp     x19, x0
    b.hs    9f                           /* x0 >= IMAGE_END: disjoint */
    adrp    x1, _text
    add     x1, x1, #:lo12:_text
    movz    x2, #(JE_ARM_DTB_GUARD_SPAN >> 16), lsl #16
    add     x2, x19, x2                  /* x0 + MAX_TOTAL (cannot wrap: bounded above) */
    cmp     x2, x1
    b.hi    dtb_fatal                    /* x0 + MAX_TOTAL > _text: overlap */
9:

    /* ---- Clear BSS with plain aligned 16-byte stores. __bss_start/__bss_end
     * are 16-byte aligned by the linker script. No DC ZVA (Device memory). ---- */
    adrp    x0, __bss_start
    add     x0, x0, #:lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, #:lo12:__bss_end
#ifdef JE_ARM_BSS_CHECK
    /* Test-only: BEFORE the real clear, pattern-fill the whole BSS with a
     * nonzero value. QEMU hands us already-zeroed RAM, so without this the
     * "cleared" check is tautological. The production clear loop below must then
     * drive je_arm_bss_probe (a .bss symbol) back to zero for the C check. */
    mov     x4, x0
    movz    x5, #0xa5a5, lsl #48
    movk    x5, #0xa5a5, lsl #32
    movk    x5, #0xa5a5, lsl #16
    movk    x5, #0xa5a5
5:  cmp     x4, x1
    b.hs    6f
    stp     x5, x5, [x4], #16
    b       5b
6:
#endif
1:  cmp     x0, x1
    b.hs    2f
    stp     xzr, xzr, [x0], #16
    b       1b
2:

    /* ---- Stash the DTB pointer into .data (survives; a .bss slot would have
     * just been zeroed above). ---- */
    adrp    x0, je_arm_dtb_phys
    add     x0, x0, #:lo12:je_arm_dtb_phys
    str     x19, [x0]

    /* ---- Enter C. je_arm_main() never returns; it halts via WFE. ---- */
    bl      je_arm_main

    /* Defensive: if C ever returns, halt. */
el_hang:
    wfe
    b       el_hang

    /* ---- Not-EL1 path: write a bounded FATAL marker straight to the PL011 DR
     * (works at any EL on this target, no stack needed) and halt. Kept tiny and
     * self-contained because we cannot trust the normal console setup here. ---- */
el_fatal:
    movz    x1, #0x0900, lsl #16    /* PL011 DR @ 0x09000000 */
    adr     x2, el_fatal_msg
3:  ldrb    w3, [x2], #1
    cbz     w3, 4f
    str     w3, [x1]
    b       3b
4:  wfe
    b       4b
el_fatal_msg:
    .asciz  "JeiOS/aarch64: FATAL entry not at EL1; halted\r\n"

    /* ---- Load-address mismatch: QEMU placed us somewhere other than the link
     * address. Bounded DR-only FATAL marker + halt. ---- */
load_fatal:
    movz    x1, #0x0900, lsl #16
    adr     x2, load_fatal_msg
7:  ldrb    w3, [x2], #1
    cbz     w3, 8f
    str     w3, [x1]
    b       7b
8:  wfe
    b       8b
load_fatal_msg:
    .asciz  "JeiOS/aarch64: FATAL load address != link address; halted\r\n"

    /* ---- DTB pointer invalid (NULL / misaligned) or the conservative DTB
     * extent overlaps the image. Bounded DR-only FATAL marker + halt; reached
     * before the BSS clear, so nothing in BSS may be relied on. ---- */
dtb_fatal:
    movz    x1, #0x0900, lsl #16
    adr     x2, dtb_fatal_msg
10: ldrb    w3, [x2], #1
    cbz     w3, 11f
    str     w3, [x1]
    b       10b
11: wfe
    b       11b
dtb_fatal_msg:
    .asciz  "JeiOS/aarch64: FATAL dtb pointer invalid or overlaps image; halted\r\n"

.section .note.GNU-stack, "", @progbits
