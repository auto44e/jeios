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
    /* IRQ: preserve x16-x19 before using any scratch register. */
    .balign 0x80
    sub     sp, sp, #JE_TF_SIZE
    stp     x16, x17, [sp, #(JE_TF_GPR + 128)]
    stp     x18, x19, [sp, #(JE_TF_GPR + 144)]
    b       je_arm_irq_common
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

/* ---- Returning EL1h IRQ saver (ADR 0012 D5). The dedicated slot-5 veneer has
 * already reserved the frozen frame and spilled x16-x19. Save every remaining
 * GPR before using scratch registers, then build the rest of the frozen
 * trapframe. ESR/FAR are deliberately zero for an IRQ frame: IRQ classification
 * uses the vector, GIC IAR and ISR_EL1 instead, never stale syndrome registers. */
    .globl  je_arm_irq_common
    .extern je_arm_irq
je_arm_irq_common:
    /* The only supported returning IRQ source is Current EL using SP_ELx
     * (EL1h). Check SPSR_EL1 before the guard is set; any other mode is an
     * unrecoverable entry invariant break and takes the stack-free fatal path. */
    stp     x0,  x1,  [sp, #(JE_TF_GPR + 0)]
    stp     x2,  x3,  [sp, #(JE_TF_GPR + 16)]
    stp     x4,  x5,  [sp, #(JE_TF_GPR + 32)]
    stp     x6,  x7,  [sp, #(JE_TF_GPR + 48)]
    stp     x8,  x9,  [sp, #(JE_TF_GPR + 64)]
    stp     x10, x11, [sp, #(JE_TF_GPR + 80)]
    stp     x12, x13, [sp, #(JE_TF_GPR + 96)]
    stp     x14, x15, [sp, #(JE_TF_GPR + 112)]
    stp     x20, x21, [sp, #(JE_TF_GPR + 160)]
    stp     x22, x23, [sp, #(JE_TF_GPR + 176)]
    stp     x24, x25, [sp, #(JE_TF_GPR + 192)]
    stp     x26, x27, [sp, #(JE_TF_GPR + 208)]
    stp     x28, x29, [sp, #(JE_TF_GPR + 224)]
    str     x30,      [sp, #(JE_TF_GPR + 240)]

    mrs     x0, SPSR_EL1
    and     x1, x0, #0x1f
    cmp     x1, #0x5                    /* EL1h: PSTATE.SP selects SP_EL1 */
    b.ne    je_arm_double_fault
    str     x0, [sp, #JE_TF_SPSR]
    mrs     x1, ELR_EL1
    str     x1, [sp, #JE_TF_ELR]
    add     x1, sp, #JE_TF_SIZE
    str     x1, [sp, #JE_TF_SP]
    mov     x1, #5
    str     x1, [sp, #JE_TF_VECTOR]     /* immediate: x16 is live state */
    stp     xzr, xzr, [sp, #JE_TF_ESR] /* UNKNOWN for IRQ; never classify */
    str     xzr, [sp, #JE_TF_PAD]

    /* Set the transient guard only after all GPR/frame stores. A synchronous
     * fault in je_arm_irq therefore re-enters slot 4 and takes double_fault;
     * a successful return clears it before eret. */
    adrp    x17, je_arm_in_handler
    add     x17, x17, #:lo12:je_arm_in_handler
    ldrb    w18, [x17]
    cbnz    w18, je_arm_double_fault
    mov     w18, #1
    strb    w18, [x17]

    mov     x0, sp
    bl      je_arm_irq

    /* C returned without changing the saved context. Clear the guard before
     * restoring registers; the following eret is the successful return point. */
    adrp    x17, je_arm_in_handler
    add     x17, x17, #:lo12:je_arm_in_handler
    strb    wzr, [x17]

    /* Restore the exception-return state first. ERET consumes these registers
     * and restores PSTATE.SP from SPSR.M; no manual SPSel write is permitted. */
    ldp     x0, x1, [sp, #JE_TF_ELR]
    msr     ELR_EL1, x0
    msr     SPSR_EL1, x1

    /* Restore every GPR from the frozen frame. The original interrupted SP is
     * deliberately NOT loaded from tf->sp: the frame is unwound from the known
     * current SP so a corrupted field cannot redirect the exception stack. */
    ldp     x0,  x1,  [sp, #(JE_TF_GPR + 0)]
    ldp     x2,  x3,  [sp, #(JE_TF_GPR + 16)]
    ldp     x4,  x5,  [sp, #(JE_TF_GPR + 32)]
    ldp     x6,  x7,  [sp, #(JE_TF_GPR + 48)]
    ldp     x8,  x9,  [sp, #(JE_TF_GPR + 64)]
    ldp     x10, x11, [sp, #(JE_TF_GPR + 80)]
    ldp     x12, x13, [sp, #(JE_TF_GPR + 96)]
    ldp     x14, x15, [sp, #(JE_TF_GPR + 112)]
    ldp     x16, x17, [sp, #(JE_TF_GPR + 128)]
    ldp     x18, x19, [sp, #(JE_TF_GPR + 144)]
    ldp     x20, x21, [sp, #(JE_TF_GPR + 160)]
    ldp     x22, x23, [sp, #(JE_TF_GPR + 176)]
    ldp     x24, x25, [sp, #(JE_TF_GPR + 192)]
    ldp     x26, x27, [sp, #(JE_TF_GPR + 208)]
    ldp     x28, x29, [sp, #(JE_TF_GPR + 224)]
    ldr     x30,      [sp, #(JE_TF_GPR + 240)]
    add     sp, sp, #JE_TF_SIZE
    eret

#ifdef JE_ARM_TIMER_TEST
/* ---- Returning IRQ restore proof (ADR 0012 D8). This loop owns x0-x30 while
 * I is unmasked, so C cannot accidentally hide an x16-x18 restore defect.
 * The C side initializes the two counters and prints the result after this
 * bounded loop returns. A mismatch masks I, stores its register number, and
 * returns to C for the complete-line diagnostic. */
    .globl  je_arm_canary_loop
    .extern je_arm_canary_heartbeat
    .extern je_arm_irq_tick_count
    .extern je_arm_canary_fail_reg

/* (b) extra live ticks the rotating-scratch x29/x30 phase waits for. */
#define JE_ARM_CANARY_HI_TICKS 3

.macro CANARY_CHECK reg, num
    adrp    x30, je_arm_canary_patterns
    add     x30, x30, #:lo12:je_arm_canary_patterns
    ldr     x30, [x30, #(\num * 8)]
    cmp     x\reg, x30
    b.ne    .Lcanary_fail_\num
.endm

je_arm_canary_loop:
    /* Keep the caller's callee-saved registers and return address outside the
     * canary set. The remaining four words are scratch for the heartbeat and
     * the x30 comparison. */
    sub     sp, sp, #128
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]

    /* Each pattern is 0xc0de000000000000 + register number. */
    movz    x0,  #0xc0de, lsl #48
    add     x0,  x0,  #0
    movz    x1,  #0xc0de, lsl #48
    add     x1,  x1,  #1
    movz    x2,  #0xc0de, lsl #48
    add     x2,  x2,  #2
    movz    x3,  #0xc0de, lsl #48
    add     x3,  x3,  #3
    movz    x4,  #0xc0de, lsl #48
    add     x4,  x4,  #4
    movz    x5,  #0xc0de, lsl #48
    add     x5,  x5,  #5
    movz    x6,  #0xc0de, lsl #48
    add     x6,  x6,  #6
    movz    x7,  #0xc0de, lsl #48
    add     x7,  x7,  #7
    movz    x8,  #0xc0de, lsl #48
    add     x8,  x8,  #8
    movz    x9,  #0xc0de, lsl #48
    add     x9,  x9,  #9
    movz    x10, #0xc0de, lsl #48
    add     x10, x10, #10
    movz    x11, #0xc0de, lsl #48
    add     x11, x11, #11
    movz    x12, #0xc0de, lsl #48
    add     x12, x12, #12
    movz    x13, #0xc0de, lsl #48
    add     x13, x13, #13
    movz    x14, #0xc0de, lsl #48
    add     x14, x14, #14
    movz    x15, #0xc0de, lsl #48
    add     x15, x15, #15
    movz    x16, #0xc0de, lsl #48
    add     x16, x16, #16
    movz    x17, #0xc0de, lsl #48
    add     x17, x17, #17
    movz    x18, #0xc0de, lsl #48
    add     x18, x18, #18
    movz    x19, #0xc0de, lsl #48
    add     x19, x19, #19
    movz    x20, #0xc0de, lsl #48
    add     x20, x20, #20
    movz    x21, #0xc0de, lsl #48
    add     x21, x21, #21
    movz    x22, #0xc0de, lsl #48
    add     x22, x22, #22
    movz    x23, #0xc0de, lsl #48
    add     x23, x23, #23
    movz    x24, #0xc0de, lsl #48
    add     x24, x24, #24
    movz    x25, #0xc0de, lsl #48
    add     x25, x25, #25
    movz    x26, #0xc0de, lsl #48
    add     x26, x26, #26
    movz    x27, #0xc0de, lsl #48
    add     x27, x27, #27
    movz    x28, #0xc0de, lsl #48
    add     x28, x28, #28
    movz    x29, #0xc0de, lsl #48
    add     x29, x29, #29
    movz    x30, #0xc0de, lsl #48
    add     x30, x30, #30

    /* The proof's only unmask: I clear, while A/F/D remain masked. */
    msr     daifclr, #2

.Lcanary_loop:
    /* Increment heartbeat without disturbing the canary values. */
    stp     x29, x30, [sp, #104]
    adrp    x29, je_arm_canary_heartbeat
    add     x29, x29, #:lo12:je_arm_canary_heartbeat
    ldr     x30, [x29]
    add     x30, x30, #1
    str     x30, [x29]
    ldp     x29, x30, [sp, #104]

    /* x30 is a temporary for x0-x29; save its canary value first. */
    str     x30, [sp, #96]
    CANARY_CHECK 0, 0
    CANARY_CHECK 1, 1
    CANARY_CHECK 2, 2
    CANARY_CHECK 3, 3
    CANARY_CHECK 4, 4
    CANARY_CHECK 5, 5
    CANARY_CHECK 6, 6
    CANARY_CHECK 7, 7
    CANARY_CHECK 8, 8
    CANARY_CHECK 9, 9
    CANARY_CHECK 10, 10
    CANARY_CHECK 11, 11
    CANARY_CHECK 12, 12
    CANARY_CHECK 13, 13
    CANARY_CHECK 14, 14
    CANARY_CHECK 15, 15
    CANARY_CHECK 16, 16
    CANARY_CHECK 17, 17
    CANARY_CHECK 18, 18
    CANARY_CHECK 19, 19
    CANARY_CHECK 20, 20
    CANARY_CHECK 21, 21
    CANARY_CHECK 22, 22
    CANARY_CHECK 23, 23
    CANARY_CHECK 24, 24
    CANARY_CHECK 25, 25
    CANARY_CHECK 26, 26
    CANARY_CHECK 27, 27
    CANARY_CHECK 28, 28
    CANARY_CHECK 29, 29
    ldr     x30, [sp, #96]
    adrp    x29, je_arm_canary_patterns
    add     x29, x29, #:lo12:je_arm_canary_patterns
    ldr     x29, [x29, #(30 * 8)]
    cmp     x30, x29
    b.ne    .Lcanary_fail_30
    /* Restore x29's canary after using it as the x30 comparison temporary. */
    adrp    x29, je_arm_canary_patterns
    add     x29, x29, #:lo12:je_arm_canary_patterns
    ldr     x29, [x29, #(29 * 8)]

    /* Stop after four successful timer ticks (N >= 3); x29/x30 are scratch
     * only while the already-checked canary values are saved on the stack. */
    stp     x29, x30, [sp, #104]
    adrp    x29, je_arm_irq_tick_count
    add     x29, x29, #:lo12:je_arm_irq_tick_count
    ldr     x30, [x29]
    cmp     x30, #4
    b.hs    .Lcanary_done
    ldp     x29, x30, [sp, #104]
    b       .Lcanary_loop

.Lcanary_done:
    /* Mask I FIRST so no further IRQ can land during the final verification,
     * then run ONE complete check of x0..x28 with interrupts masked. This closes
     * the cross-review gap that the per-iteration checks ran BEFORE the
     * tick-count test, so a corruption on the Nth (last) return was never
     * re-checked; with I masked here that final pass is race-free.
     *
     * HONEST SCOPE (cross-review finding): x29 and x30 are the loop's own
     * working registers — x30 is the scratch/link register and x29 addresses the
     * heartbeat/pattern table — so they are saved to and reloaded from the stack
     * across every check and heartbeat step. Their LIVE post-IRQ value is
     * therefore always overwritten by a saved-good copy before any compare, and
     * a handler that corrupts x29/x30 on return CANNOT be caught by this loop (a
     * check that reloads the saved value would be a FALSE proof). The canary set
     * is x0..x28 — which includes the frozen-MP1 hazard registers x16, x17, x18
     * that this proof exists to cover. x29/x30 survival on the IRQ return is
     * exercised implicitly (the loop keeps running and the heartbeat advances),
     * not asserted register-wise. Recorded in HANDOFF / the PR body. */
    msr     daifset, #2
    ldp     x29, x30, [sp, #104]
    str     x30, [sp, #96]
    CANARY_CHECK 0, 0
    CANARY_CHECK 1, 1
    CANARY_CHECK 2, 2
    CANARY_CHECK 3, 3
    CANARY_CHECK 4, 4
    CANARY_CHECK 5, 5
    CANARY_CHECK 6, 6
    CANARY_CHECK 7, 7
    CANARY_CHECK 8, 8
    CANARY_CHECK 9, 9
    CANARY_CHECK 10, 10
    CANARY_CHECK 11, 11
    CANARY_CHECK 12, 12
    CANARY_CHECK 13, 13
    CANARY_CHECK 14, 14
    CANARY_CHECK 15, 15
    CANARY_CHECK 16, 16
    CANARY_CHECK 17, 17
    CANARY_CHECK 18, 18
    CANARY_CHECK 19, 19
    CANARY_CHECK 20, 20
    CANARY_CHECK 21, 21
    CANARY_CHECK 22, 22
    CANARY_CHECK 23, 23
    CANARY_CHECK 24, 24
    CANARY_CHECK 25, 25
    CANARY_CHECK 26, 26
    CANARY_CHECK 27, 27
    CANARY_CHECK 28, 28
    ldr     x30, [sp, #96]
    b       .Lcanary_restore

.Lcanary_fail_0:  mov w0, #0;  b .Lcanary_fail
.Lcanary_fail_1:  mov w0, #1;  b .Lcanary_fail
.Lcanary_fail_2:  mov w0, #2;  b .Lcanary_fail
.Lcanary_fail_3:  mov w0, #3;  b .Lcanary_fail
.Lcanary_fail_4:  mov w0, #4;  b .Lcanary_fail
.Lcanary_fail_5:  mov w0, #5;  b .Lcanary_fail
.Lcanary_fail_6:  mov w0, #6;  b .Lcanary_fail
.Lcanary_fail_7:  mov w0, #7;  b .Lcanary_fail
.Lcanary_fail_8:  mov w0, #8;  b .Lcanary_fail
.Lcanary_fail_9:  mov w0, #9;  b .Lcanary_fail
.Lcanary_fail_10: mov w0, #10; b .Lcanary_fail
.Lcanary_fail_11: mov w0, #11; b .Lcanary_fail
.Lcanary_fail_12: mov w0, #12; b .Lcanary_fail
.Lcanary_fail_13: mov w0, #13; b .Lcanary_fail
.Lcanary_fail_14: mov w0, #14; b .Lcanary_fail
.Lcanary_fail_15: mov w0, #15; b .Lcanary_fail
.Lcanary_fail_16: mov w0, #16; b .Lcanary_fail
.Lcanary_fail_17: mov w0, #17; b .Lcanary_fail
.Lcanary_fail_18: mov w0, #18; b .Lcanary_fail
.Lcanary_fail_19: mov w0, #19; b .Lcanary_fail
.Lcanary_fail_20: mov w0, #20; b .Lcanary_fail
.Lcanary_fail_21: mov w0, #21; b .Lcanary_fail
.Lcanary_fail_22: mov w0, #22; b .Lcanary_fail
.Lcanary_fail_23: mov w0, #23; b .Lcanary_fail
.Lcanary_fail_24: mov w0, #24; b .Lcanary_fail
.Lcanary_fail_25: mov w0, #25; b .Lcanary_fail
.Lcanary_fail_26: mov w0, #26; b .Lcanary_fail
.Lcanary_fail_27: mov w0, #27; b .Lcanary_fail
.Lcanary_fail_28: mov w0, #28; b .Lcanary_fail
.Lcanary_fail_29: mov w0, #29; b .Lcanary_fail
.Lcanary_fail_30: mov w0, #30; b .Lcanary_fail

.Lcanary_fail:
    adrp    x29, je_arm_canary_fail_reg
    add     x29, x29, #:lo12:je_arm_canary_fail_reg
    str     x0, [x29]
    msr     daifset, #2

.Lcanary_restore:
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #128
    ret

/* ---- (b) Rotating-scratch canary for x29 and x30 (ADR 0012 D8, owner a+b).
 * The primary loop above cannot check x29/x30 (they are ITS scratch/LR), so a
 * SECOND phase checks exactly those two, using x0-x2 — which the primary loop
 * already proved survive an IRQ return — as the scratch/address temporaries.
 * It seeds x29,x30 with their canary patterns, unmasks I, and spins until the
 * tick count advances by JE_ARM_CANARY_HI_TICKS more ticks, checking x29,x30
 * every iteration; a mismatch stores the reg number and halts the proof. A
 * final I-masked re-check closes the last-tick race. On entry the primary loop
 * has returned, so x0-x28 are caller-restored; we only clobber x0-x2 (scratch),
 * x29, x30 here and restore the frame's x29/x30 (LR) at the end. ---- */
    .globl  je_arm_canary_hi_loop
je_arm_canary_hi_loop:
    sub     sp, sp, #32
    stp     x29, x30, [sp, #0]          /* save real FP/LR to restore + return */
    str     x19, [sp, #16]              /* x19 scratch for the deadline base */

    /* Record the starting tick count; we require +JE_ARM_CANARY_HI_TICKS. */
    adrp    x0, je_arm_irq_tick_count
    add     x0, x0, #:lo12:je_arm_irq_tick_count
    ldr     x19, [x0]

    /* Seed x29,x30 with their canary patterns (indices 29,30). */
    adrp    x0, je_arm_canary_patterns
    add     x0, x0, #:lo12:je_arm_canary_patterns
    ldr     x29, [x0, #(29 * 8)]
    ldr     x30, [x0, #(30 * 8)]

    msr     daifclr, #2                 /* unmask IRQ (I only) */
.Lcanary_hi_loop:
    /* Check x29 against pattern[29] using x0,x1 as scratch. */
    adrp    x0, je_arm_canary_patterns
    add     x0, x0, #:lo12:je_arm_canary_patterns
    ldr     x1, [x0, #(29 * 8)]
    cmp     x29, x1
    b.ne    .Lcanary_hi_fail_29
    ldr     x1, [x0, #(30 * 8)]
    cmp     x30, x1
    b.ne    .Lcanary_hi_fail_30
    /* Bump the heartbeat so the --timer-test oracle sees progress here too. */
    adrp    x0, je_arm_canary_heartbeat
    add     x0, x0, #:lo12:je_arm_canary_heartbeat
    ldr     x1, [x0]
    add     x1, x1, #1
    str     x1, [x0]
    /* Exit after +JE_ARM_CANARY_HI_TICKS ticks. */
    adrp    x0, je_arm_irq_tick_count
    add     x0, x0, #:lo12:je_arm_irq_tick_count
    ldr     x1, [x0]
    sub     x1, x1, x19
    cmp     x1, #JE_ARM_CANARY_HI_TICKS
    b.lo    .Lcanary_hi_loop

    msr     daifset, #2                 /* mask I: final race-free re-check */
    adrp    x0, je_arm_canary_patterns
    add     x0, x0, #:lo12:je_arm_canary_patterns
    ldr     x1, [x0, #(29 * 8)]
    cmp     x29, x1
    b.ne    .Lcanary_hi_fail_29
    ldr     x1, [x0, #(30 * 8)]
    cmp     x30, x1
    b.ne    .Lcanary_hi_fail_30
    mov     w0, #0                       /* 0 = OK */
    b       .Lcanary_hi_done

.Lcanary_hi_fail_29:
    msr     daifset, #2
    mov     w0, #29
    b       .Lcanary_hi_store
.Lcanary_hi_fail_30:
    msr     daifset, #2
    mov     w0, #30
.Lcanary_hi_store:
    adrp    x1, je_arm_canary_fail_reg
    add     x1, x1, #:lo12:je_arm_canary_fail_reg
    str     x0, [x1]
.Lcanary_hi_done:
    ldp     x29, x30, [sp, #0]
    ldr     x19, [sp, #16]
    add     sp, sp, #32
    ret


    .section .rodata, "a"
je_arm_canary_patterns:
    .xword  0xc0de000000000000, 0xc0de000000000001
    .xword  0xc0de000000000002, 0xc0de000000000003
    .xword  0xc0de000000000004, 0xc0de000000000005
    .xword  0xc0de000000000006, 0xc0de000000000007
    .xword  0xc0de000000000008, 0xc0de000000000009
    .xword  0xc0de00000000000a, 0xc0de00000000000b
    .xword  0xc0de00000000000c, 0xc0de00000000000d
    .xword  0xc0de00000000000e, 0xc0de00000000000f
    .xword  0xc0de000000000010, 0xc0de000000000011
    .xword  0xc0de000000000012, 0xc0de000000000013
    .xword  0xc0de000000000014, 0xc0de000000000015
    .xword  0xc0de000000000016, 0xc0de000000000017
    .xword  0xc0de000000000018, 0xc0de000000000019
    .xword  0xc0de00000000001a, 0xc0de00000000001b
    .xword  0xc0de00000000001c, 0xc0de00000000001d
    .xword  0xc0de00000000001e
#endif

.section .note.GNU-stack, "", @progbits
