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
#include <jeios/usercopy.h>
#ifndef JE_SYSCALL_HOSTED_TEST
#include <jeios/vm.h>
#include <jeios/handle.h>
#endif

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
    case JE_SYS_COPY_PROBE:
        /* dev-only copy_probe(src=RDI, dst=RSI, len=RDX); R10/R8/R9 reserved=0.
         * The actual copy (with prevalidation + fixup) is performed by the glue;
         * the pure decoder only rejects nonzero reserved args here. */
        if (r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_COPY_PROBE;
    case JE_SYS_HANDLE_SELF:
        /* handle_self(): no arguments; RDI..R9 reserved must be zero. The glue
         * mints a SELF handle in the current process's table. */
        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_SELF;
    case JE_SYS_HANDLE_CLOSE:
        /* handle_close(h=RDI): RSI..R9 reserved must be zero. A handle value with
         * bits 63:32 set is rejected here as INVALID before it is ever narrowed
         * to the 32-bit je_handle (decode-INVALID precedes BADINDEX/STALE). */
        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_CLOSE;
    case JE_SYS_HANDLE_QUERY:
        /* handle_query(h=RDI): RSI..R9 reserved must be zero; same wide-handle
         * rejection as close. The glue returns the object type in RDX. */
        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_QUERY;
#ifdef JE_DEV_COPY_INJECT
    case JE_SYS_COPY_FIXUP_INJECT:
        /* dev-only: uaddr=RDI (an unmapped arena page); glue raw-copies to prove
         * the fixup recovers. Guarded out of the default build entirely. */
        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_COPY_FIXUP_INJECT;
#endif
    default:
        /* Unknown op: UNSUPPORTED regardless of arguments (full 64-bit op). */
        *out_status=JE_ABI_UNSUPPORTED; return JE_SYSCALL_RETURN;
    }
}

#ifndef JE_SYSCALL_HOSTED_TEST
/* M3b: SELF is the only object kind so far — a kernel-minted, immortal,
 * resource-free process-identity capability (no dereferenceable pointer). */
#define JE_HANDLE_TYPE_SELF JE_ABI_OBJ_SELF

/* Exhaustive, single-point mapping of internal JE_HANDLE_* results to the v0
 * ABI status namespace (ADR 0008 D2: distinct, not collapsed). Keeping it here
 * in the glue is what lets abi.h stay independent of handle.h. */
static je_abi_status handle_status_to_abi(je_handle_status s)
{
    switch (s) {
    case JE_HANDLE_OK:        return JE_ABI_OK;
    case JE_HANDLE_STALE:     return JE_ABI_H_STALE;
    case JE_HANDLE_BADINDEX:  return JE_ABI_H_BADINDEX;
    case JE_HANDLE_WRONGTYPE: return JE_ABI_H_WRONGTYPE;
    case JE_HANDLE_DENIED:    return JE_ABI_H_DENIED;
    case JE_HANDLE_QUOTA:     return JE_ABI_H_QUOTA;
    case JE_HANDLE_NOSLOTS:   return JE_ABI_H_NOSLOTS;
    }
    return JE_ABI_INVALID;   /* unreachable; fail closed if the enum ever grows */
}

/* Resolve the CURRENT user process's handle table. Identity comes ONLY from the
 * scheduler (je_sched_current_user), never from a syscall argument. Returns NULL
 * if the caller is not a user thread (the glue maps that to JE_ABI_INVALID). */
static je_handle_table *current_htable(void)
{
    je_vm *vm; uint32_t index;
    if (je_sched_current_user(&vm, &index) != JE_OK) return (je_handle_table *)0;
    return &vm->procs[index].htable;
}

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
    case JE_SYSCALL_COPY_PROBE: {
        /* dev-only: stage <=256 user bytes src->kernel->dst with prevalidation
         * and the exception-fixup net. src=RDI, dst=RSI, len=RDX. */
        je_abi_status s = je_copy_probe(f->rdi, f->rsi, f->rdx);
        f->rax = (uint64_t)s; f->rdx = 0;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_SELF: {
        /* Mint a SELF handle in the caller's own table. SELF carries no object
         * pointer and no rights (rights are descriptive-only / unenforced in
         * M3b; REVISIT in docs). Handle returned in RDX. */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = 0;
        je_handle_status s = je_handle_alloc(t, (void *)0, JE_HANDLE_TYPE_SELF, 0u, &h);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (uint64_t)h;                 /* 0 on failure (alloc clears out) */
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_CLOSE: {
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle_status s = je_handle_close(t, (je_handle)f->rdi);
        f->rax = (uint64_t)handle_status_to_abi(s); f->rdx = 0;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_QUERY: {
        /* Look up the handle for its object type. No expected-type/rights are
         * required from ring 3 in M3b, so WRONGTYPE/DENIED are not user-reachable
         * here (they are covered by the hosted table tests and the mapper). On OK,
         * RDX = object type; otherwise RDX = 0. */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        void *obj;
        je_handle_status s = je_handle_lookup(t, (je_handle)f->rdi,
                                              JE_HANDLE_TYPE_SELF, 0u, &obj);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (s == JE_HANDLE_OK) ? (uint64_t)JE_HANDLE_TYPE_SELF : 0u;
        return je_sched_return_to_user(f);
    }
#ifdef JE_DEV_COPY_INJECT
    case JE_SYSCALL_COPY_FIXUP_INJECT: {
        /* dev-only: raw-copy an unmapped arena page to prove the fixup recovers;
         * RDX reports 1 if recovered. RDI = the unmapped user address. */
        int recovered = je_copy_fixup_inject(f->rdi);
        f->rax = (uint64_t)JE_ABI_OK; f->rdx = (uint64_t)recovered;
        return je_sched_return_to_user(f);
    }
#endif
    case JE_SYSCALL_RETURN:
    default:
        f->rax = (uint64_t)status; f->rdx = result;
        /* Validate+sanitize the frame on this user return (ADR: every return). */
        return je_sched_return_to_user(f);
    }
}
#endif
