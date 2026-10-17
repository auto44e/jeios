/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3a #5 step (b): the approved user-copy assembly site + its exception-fixup
 * entry (ADR 0007 D9). This is the ONLY place the kernel dereferences a user
 * address, and it does so from an exact, table-registered instruction so the
 * #PF handler can recognize a recoverable copy fault and NOTHING else.
 *
 *   int je_copy_user_run(void *dst, const void *src, uint64_t len)
 *     Copies len bytes dst<-src one byte at a time. Returns 0 on success, 1 if
 *     the #PF handler recovered a not-present fault at the approved site (the
 *     caller treats a nonzero return as a copy failure). Leaf function, no stack
 *     frame, no callee-saved registers touched — so the fault handler may set
 *     RIP = the fixup label and RAX = 1 and simply resume (the normal `ret`
 *     then unwinds to the C caller with no stack adjustment needed).
 *
 * The handler uses the exact RIPs of je_copy_user_fault_rip (the faulting load
 * and store share one instruction window via the movb pair) and je_copy_user_fixup,
 * exported below and registered in the fixup table (kernel/core/usercopy.c).
 * Direction (load vs store) is irrelevant to the site identity; the copy scope
 * records direction and the handler matches the PFEC write bit to it.
 *
 * System V AMD64: RDI=dst, RSI=src, RDX=len, return in EAX. SMAP/SMEP are not
 * enabled in M3a, so this site is trusted to touch only validated user pages;
 * the fixup is the fail-closed net for a not-present page, not a general
 * stray-deref guard (documented limitation).
 */
.section .text,"ax"
.code64
.globl je_copy_user_run
.globl je_copy_user_load_rip
.globl je_copy_user_store_rip
.globl je_copy_user_fixup
je_copy_user_run:
    xorl %eax, %eax                 /* result = 0 (success) */
    testq %rdx, %rdx
    jz je_copy_user_done            /* len == 0: touch nothing */
1:
    /* Two approved faulting instructions: the load from src and the store to
     * dst. Each has an exact RIP registered in the fixup table; a not-present
     * #PF at EITHER is the only recoverable kernel fault, and the handler
     * rewrites RIP to je_copy_user_fixup. The copy scope records direction and
     * the handler matches the PFEC write bit (load=read fault, store=write
     * fault) so a load site can only recover a read fault and vice versa. */
je_copy_user_load_rip:
    movb (%rsi), %cl                /* load from user */
je_copy_user_store_rip:
    movb %cl, (%rdi)                /* store to user */
    incq %rsi
    incq %rdi
    decq %rdx
    jnz 1b
je_copy_user_done:
    ret
je_copy_user_fixup:
    movl $1, %eax                   /* result = 1 (recovered copy fault) */
    ret
.section .note.GNU-stack,"",@progbits
