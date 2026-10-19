/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP1 (ADR 0002 D5-3 + ARM64.md MP1): the AArch64 EL1 exception vector table and
 * the common trap-frame saver. Design frozen by the owner-approved vectors pass.
 *
 * VBAR_EL1 points here. The table is 16 entries of 0x80 (128) bytes each, 4
 * groups of 4 — {Cur EL SP_EL0, Cur EL SP_ELx, Lower EL AArch64, Lower EL
 * AArch32} x {Sync, IRQ, FIQ, SError} — and MUST be 2 KiB aligned (VBAR_EL1[10:0]
 * are RES0). We install ALL 16 even though MP1 only legitimately reaches "Current
 * EL with SP_ELx, Synchronous" (no EL0 yet, IRQ/FIQ masked via DAIF, no lower EL,
 * SError possible from a bad access): an unexpected vector must still land on a
 * defined bounded halt, never execute garbage.
 *
 * Each 0x80 slot is a tiny INDEX-BEARING stub (`mov w16,#idx; b je_arm_vec_common`)
 * so the common saver knows which of the 16 fired — a plain `b` would lose it.
 * The saver builds a je_arm_trapframe (trapframe.h, FROZEN offsets) on the
 * current EL1 stack and calls je_arm_exception(tf).
 *
 * MP1 reentrancy contract (ARM64.md "declare reentrancy assumptions"): single
 * kernel stack, handlers are NON-reentrant and NON-returning. DAIF stays masked
 * throughout (exception entry masks it; we never unmask), so no async exception
 * nests. A NESTED SYNCHRONOUS fault (a fault while already in a handler) is a
 * FATAL double-fault -> bounded DR-only halt via je_arm_double_fault; the frame
 * of the SECOND fault is only partially trustworthy, documented, not recovered.
 * No allocating fault handler. MMU is off in MP1 so table VA==PA (flat); the
 * table is static image content (no I-cache maintenance needed; the ISB after
 * the VBAR write in C is sufficient).
 *
 * Built -mstrict-align -mgeneral-regs-only: the saver uses only GPR stp/ldr and
 * aligned SP, no FP/SIMD, no unaligned, no atomics/exclusives.
 */
#include "trapframe.h"

/* One vector slot: stamp the index into w16 and branch to the common saver.
 * `.balign 0x80` pads each entry to the architectural 128-byte stride. */
.macro VEC idx
    .balign 0x80
    mov     w16, #\idx
    b       je_arm_vec_common
.endm

    .section .text.vectors, "ax"
    .balign 0x800                 /* VBAR_EL1 2 KiB alignment (bits[10:0] RES0) */
    .globl  je_arm_vectors
je_arm_vectors:
    /* Group 0: Current EL with SP_EL0 (not used at EL1h, but defined). */
    VEC 0    /* Sync   */
    VEC 1    /* IRQ    */
    VEC 2    /* FIQ    */
    VEC 3    /* SError */
    /* Group 1: Current EL with SP_ELx  (EL1h — the reachable group in MP1). */
    VEC 4    /* Sync   <- BRK / undefined / data abort land here */
    VEC 5    /* IRQ    (masked in MP1) */
    VEC 6    /* FIQ    (masked in MP1) */
    VEC 7    /* SError */
    /* Group 2: Lower EL using AArch64 (no EL0 yet in MP1). */
    VEC 8    /* Sync   */
    VEC 9    /* IRQ    */
    VEC 10   /* FIQ    */
    VEC 11   /* SError */
    /* Group 3: Lower EL using AArch32 (unsupported; defined halt). */
    VEC 12   /* Sync   */
    VEC 13   /* IRQ    */
    VEC 14   /* FIQ    */
    VEC 15   /* SError */

    .section .text, "ax"
    .globl  je_arm_vec_common
    .extern je_arm_exception
    .extern je_arm_double_fault
    .extern je_arm_in_handler
je_arm_vec_common:
    /* ---- Reentrancy guard FIRST, before any stack access that could itself
     * fault: if je_arm_in_handler != 0 we are already handling an exception ->
     * nested/double fault -> fatal bounded halt. Set it to 1 otherwise. This is
     * a plain byte load/store (single CPU, MMU-off Normal access, no atomics). */
    adrp    x17, je_arm_in_handler
    add     x17, x17, #:lo12:je_arm_in_handler
    ldrb    w18, [x17]
    cbnz    w18, je_arm_double_fault
    mov     w18, #1
    strb    w18, [x17]

    /* ---- Build the trap frame on the current (EL1) stack. Reserve JE_TF_SIZE
     * (304, 16-aligned) and fill it to the trapframe.h layout. ---- */
    sub     sp, sp, #JE_TF_SIZE
    /* x0..x30 -> gpr[0..30] (offset 0). stp in pairs; x30 alone last. */
    stp     x0,  x1,  [sp, #(JE_TF_GPR + 0)]
    stp     x2,  x3,  [sp, #(JE_TF_GPR + 16)]
    stp     x4,  x5,  [sp, #(JE_TF_GPR + 32)]
    stp     x6,  x7,  [sp, #(JE_TF_GPR + 48)]
    stp     x8,  x9,  [sp, #(JE_TF_GPR + 64)]
    stp     x10, x11, [sp, #(JE_TF_GPR + 80)]
    stp     x12, x13, [sp, #(JE_TF_GPR + 96)]
    stp     x14, x15, [sp, #(JE_TF_GPR + 112)]
    stp     x16, x17, [sp, #(JE_TF_GPR + 128)]
    stp     x18, x19, [sp, #(JE_TF_GPR + 144)]
    stp     x20, x21, [sp, #(JE_TF_GPR + 160)]
    stp     x22, x23, [sp, #(JE_TF_GPR + 176)]
    stp     x24, x25, [sp, #(JE_TF_GPR + 192)]
    stp     x26, x27, [sp, #(JE_TF_GPR + 208)]
    stp     x28, x29, [sp, #(JE_TF_GPR + 224)]
    str     x30,      [sp, #(JE_TF_GPR + 240)]

    /* Faulting SP = the value BEFORE we subtracted the frame. NOTE: this is the
     * correct interrupted SP ONLY for the "Current EL with SP_ELx" group (slots
     * 4-7), which is the only group reachable in MP1 (EL1h, no EL0, so no lower-EL
     * groups; and we run on SP_ELx not SP_EL0). For the unreachable groups the
     * interrupted SP would be SP_EL0 and this field would be wrong — those slots
     * exist only as defined bounded halts, not as correct savers. */
    add     x0, sp, #JE_TF_SIZE
    str     x0, [sp, #JE_TF_SP]

    /* D5-3 system registers: ELR, SPSR, ESR, FAR. */
    mrs     x0, ELR_EL1
    mrs     x1, SPSR_EL1
    stp     x0, x1, [sp, #JE_TF_ELR]      /* ELR @256, SPSR @264 (aligned pair) */
    mrs     x0, ESR_EL1
    mrs     x1, FAR_EL1
    stp     x0, x1, [sp, #JE_TF_ESR]      /* ESR @272, FAR @280 (aligned pair) */

    /* Vector index: the slot stub put it in w16, and we saved x16 to gpr[16]
     * above BEFORE anything clobbered x16, so gpr[16]'s low word is the index.
     * (Honest caveat: gpr[17]/gpr[18] hold this stub's guard scratch, NOT the
     * pre-exception x17/x18 — x16-x18 are IP0/IP1/scratch, conventionally
     * clobberable by an exception veneer; irrelevant to the MP1 diagnostic.) */
    ldr     x0, [sp, #(JE_TF_GPR + 128)]  /* saved x16 = {idx in low 32} */
    and     x0, x0, #0xffffffff
    str     x0, [sp, #JE_TF_VECTOR]
    str     xzr, [sp, #JE_TF_PAD]

    /* Call the C handler with a pointer to the frame. It does not return. */
    mov     x0, sp
    bl      je_arm_exception
    /* je_arm_exception is _Noreturn; if it ever returns, halt hard. */
1:  wfe
    b       1b

/* ---- Double/nested fault (je_arm_in_handler was already set). Pure asm so it
 * is guaranteed STACK-FREE: a C handler could gain a stack-touching prologue and
 * then fault again on the already-corrupt/misaligned SP that may have caused the
 * nested fault, looping with no output. Uses only x0/x1 (caller regs; we halt
 * anyway), polls PL011 TXFF so it works on real hardware (not just QEMU), and
 * writes a bounded fixed marker. No stack, no sysreg frame, no re-entry. ---- */
    .globl  je_arm_double_fault
je_arm_double_fault:
    movz    x0, #0x0900, lsl #16        /* PL011 base @ 0x09000000 */
    adr     x1, je_arm_df_msg
2:  ldrb    w2, [x1], #1
    cbz     w2, 4f
    mov     w4, #0x10000               /* bounded TXFF wait (never block forever) */
3:  ldr     w3, [x0, #0x18]             /* FR */
    tbz     w3, #5, 5f                  /* TXFF clear -> send */
    subs    w4, w4, #1
    b.ne    3b
5:  str     w2, [x0]                    /* DR (after TXFF clear, or bound hit) */
    b       2b
4:  wfe
    b       4b
je_arm_df_msg:
    .asciz  "JeiOS/aarch64: FATAL double fault; halted\r\n"

.section .note.GNU-stack, "", @progbits
