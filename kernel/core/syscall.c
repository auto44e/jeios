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
#include <jeios/endpoint.h>
#include <jeios/endpoint_kern.h>
#include <jeios/arch.h>
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
    case JE_SYS_HANDLE_RIGHTS:
        /* handle_rights(h=RDI): RSI..R9 reserved must be zero; wide handle
         * rejected before narrowing. The glue looks the handle up requiring the
         * INSPECT right and returns the masked rights mask in RDX. */
        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_RIGHTS;
    case JE_SYS_HANDLE_DUP:
        /* handle_dup(h=RDI, new_rights=RSI): RDX/R10/R8/R9 reserved must be zero;
         * both the handle (RDI) and the rights (RSI) must fit 32 bits; and the
         * rights must contain only KNOWN bits (an unknown bit is a malformed call
         * -> INVALID, distinct from asking for a known-but-unheld right which the
         * glue reports as DENIED). The glue does the resolve + subset + allocate. */
        if (rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if ((rdi >> 32) || (rsi >> 32)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rsi & ~(uint64_t)JE_ABI_RIGHTS_KNOWN) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_HANDLE_DUP;
    case JE_SYS_ENDPOINT_CREATE:
        /* endpoint_create(): takes NO args; every register reserved. Mints both
         * ends into the caller's table (dispatch side). All-zero args or INVALID. */
        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_ENDPOINT_CREATE;
    case JE_SYS_TRY_SEND:
        /* try_send(h=RDI, ubuf=RSI, len=RDX, meta=R10, R8, R9). Shape checks only;
         * the copy + enqueue happen in the glue. R10[7:0]=cap_count must be 0
         * (data-only, D9); R10[63:8] reserved must be 0; so R10 must be 0 entirely
         * this step. R8 (step-5 cap-array ptr) and R9 reserved must be 0. len must
         * be <= JE_EP_MSG_MAX. Handle must fit 32 bits. */
        if (r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdx > (uint64_t)JE_ABI_MSG_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_TRY_SEND;
    case JE_SYS_TRY_RECV:
        /* try_recv(h=RDI, ubuf=RSI, cap=RDX, R10/R8/R9 reserved must be 0). */
        if (r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_TRY_RECV;
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
 * resource-free process-identity capability (ADR 0008). It is granted the
 * INSPECT right so handle_rights has a right to check; the mask is a kernel
 * constant asserted to contain only known bits (no unknown bit can be minted). */
#define JE_HANDLE_TYPE_SELF   JE_ABI_OBJ_SELF
#define JE_HANDLE_RIGHTS_SELF JE_ABI_RIGHT_INSPECT
_Static_assert((JE_HANDLE_RIGHTS_SELF & ~JE_ABI_RIGHTS_KNOWN) == 0u,
               "SELF must be minted only with known rights bits");
/* The table's SELF-only dup gate uses JE_HANDLE_OBJ_SELF; it MUST equal the ABI
 * SELF type the glue mints/passes, or dup_self would reject legitimate SELF
 * handles (or the gate would be bypassable). Pin them together here. */
_Static_assert(JE_HANDLE_OBJ_SELF == JE_ABI_OBJ_SELF, "SELF type: table vs ABI");

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

/* Resolve the current user process as (vm,index) for the endpoint ops, which
 * need the vm (teardown-hook check + pool) not just the table. Returns false for
 * a non-user caller. */
static bool current_vm_index(je_vm **out_vm, uint32_t *out_index)
{
    return je_sched_current_user(out_vm, out_index) == JE_OK;
}

/* M3c (A3-2): a handle packs losslessly into half of the RDX scalar only because
 * it is 32 bits. The endpoint_create two-handle return depends on this. */
_Static_assert(sizeof(je_handle) == 4u, "endpoint_create packs two handles in RDX");
/* The table-side endpoint type/rights copies must equal the ABI ones the glue
 * uses, or typed dispatch and create would disagree. */
_Static_assert(JE_ENDPOINT_OBJ_TYPE == JE_ABI_OBJ_ENDPOINT, "endpoint type: table vs ABI");
_Static_assert(JE_EP_RIGHT_SEND == JE_ABI_RIGHT_SEND
               && JE_EP_RIGHT_RECV == JE_ABI_RIGHT_RECV
               && JE_EP_RIGHT_TRANSFER == JE_ABI_RIGHT_TRANSFER
               && JE_EP_RIGHT_INSPECT == JE_ABI_RIGHT_INSPECT,
               "endpoint rights: table vs ABI");
_Static_assert(JE_EP_MSG_MAX == JE_ABI_MSG_MAX, "endpoint msg max: table vs ABI");

/* Map endpoint-layer status to the v0 ABI (reuses the JE_ABI_H_* namespace; the
 * glue is the only place endpoint.h and abi.h meet). */
static je_abi_status ep_status_to_abi(je_ep_status s)
{
    switch (s) {
    case JE_EP_OK:          return JE_ABI_OK;
    case JE_EP_BADINDEX:    return JE_ABI_H_BADINDEX;
    case JE_EP_STALE:       return JE_ABI_H_STALE;
    case JE_EP_WRONGTYPE:   return JE_ABI_H_WRONGTYPE;
    case JE_EP_DENIED:      return JE_ABI_H_DENIED;
    case JE_EP_QUOTA:       return JE_ABI_H_QUOTA;
    case JE_EP_NOSLOTS:     return JE_ABI_H_NOSLOTS;
    case JE_EP_REFOVERFLOW: return JE_ABI_H_REFOVERFLOW;
    case JE_EP_QUEUE_FULL:  return JE_ABI_H_QUEUE_FULL;
    case JE_EP_SHORT_BUFFER: return JE_ABI_H_SHORT_BUFFER;
    case JE_EP_PEER_CLOSED: return JE_ABI_H_PEER_CLOSED;
    case JE_EP_EMPTY:       return JE_ABI_H_EMPTY;
    case JE_EP_INVALID:     return JE_ABI_INVALID;
    case JE_EP_CORRUPT:     return JE_ABI_UNSUPPORTED; /* unwired vm / invariant */
    }
    return JE_ABI_INVALID;
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
         * pointer; it is granted the INSPECT right (JE_HANDLE_RIGHTS_SELF). A
         * freshly minted SELF holds INSPECT, so DENIED is not observable on it
         * directly — but handle_dup can derive a rights-reduced handle on which
         * handle_rights then returns DENIED (ring-3-observable). Handle in RDX. */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = 0;
        je_handle_status s = je_handle_alloc(t, (void *)0, JE_HANDLE_TYPE_SELF,
                                             JE_HANDLE_RIGHTS_SELF, &h);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (uint64_t)h;                 /* 0 on failure (alloc clears out) */
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_CLOSE: {
        /* Typed close dispatch (A3-3): peek the slot's object type, then branch
         * EXACTLY — SELF -> raw table close; ENDPOINT -> endpoint-aware close
         * (drops the handle_ref); any other/unknown type -> fail closed. The peek
         * and the close are atomic because this int 0x80 path runs at IF=0 on one
         * CPU (SMP revisit). A raw je_handle_close on an endpoint slot would leak
         * a ref, which is exactly what this dispatch prevents. */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle_table *t = &vm->procs[index].htable;
        je_handle h = (je_handle)f->rdi;
        uint32_t type = 0u; je_handle peek = 0u;
        /* The decoder already rejected a wide handle (rdi>>32) as INVALID. Decode
         * to an index and read the slot type via the read-only iterator, matching
         * on the same generation by re-encoding and comparing. */
        uint32_t di = 0u, dg = 0u; je_handle_decode(h, &di, &dg);
        bool live = je_handle_live_at(t, di, &peek, &type) && peek == h;
        if (live && type == JE_ABI_OBJ_ENDPOINT) {
            je_ep_status s = je_endpoint_kern_close(vm, index, h);
            f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
        } else if (live && type != JE_ABI_OBJ_SELF) {
            /* A LIVE slot of a type that is neither ENDPOINT nor SELF must FAIL
             * CLOSED (A3-3): routing it to raw je_handle_close would drop a ref a
             * future typed object owns. Unreachable today (only SELF/ENDPOINT are
             * minted), but the contract is enforced, not assumed. */
            f->rax = (uint64_t)JE_ABI_H_WRONGTYPE; f->rdx = 0;
        } else {
            /* SELF, or a not-live handle (stale/forged/free/retired): raw close
             * gives the exact OK/BADINDEX/STALE status. */
            je_handle_status s = je_handle_close(t, h);
            f->rax = (uint64_t)handle_status_to_abi(s); f->rdx = 0;
        }
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
    case JE_SYSCALL_HANDLE_RIGHTS: {
        /* Look up the handle requiring the INSPECT right, and return its rights
         * mask (masked to the known ABI bits, so an internal bit can never leak).
         * DENIED here IS ring-3-observable: handle_dup can derive a handle that
         * lacks INSPECT, and this op on that handle returns DENIED (proven by the
         * M3b-s1 QEMU marker). */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        uint32_t rights = 0;
        je_handle_status s = je_handle_rights_of(t, (je_handle)f->rdi,
                                                 JE_HANDLE_TYPE_SELF,
                                                 JE_ABI_RIGHT_INSPECT, &rights);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (s == JE_HANDLE_OK) ? (uint64_t)(rights & JE_ABI_RIGHTS_KNOWN) : 0u;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_HANDLE_DUP: {
        /* Attenuate: derive a new SELF handle (RDI) with a rights SUBSET (RSI).
         * The decoder already rejected unknown rights bits as INVALID; here a
         * request for a known-but-unheld right returns DENIED (never escalates).
         * The new handle is a fresh, independently-closable slot naming the same
         * SELF object; it counts against quota. This is where DENIED becomes
         * genuinely ring-3-observable: dup(self, 0) yields a rights-less handle on
         * which handle_rights then returns DENIED. */
        je_handle_table *t = current_htable();
        if (!t) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle nh = 0;
        je_handle_status s = je_handle_dup_self(t, (je_handle)f->rdi,
                                                JE_HANDLE_TYPE_SELF,
                                                (uint32_t)f->rsi, &nh);
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (uint64_t)nh;                /* 0 on failure (dup clears out) */
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_ENDPOINT_CREATE: {
        /* endpoint_create() (A3-2): mint both ends into the caller's own table
         * via the kernel pool wrapper, which refuses unless this vm has the
         * teardown hook installed (UNSUPPORTED) so a created endpoint is always
         * on a death path that reclaims it. On OK, pack the two 32-bit handles:
         * RDX[31:0]=h0, RDX[63:32]=h1. RDX is left 0 on failure (undefined by
         * ABI, but we zero it rather than leak a stale value). */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h0 = 0u, h1 = 0u;
        je_ep_status s = je_endpoint_kern_create(vm, index, &h0, &h1);
        f->rax = (uint64_t)ep_status_to_abi(s);
        f->rdx = (s == JE_EP_OK)
               ? ((uint64_t)h0 | ((uint64_t)h1 << 32)) : 0u;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_TRY_SEND: {
        /* D6 send: read-only PREFLIGHT (so a bad ubuf never masks DENIED/
         * PEER_CLOSED/QUEUE_FULL, and user memory is not read for an
         * unauthorized handle) -> copy_from_user into a STACK staging buffer
         * (fault -> INVALID, nothing enqueued) -> pure try_send re-checks +
         * enqueues. */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = (je_handle)f->rdi;
        uint64_t ubuf = f->rsi, len = f->rdx;   /* decode bounded len <= MSG_MAX */
        je_ep_status pf = je_endpoint_kern_send_preflight(vm, index, h, (size_t)len);
        if (pf != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(pf); f->rdx=0; return je_sched_return_to_user(f); }
        uint8_t staging[JE_EP_MSG_MAX];
        if (len != 0u) {
            je_abi_status cs = je_copy_from_user(staging, ubuf, len);
            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        }
        je_ep_status s = je_endpoint_kern_send(vm, index, h, staging, (size_t)len);
        f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_TRY_RECV: {
        /* D6 recv: PEEK into a stack buffer (no dequeue) -> glue checks cap ->
         * copy_to_user (fault -> INVALID, message stays queued) -> INFALLIBLE
         * commit (dequeue). A commit mismatch halts (double-deliver hazard). */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = (je_handle)f->rdi;
        uint64_t ubuf = f->rsi, cap = f->rdx;
        uint8_t staging[JE_EP_MSG_MAX];
        size_t mlen = 0u;
        je_ep_recv_lease lease;
        je_ep_status s = je_endpoint_kern_recv_peek(vm, index, h, staging,
                                                    (size_t)cap, &mlen, &lease);
        if (s == JE_EP_SHORT_BUFFER) { f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER; f->rdx=(uint64_t)mlen; return je_sched_return_to_user(f); }
        if (s != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return je_sched_return_to_user(f); }
        /* Peek OK: copy out, then commit. Zero-length delivers nothing to copy. */
        if (mlen != 0u) {
            je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        }
        je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
        if (cm != JE_EP_OK) {
            /* Impossible under IF=0 single-CPU; a retry would double-deliver, so
             * this is a kernel invariant violation, not a user status. */
            je_arch_console_write("JeiOS fatal: recv commit mismatch\r\n");
            je_arch_halt();
        }
        f->rax = (uint64_t)JE_ABI_OK; f->rdx = (uint64_t)mlen;
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
