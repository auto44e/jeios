/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-1b (ADR 0011 D6): the stack-free MMU enable leaf.
 *
 * AAPCS64 arguments:
 *   x0 root_pa, x1 MAIR, x2 TCR,
 *   x3/x4 invalidate range 0 [start,end),
 *   x5/x6 invalidate range 1 [start,end),
 *   x7 SCTLR bits to OR in (M|C|I|WXN).
 *
 * The caller has already constructed all tables and has not returned to C
 * after the first invalidation. This leaf uses only registers: in particular,
 * it neither pushes a frame nor writes memory in the critical window.
 */
.text
.align 2
.global je_arm_mmu_enable_leaf
.type je_arm_mmu_enable_leaf, %function
je_arm_mmu_enable_leaf:
    /* Preserve all arguments in caller-clobbered temporaries before CTR use. */
    mov     x9,  x0
    mov     x10, x1
    mov     x11, x2
    mov     x12, x3
    mov     x13, x4
    mov     x14, x5
    mov     x15, x6
    mov     x16, x7

    /* CTR_EL0.DminLine is log2(words), so the byte stride is 1 << (field+2).
     * Deriving this avoids assuming a 64-byte line on another implementation. */
    mrs     x17, CTR_EL0
    ubfx    x17, x17, #16, #4
    add     x17, x17, #2
    mov     x8, #1
    lsl     x17, x8, x17
    sub     x8, x17, #1

    /* Range 0: tables. DC IVAC is deliberate: tables were written with C=0,
     * so a CLEAN could write a stale dirty line over a correct descriptor. */
    cmp     x12, x13
    b.hs    2f
    bic     x0, x12, x8
1:
    dc      ivac, x0
    add     x0, x0, x17
    cmp     x0, x13
    b.lo    1b
2:
    /* Range 1: the whole image, including .data/.bss, DTB copy and stack.
     * The image superset is intentional; it also covers the boot stack whose
     * lines must not remain stale when C resumes with C=1. */
    cmp     x14, x15
    b.hs    4f
    bic     x0, x14, x8
3:
    dc      ivac, x0
    add     x0, x0, x17
    cmp     x0, x15
    b.lo    3b
4:
    /* Complete all invalidate requests before changing the translation regime. */
    dsb     sy

    /* Install the memory type, walk regime and both translation bases. */
    msr     MAIR_EL1, x10
    msr     TCR_EL1,  x11
    msr     TTBR0_EL1, x9
    msr     TTBR1_EL1, xzr
    /* Make the table-base and regime writes visible before invalidating TLBs. */
    dsb     sy
    isb

    /* Reset TLB contents are implementation-defined; discard all EL1 entries. */
    tlbi    VMALLE1
    dsb     nsh
    isb

    /* One SCTLR write turns on MMU, D-cache, I-cache and WXN together. WXN is
     * installed before any post-enable execution so writable pages cannot
     * become executable by a stale TLB permission. */
    mrs     x0, SCTLR_EL1
    orr     x0, x0, x16
    msr     SCTLR_EL1, x0
    isb

    /* Instructions fetched while MMU/I-cache were off may be stale. */
    ic      iallu
    dsb     nsh
    isb
    ret
.size je_arm_mmu_enable_leaf, .-je_arm_mmu_enable_leaf

.section .note.GNU-stack, "", @progbits
