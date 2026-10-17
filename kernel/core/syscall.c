/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3a #5 step (a): the experimental v0 syscall dispatcher reached from the DPL3
 * int 0x80 gate (ADR 0007 D8/D10). Interrupts are disabled on this short path
 * (the gate is an interrupt gate). Bounded and nonblocking.
 *
 * Split in two: je_syscall_decode() is a PURE function (op + six args ->
 * action/status/result) with no kernel state, so it is unit-tested on the host;
 * je_syscall_dispatch() is the thin kernel glue that reads the trap frame, calls
 * decode, writes the result registers, and routes to the scheduler (resume the
 * caller, yield, or exit). No user pointers are touched in step (a) — copy ops
 * are step (b).
 */
#include <jeios/abi.h>
#include <jeios/syscall.h>
#include <jeios/sched.h>
#include <jeios/trap.h>

/* Pure decode: given the operation and its six argument registers, decide the
 * action, the ABI status to return in RAX, and any scalar result for RDX. No
 * side effects; safe to unit-test. Reserved-argument rules are per-op and are
 * checked only for recognized ops (an unknown op is UNSUPPORTED regardless of
 * arguments). exit(code) carries the code in *out_result for the glue to use. */
je_syscall_action je_syscall_decode(uint64_t op, const uint64_t args[6],
                                    je_abi_status *out_status, uint64_t *out_result)
{
    uint64_t rdi=args[0], rsi=args[1], rdx=args[2], r10=args[3], r8=args[4], r9=args[5];
    *out_result = 0;
    switch (op) {
    case JE_SYS_ABI_QUERY:
        /* which = RDI (0 version, 1 profile); RSI..R9 reserved must be zero. */
        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi==JE_ABI_QUERY_VERSION) { *out_result=JE_ABI_VERSION; *out_status=JE_ABI_OK; return JE_SYSCALL_RETURN; }
        if (rdi==JE_ABI_QUERY_PROFILE) { *out_result=JE_ABI_PROFILE; *out_status=JE_ABI_OK; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN;
    case JE_SYS_YIELD:
        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_YIELD;
    case JE_SYS_EXIT:
        /* code = RDI; RSI..R9 reserved must be zero. */
        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_result=rdi; *out_status=JE_ABI_OK; return JE_SYSCALL_EXIT;
    default:
        /* Unknown op: UNSUPPORTED regardless of arguments (full 64-bit op). */
        *out_status=JE_ABI_UNSUPPORTED; return JE_SYSCALL_RETURN;
    }
}

#ifndef JE_SYSCALL_HOSTED_TEST
/* Kernel glue. Returns the frame to iretq to (the caller for RETURN/invalid, a
 * live successor for YIELD/EXIT). Must only be called for a user-origin int 0x80
 * frame (je_trap_dispatch enforces cs&3==3; the scheduler helpers re-check the
 * frame identity). */
je_trap_frame *je_syscall_dispatch(je_trap_frame *f)
{
    uint64_t args[6] = { f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9 };
    je_abi_status status; uint64_t result;
    je_syscall_action action = je_syscall_decode(f->rax, args, &status, &result);

    switch (action) {
    case JE_SYSCALL_YIELD:
        /* Write the OK result into the CALLER's saved frame BEFORE switching, so
         * when this task is next resumed its RAX already reads OK. Reuse the
         * existing kind-aware reschedule path (same as the 0x81 kernel yield);
         * no recursion, no M2 sleep (D10). */
        f->rax = (uint64_t)status; f->rdx = result;
        return je_sched_interrupt(f, false);
    case JE_SYSCALL_EXIT:
        return je_sched_exit_current(f, result);
    case JE_SYSCALL_RETURN:
    default:
        f->rax = (uint64_t)status; f->rdx = result;
        /* Validate+sanitize the frame on this user return (ADR: every return). */
        return je_sched_return_to_user(f);
    }
}
#endif
