/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP3b AArch64 user-copy leaf sites (ADR 0016 D1).
 *
 * The two directions are intentionally separate.  LDTRB/STTRB apply EL0
 * permission checks to the user-side access, while STRB/LDRB access the
 * kernel-side buffer as EL1.  Each routine is a leaf with no stack frame and
 * touches only x0-x3 and x30; x16-x18 are reserved by the exception veneer.
 */
.section .text, "ax"

.globl je_copy_from_user_run
.type je_copy_from_user_run, %function
je_copy_from_user_run:
    cbz     x2, .Lfrom_zero
.Lfrom_loop:
    /* Exact recoverable user-read site. */
.globl je_copy_user_load_site
je_copy_user_load_site:
    ldtrb   w3, [x1]
    strb    w3, [x0]
    add     x1, x1, #1
    add     x0, x0, #1
    subs    x2, x2, #1
    b.ne    .Lfrom_loop
.Lfrom_zero:
    mov     w0, wzr
    ret
.size je_copy_from_user_run, .-je_copy_from_user_run

.globl je_copy_to_user_run
.type je_copy_to_user_run, %function
je_copy_to_user_run:
    cbz     x2, .Lto_zero
.Lto_loop:
    ldrb    w3, [x1]
    /* Exact recoverable user-write site. */
.globl je_copy_user_store_site
je_copy_user_store_site:
    sttrb   w3, [x0]
    add     x1, x1, #1
    add     x0, x0, #1
    subs    x2, x2, #1
    b.ne    .Lto_loop
.Lto_zero:
    mov     w0, wzr
    ret
.size je_copy_to_user_run, .-je_copy_to_user_run

.globl je_copy_user_fixup
.type je_copy_user_fixup, %function
je_copy_user_fixup:
    mov     w0, #1
    ret
.size je_copy_user_fixup, .-je_copy_user_fixup

#ifdef JE_ARM_EL0_TEST
/*
 * Test-only stray kernel LDR.  The caller holds an active user-copy scope, but
 * this instruction is not one of the two approved recovery labels.  A fault
 * here must therefore be stopped by the stack-free ELR prefilter.
 */
.globl je_arm_copy_stray_load
.type je_arm_copy_stray_load, %function
je_arm_copy_stray_load:
    ldrb    w3, [x1]
    mov     w0, wzr
    ret
.size je_arm_copy_stray_load, .-je_arm_copy_stray_load
#endif

.section .note.GNU-stack, "", %progbits
