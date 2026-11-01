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
#include <jeios/vm.h>
#include <jeios/endpoint.h>
#ifndef JE_SYSCALL_HOSTED_TEST
#include <jeios/handle.h>
#include <jeios/endpoint_kern.h>
#include <jeios/proc.h>
#include <jeios/arch.h>
#include <jeios/spawn.h>
#include <jeios/device.h>
extern je_ep_status je_endpoint_kern_dup(je_vm *vm, uint32_t index, je_handle src,
                                         uint32_t rights, je_handle *out);
extern bool je_endpoint_kern_is_wired(const je_vm *vm);
#if JE_TEST_MODE == 15
extern void je_m4g_record_cap_delivery(je_vm *vm, uint32_t index,
                                       const je_ep_capdesc *descriptor);
extern void je_m4i_record_control_receive(je_vm *vm, uint32_t index,
                                          const void *message, size_t length,
                                          uint8_t cap_count);
#endif
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
        /* try_send(h=RDI, ubuf=RSI, len=RDX, meta=R10, cap_array=R8, R9).
         * R10[7:0] = cap_count (0..JE_ABI_MSG_CAPS_MAX); R10[63:8] reserved = 0.
         * R8 = user pointer to a u64[cap_count] of {low32 handle, high32 rights}
         * (0 iff cap_count==0). R9 reserved = 0. len <= JE_ABI_MSG_MAX. (A5-4) */
        if (r10 >> 8) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; } /* reserved bits */
        if (r9)       { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if ((r10 & 0xffu) > (uint64_t)JE_ABI_MSG_CAPS_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        /* R8 must be 0 iff cap_count==0 (and nonzero iff cap_count>0). */
        if (((r10 & 0xffu) == 0u) != (r8 == 0u)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdx > (uint64_t)JE_ABI_MSG_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_TRY_SEND;
    case JE_SYS_TRY_RECV:
        /* try_recv(h=RDI, ubuf=RSI, cap=RDX, hbuf=R8, hcap=R9, timeout=R10).
         * R8 = user u32[] to receive minted handles; R9 = its slot capacity
         * (0..JE_ABI_MSG_CAPS_MAX; a larger value is rejected so it can't be
         * truncated to a smaller capacity in the glue). R8 must be 0 iff R9==0.
         * R10 = A6-ABI relative tick timeout: 0 = non-blocking poll (today's
         * behavior), nonzero = block up to JE_IPC_MAX_WAIT (bound+overflow checked
         * in the glue). Any R10 value is a legal arg now (no reserved-0 rule). */
        if (r9 > (uint64_t)JE_ABI_MSG_CAPS_MAX) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if ((r8 == 0u) != (r9 == 0u)) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_TRY_RECV;
    case JE_SYS_CLOCK_TICKS:
        if (rdi|rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_CLOCK_TICKS;
    case JE_SYS_PROC_SPAWN: {
        uint32_t handle_quota = JE_ABI_SPAWN_QUOTA_HANDLES(r8);
        uint32_t endpoint_quota = JE_ABI_SPAWN_QUOTA_EPS(r8);
        if ((rdi >> 32) || (rsi >> 32) || r9 != 0u
            || r10 > JE_ABI_SPAWN_GRANTS_MAX
            || ((r10 == 0u) != (rdx == 0u))
            || (r8 >> 32) != 0u
            || handle_quota > JE_VM_HANDLE_QUOTA_MAX
            || endpoint_quota > JE_EP_CHILD_QUOTA_MAX) {
            *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN;
        }
        *out_status=JE_ABI_OK; return JE_SYSCALL_PROC_SPAWN;
    }
    case JE_SYS_PROC_STATUS:
        /* proc_status(h=RDI, out_ptr=RSI): the handle is narrowed only after
         * this decode check; all other registers are reserved. */
        if (rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_PROC_STATUS;
    case JE_SYS_PROC_KILL:
        /* proc_kill(h=RDI): the handle is narrowed only after this decode check;
         * every other argument is reserved. */
        if (rsi|rdx|r10|r8|r9) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        if (rdi >> 32) { *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN; }
        *out_status=JE_ABI_OK; return JE_SYSCALL_PROC_KILL;
    case JE_SYS_DEV_ASSIGN:
        if ((rdi >> 32) || (rsi >> 32)
            || (rdx & ~(uint64_t)JE_ABI_DEV_REQUIRE_DMA_CONTAINED)
            || r10 || r8 || r9) {
            *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN;
        }
        *out_status=JE_ABI_OK; return JE_SYSCALL_DEV_ASSIGN;
    case JE_SYS_DEV_REVOKE:
        if ((rdi >> 32) || rsi || rdx || r10 || r8 || r9) {
            *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN;
        }
        *out_status=JE_ABI_OK; return JE_SYSCALL_DEV_REVOKE;
    case JE_SYS_DEV_IRQ_ACK:
        if ((rdi >> 32) || rsi || rdx || r10 || r8 || r9) {
            *out_status=JE_ABI_INVALID; return JE_SYSCALL_RETURN;
        }
        *out_status=JE_ABI_OK; return JE_SYSCALL_DEV_IRQ_ACK;
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
#define JE_PROCESS_OBJ_TYPE   JE_ABI_OBJ_PROCESS
#define JE_PROCESS_TYPE       JE_PROCESS_OBJ_TYPE
_Static_assert(JE_PROCESS_OBJ_TYPE == JE_ABI_OBJ_PROCESS, "PROCESS type: ABI");
_Static_assert(JE_PROC_FREE == 0u, "proc FREE ABI baseline");
_Static_assert(JE_PROC_LIVE == JE_ABI_PROC_LIVE, "proc LIVE ABI");
_Static_assert(JE_PROC_DYING == JE_ABI_PROC_DYING, "proc DYING ABI");
_Static_assert(JE_PROC_DEAD == JE_ABI_PROC_DEAD, "proc DEAD ABI");
_Static_assert(JE_DEATH_FAULT == JE_ABI_DEATH_FAULT, "death FAULT ABI");
_Static_assert(JE_DEATH_EXIT == JE_ABI_DEATH_EXIT, "death EXIT ABI");
_Static_assert(JE_DEATH_BAD_RETURN == JE_ABI_DEATH_BAD_RETURN,
               "death BAD_RETURN ABI");
_Static_assert(JE_DEATH_KILLED == JE_ABI_DEATH_KILLED, "death KILLED ABI");
_Static_assert(JE_PROC_REASON_MAX == JE_DEATH_KILLED, "proc reason bound");
_Static_assert((JE_HANDLE_RIGHTS_SELF & ~JE_ABI_RIGHTS_KNOWN) == 0u,
               "SELF must be minted only with known rights bits");
/* The table's SELF-only dup gate uses JE_HANDLE_OBJ_SELF; it MUST equal the ABI
 * SELF type the glue mints/passes, or dup_self would reject legitimate SELF
 * handles (or the gate would be bypassable). Pin them together here. */
_Static_assert(JE_HANDLE_OBJ_SELF == JE_ABI_OBJ_SELF, "SELF type: table vs ABI");
_Static_assert(JE_ENDPOINT_OBJ_TYPE == JE_ABI_OBJ_ENDPOINT, "endpoint type: ABI");

typedef enum {
    JE_DUP_POLICY_DENIED = 0,
    JE_DUP_POLICY_SELF,
    JE_DUP_POLICY_ENDPOINT,
    JE_DUP_POLICY_PROCESS
} je_dup_policy;

typedef struct {
    uint32_t known_rights;
    je_dup_policy dup;
    uint32_t inspect_result;
    je_status (*retain)(void *object);
    void (*release)(void *object);
} je_type_dispatch;

static bool process_id_from_object(je_proc_pool *pool, void *object,
                                   je_proc_id *out);
static je_status dispatch_retain_process(void *object);
static void dispatch_release_process(void *object);

static const je_type_dispatch type_dispatch[JE_ABI_OBJ_DEVICE + 1u] = {
    [JE_ABI_OBJ_SELF] = {
        JE_ABI_RIGHT_INSPECT, JE_DUP_POLICY_SELF, JE_ABI_OBJ_SELF,
        (je_status (*)(void *))0, (void (*)(void *))0
    },
    [JE_ABI_OBJ_ENDPOINT] = {
        JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV
            | JE_ABI_RIGHT_TRANSFER,
        JE_DUP_POLICY_ENDPOINT, JE_ABI_OBJ_ENDPOINT,
        (je_status (*)(void *))0, (void (*)(void *))0
    },
    [JE_ABI_OBJ_PROCESS] = {
        JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL, JE_DUP_POLICY_PROCESS,
        JE_ABI_OBJ_PROCESS, dispatch_retain_process, dispatch_release_process
    },
    [JE_ABI_OBJ_LAUNCHER] = {
        JE_ABI_RIGHT_SPAWN, JE_DUP_POLICY_DENIED, JE_ABI_OBJ_LAUNCHER,
        (je_status (*)(void *))0, (void (*)(void *))0
    },
    [JE_ABI_OBJ_DEVICE] = {
        JE_ABI_RIGHT_PIO | JE_ABI_RIGHT_IRQ | JE_ABI_RIGHT_ASSIGN,
        JE_DUP_POLICY_DENIED, JE_ABI_OBJ_DEVICE,
        (je_status (*)(void *))0, (void (*)(void *))0
    }
};

static const je_type_dispatch *dispatch_for_type(uint32_t type)
{
    if (type == 0u || type > JE_ABI_OBJ_DEVICE
        || type_dispatch[type].known_rights == 0u)
        return (const je_type_dispatch *)0;
    return &type_dispatch[type];
}

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

static je_proc_pool *process_pool(void)
{
    extern je_proc_pool *je_sched_proc_pool(void);
    return je_sched_proc_pool();
}

static bool process_id_from_object(je_proc_pool *pool, void *object, je_proc_id *out)
{
    if (!pool || !out || !object) return false;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        if (object == (void *)&pool->rec[i] && pool->rec[i].state != JE_PROC_FREE) {
            *out = (je_proc_id){i, pool->rec[i].gen};
            return true;
        }
    }
    return false;
}

_Noreturn void syscall_invariant(const char *message)
{
    je_arch_console_write(message);
    je_arch_halt();
}

static je_status dispatch_retain_process(void *object)
{
    je_proc_id id = JE_PROC_ID_NONE;
    je_proc_pool *pool = process_pool();
    if (!process_id_from_object(pool, object, &id))
        syscall_invariant("JeiOS fatal: PROCESS retain invariant\r\n");
    return je_proc_retain(pool, id);
}

static void dispatch_release_process(void *object)
{
    je_proc_id id = JE_PROC_ID_NONE;
    je_proc_pool *pool = process_pool();
    if (!process_id_from_object(pool, object, &id)
        || pool->rec[id.index].handle_refs == 0u)
        syscall_invariant("JeiOS fatal: PROCESS release invariant\r\n");
    je_proc_release(pool, id);
}

je_status je_process_handle_mint(je_vm *vm, uint32_t index, je_proc_id id,
                                 uint32_t rights, je_handle *out,
                                 je_handle_status *out_alloc_status)
{
    if (out) *out = 0u;
    if (out_alloc_status) *out_alloc_status = JE_HANDLE_OK;
    if (!vm || index >= JE_VM_MAX_PROCESSES || !out
        || !je_endpoint_kern_is_wired(vm) || !dispatch_for_type(JE_PROCESS_TYPE)
        || (rights & ~JE_ABI_RIGHTS_KNOWN) != 0u
        || (rights & ~type_dispatch[JE_PROCESS_TYPE].known_rights) != 0u)
        return JE_ERR_INVALID;
    je_proc_pool *pool = process_pool();
    if (!pool || !je_proc_valid(pool, id)) return JE_ERR_INVALID;
    je_status retain = type_dispatch[JE_PROCESS_TYPE].retain(
        (void *)&pool->rec[id.index]);
    if (retain != JE_OK) return retain;
    je_handle h = 0u;
    je_handle_status alloc = je_handle_alloc(&vm->procs[index].htable,
                                             (void *)&pool->rec[id.index],
                                             JE_PROCESS_TYPE, rights, &h);
    if (alloc != JE_HANDLE_OK) {
        if (out_alloc_status) *out_alloc_status = alloc;
        type_dispatch[JE_PROCESS_TYPE].release((void *)&pool->rec[id.index]);
        switch (alloc) {
        case JE_HANDLE_QUOTA: return JE_ERR_DENIED;
        case JE_HANDLE_NOSLOTS: return JE_ERR_NOMEM;
        default: return JE_ERR_INVALID;
        }
    }
    *out = h;
    return JE_OK;
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
_Static_assert(JE_EP_MSG_CAPS_MAX == JE_ABI_MSG_CAPS_MAX, "endpoint caps max: table vs ABI");

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
    case JE_EP_WAITERS_BUSY: return JE_ABI_H_EMPTY; /* A6-4: occupied slot -> non-blocking EMPTY */
    case JE_EP_INVALID:     return JE_ABI_INVALID;
    case JE_EP_CORRUPT:     return JE_ABI_UNSUPPORTED; /* unwired vm / invariant */
    }
    return JE_ABI_INVALID;
}

/* M3c A6: finish a resumed blocking recv. The thread parked on (wrec,wside) with
 * out-args ubuf=rsi, cap=rdx, hbuf=r8, hcap=r9. Read the terminal cause the single
 * arbitrate CAS stamped and deliver into f. WOKEN: peek the reserved head (bytes +
 * caps) from the OWNED end (no handle re-resolve, A6-5), stage mints, copy out,
 * commit, release the waiter. TIMED_OUT/PEER_CLOSED/CANCELLED: write the status.
 * A copy/mint fault on WOKEN -> JE_ABI_INVALID, message stays queued, mints rolled
 * back, reservation released (the head returns to deliverable). */
static void je_syscall_finish_blocked_recv(je_trap_frame *f, je_ep_channel *rec,
                                           uint8_t side, uint64_t ubuf, uint64_t cap,
                                           uint64_t hbuf, uint64_t hcap)
{
    je_vm *vm; uint32_t index;
    if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return; }
    je_ep_wait_cause cause = je_endpoint_wait_cause(rec, side);
    uint32_t slot = 0u; (void)je_sched_current_slot(&slot);
    switch (cause) {
    case JE_EP_WAIT_TIMED_OUT:
        je_endpoint_kern_wait_release(rec, side, slot);
        f->rax=(uint64_t)JE_ABI_H_TIMED_OUT; f->rdx=0; return;
    case JE_EP_WAIT_PEER_CLOSED:
        je_endpoint_kern_wait_release(rec, side, slot);
        f->rax=(uint64_t)JE_ABI_H_PEER_CLOSED; f->rdx=0; return;
    case JE_EP_WAIT_CANCELLED:
        je_endpoint_kern_wait_release(rec, side, slot);
        f->rax=(uint64_t)JE_ABI_H_CANCELLED; f->rdx=0; return;
    case JE_EP_WAIT_WOKEN: break;    /* deliver below */
    default:
        /* No terminal cause on a resumed thread = kernel-invariant break. */
        je_arch_console_write("JeiOS fatal: recv resume without terminal cause\r\n");
        je_arch_halt(); return;
    }
    /* WOKEN delivery from the owned reserved head. */
    uint8_t staging[JE_EP_MSG_MAX];
    je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
    size_t mlen = 0u; uint8_t mcaps = 0u;
    je_ep_recv_lease lease;
    je_ep_status s = je_endpoint_kern_recv_peek_owned(rec, side, staging, (size_t)cap,
                                                      &mlen, (uint8_t)hcap, caps, &mcaps,
                                                      &lease);
    if (s == JE_EP_SHORT_BUFFER) {
        /* Buffer too small: leave the message QUEUED but RELEASE the waiter (the
         * head returns to deliverable), so a re-issued recv with a bigger buffer
         * takes the normal path. NOT releasing here would wedge the end forever —
         * the continuation only runs while wait_pending is set, which we clear on
         * return. Report the required sizes. */
        je_endpoint_kern_wait_release(rec, side, slot);
        f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
        f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32); return;
    }
    if (s != JE_EP_OK) { je_endpoint_kern_wait_release(rec, side, slot);
                         f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return; }
    /* Stage mints (dead cap -> handle 0; roll back all on transient fault). */
    je_handle minted[JE_ABI_MSG_CAPS_MAX]; bool was_live[JE_ABI_MSG_CAPS_MAX];
    for (uint8_t c = 0; c < mcaps; ++c) { minted[c]=0u; was_live[c]=false; }
    je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
    for (uint8_t c = 0; c < mcaps; ++c) {
        if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c]=0u; continue; }
        je_handle nh = 0u;
        je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
        if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
        minted[c]=nh; was_live[c]=true; done=(uint8_t)(c+1u);
    }
    if (fail != JE_ABI_OK) {
        for (uint8_t c = 0; c < done; ++c)
            if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
        /* Message stays queued+reserved; A6-5 says a fault does NOT consume. But a
         * transient mint fault must not wedge the waiter forever: release it so a
         * re-issue can retry (the head returns to deliverable). */
        je_endpoint_kern_wait_release(rec, side, slot);
        f->rax=(uint64_t)fail; f->rdx=0; return;
    }
    if (mlen != 0u) {
        je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
        if (cs != JE_ABI_OK) {
            for (uint8_t c = 0; c < mcaps; ++c)
                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
            je_endpoint_kern_wait_release(rec, side, slot);
            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
        }
    }
    if (mcaps != 0u) {
        uint32_t hout[JE_ABI_MSG_CAPS_MAX];
        for (uint8_t c = 0; c < mcaps; ++c) hout[c]=(uint32_t)minted[c];
        je_abi_status cs = je_copy_to_user(hbuf, hout, (uint64_t)mcaps*sizeof(uint32_t));
        if (cs != JE_ABI_OK) {
            for (uint8_t c = 0; c < mcaps; ++c)
                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
            je_endpoint_kern_wait_release(rec, side, slot);
            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return;
        }
    }
    /* Infallible commit (dequeue + release inflight), then free the waiter. */
    je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
    if (cm != JE_EP_OK) { je_arch_console_write("JeiOS fatal: blocked recv commit mismatch\r\n"); je_arch_halt(); }
#if JE_TEST_MODE == 13
    for (uint8_t c = 0u; c < mcaps; ++c)
        if (was_live[c] && minted[c])
            je_m4g_record_cap_delivery(vm, index, &caps[c]);
    je_m4g_record_control_receive(vm, index, &lease, staging, mlen, mcaps);
#endif
#if JE_TEST_MODE == 15
    for (uint8_t c = 0u; c < mcaps; ++c)
        if (was_live[c] && minted[c])
            je_m4g_record_cap_delivery(vm, index, &caps[c]);
    je_m4i_record_control_receive(vm, index, staging, mlen, mcaps);
#endif
    je_endpoint_kern_wait_release(rec, side, slot);
    f->rax=(uint64_t)JE_ABI_OK;
    f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32);
}

static je_trap_frame *dispatch_proc_status(je_trap_frame *f)
{
    je_handle_table *t = current_htable();
    if (!t) {
        f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0;
        return je_sched_return_to_user(f);
    }
    uint32_t type = 0u, rights = 0u; void *object = (void *)0;
    je_handle_status resolve = je_handle_resolve_any(t, (je_handle)f->rdi,
                                                      &type, &rights, &object);
    if (resolve != JE_HANDLE_OK) {
        f->rax=(uint64_t)handle_status_to_abi(resolve); f->rdx=0;
        return je_sched_return_to_user(f);
    }
    if (type != JE_PROCESS_TYPE) {
        f->rax=(uint64_t)JE_ABI_H_WRONGTYPE; f->rdx=0;
        return je_sched_return_to_user(f);
    }
    if ((rights & JE_ABI_RIGHT_INSPECT) == 0u) {
        f->rax=(uint64_t)JE_ABI_H_DENIED; f->rdx=0;
        return je_sched_return_to_user(f);
    }
    je_proc_id id = JE_PROC_ID_NONE;
    if (!process_id_from_object(process_pool(), object, &id)) {
        syscall_invariant("JeiOS fatal: PROC_STATUS record invariant\r\n");
    }
    je_abi_proc_status snapshot;
    if (je_proc_status(process_pool(), id, &snapshot.state, &snapshot.reason,
                       &snapshot.code) != JE_OK) {
        syscall_invariant("JeiOS fatal: PROC_STATUS snapshot invariant\r\n");
    }
    f->rax=(uint64_t)je_copy_to_user(f->rsi, &snapshot, sizeof snapshot);
    f->rdx = 0u;
    return je_sched_return_to_user(f);
}

static je_trap_frame *dispatch_proc_kill(je_trap_frame *f)
{
    je_handle_table *t = current_htable();
    if (!t) {
        f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0;
        return je_sched_return_to_user(f);
    }
    uint32_t type = 0u, rights = 0u; void *object = (void *)0;
    je_handle_status resolve = je_handle_resolve_any(t, (je_handle)f->rdi,
                                                      &type, &rights, &object);
    if (resolve != JE_HANDLE_OK) {
        f->rax=(uint64_t)handle_status_to_abi(resolve); f->rdx=0;
        return je_sched_return_to_user(f);
    }
    if (type != JE_PROCESS_TYPE) {
        f->rax=(uint64_t)JE_ABI_H_WRONGTYPE; f->rdx=0;
        return je_sched_return_to_user(f);
    }
    if ((rights & JE_ABI_RIGHT_KILL) == 0u) {
        f->rax=(uint64_t)JE_ABI_H_DENIED; f->rdx=0;
        return je_sched_return_to_user(f);
    }
    je_proc_id id = JE_PROC_ID_NONE;
    if (!process_id_from_object(process_pool(), object, &id))
        syscall_invariant("JeiOS fatal: PROC_KILL record invariant\r\n");
    je_status killed = je_sched_kill(id);
    f->rax=(uint64_t)((killed == JE_OK) ? JE_ABI_OK : JE_ABI_INVALID);
    f->rdx=0;
    return je_sched_return_to_user(f);
}

static je_abi_status resolve_device_admin(je_handle_table *t, je_handle h)
{
    uint32_t type = 0u, rights = 0u;
    void *object = (void *)0;
    je_handle_status s = je_handle_resolve_any(t, h, &type, &rights, &object);
    if (s != JE_HANDLE_OK) return handle_status_to_abi(s);
    if (type != JE_ABI_OBJ_DEVICE || !je_device_is_object(je_kernel_device(), object))
        return JE_ABI_H_WRONGTYPE;
    if ((rights & JE_ABI_RIGHT_ASSIGN) == 0u) return JE_ABI_H_DENIED;
    return JE_ABI_OK;
}

static je_trap_frame *dispatch_device_assign(je_trap_frame *f)
{
    je_vm *caller_vm = (je_vm *)0;
    uint32_t caller_index = 0u;
    je_proc_id caller = JE_PROC_ID_NONE;
    if (!current_vm_index(&caller_vm, &caller_index)
        || je_sched_current_proc_id(&caller) != JE_OK) {
        f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    je_abi_status admin = resolve_device_admin(
        &caller_vm->procs[caller_index].htable, (je_handle)f->rdi);
    if (admin != JE_ABI_OK) {
        f->rax=(uint64_t)admin; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    uint32_t type = 0u, rights = 0u;
    void *object = (void *)0;
    je_handle_status rs = je_handle_resolve_any(
        &caller_vm->procs[caller_index].htable, (je_handle)f->rsi,
        &type, &rights, &object);
    if (rs != JE_HANDLE_OK) {
        f->rax=(uint64_t)handle_status_to_abi(rs); f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    if (type != JE_ABI_OBJ_PROCESS) {
        f->rax=(uint64_t)JE_ABI_H_WRONGTYPE; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    if ((rights & JE_ABI_RIGHT_KILL) == 0u) {
        f->rax=(uint64_t)JE_ABI_H_DENIED; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    je_proc_id target = JE_PROC_ID_NONE;
    if (!process_id_from_object(process_pool(), object, &target)) {
        syscall_invariant("JeiOS fatal: DEV_ASSIGN process invariant\r\n");
    }
    if ((f->rdx & JE_ABI_DEV_REQUIRE_DMA_CONTAINED) != 0u) {
        je_device *device = je_kernel_device();
        bool first = device->hardened_refusal_count == 0u;
        je_device_note_hardened_refusal(device);
        if (first) je_arch_console_write("JeiOS: legacy no-DMA PIO\r\n");
        f->rax = (uint64_t)JE_ABI_UNSUPPORTED;
        f->rdx = JE_ABI_DEV_PROFILE_LEGACY_PIO;
        return je_sched_return_to_user(f);
    }
    je_vm *target_vm = (je_vm *)0;
    uint32_t target_index = 0u;
    if (je_sched_find_process(target, &target_vm, &target_index) != JE_OK) {
        f->rax=(uint64_t)JE_ABI_H_STALE; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    je_handle out = 0u;
    je_abi_status status = je_device_assign(
        je_kernel_device(), caller, target, target_vm, target_index, &out);
    f->rax=(uint64_t)status; f->rdx=(uint64_t)out;
    return je_sched_return_to_user(f);
}

static je_trap_frame *dispatch_device_revoke(je_trap_frame *f)
{
    je_vm *vm = (je_vm *)0;
    uint32_t index = 0u;
    if (!current_vm_index(&vm, &index)) {
        f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    je_abi_status status = resolve_device_admin(
        &vm->procs[index].htable, (je_handle)f->rdi);
    if (status == JE_ABI_OK) status = je_device_revoke(je_kernel_device());
    f->rax=(uint64_t)status; f->rdx=0u;
    return je_sched_return_to_user(f);
}

static je_trap_frame *dispatch_device_ack(je_trap_frame *f)
{
    je_vm *vm = (je_vm *)0;
    uint32_t index = 0u, pending = 0u;
    je_proc_id caller = JE_PROC_ID_NONE;
    if (!current_vm_index(&vm, &index)
        || je_sched_current_proc_id(&caller) != JE_OK) {
        f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    uint32_t type = 0u, rights = 0u;
    void *object = (void *)0;
    je_handle_status rs = je_handle_resolve_any(
        &vm->procs[index].htable, (je_handle)f->rdi,
        &type, &rights, &object);
    if (rs != JE_HANDLE_OK) {
        f->rax=(uint64_t)handle_status_to_abi(rs); f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    if (type != JE_ABI_OBJ_DEVICE) {
        f->rax=(uint64_t)JE_ABI_H_WRONGTYPE; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    if ((rights & JE_ABI_RIGHT_IRQ) == 0u) {
        f->rax=(uint64_t)JE_ABI_H_DENIED; f->rdx=0u;
        return je_sched_return_to_user(f);
    }
    je_abi_status status = je_device_irq_ack(
        je_kernel_device(), caller, vm, index, (je_handle)f->rdi, &pending);
    f->rax=(uint64_t)status; f->rdx=(uint64_t)pending;
    return je_sched_return_to_user(f);
}

/* Kernel glue. Returns the frame to iretq to (the caller for RETURN/invalid, a
 * live successor for YIELD/EXIT). Must only be called for a user-origin int 0x80
 * frame (je_trap_dispatch enforces cs&3==3; the scheduler helpers re-check the
 * frame identity). */
je_trap_frame *je_syscall_dispatch(je_trap_frame *f)
{
    /* M3c A6-CLARIFY-1: pending-continuation prologue. If the CURRENT thread
     * resumed from a blocking-recv park, FINISH that recv here — keyed on the
     * waiter state, NOT on RAX — before any op decode. The thread re-executed its
     * int 0x80, so RAX/args are intact, but we complete from the SAVED owned
     * rec/side + args (never re-resolving the handle, A6-5). The terminal cause
     * stamped by the single arbitrate CAS decides the result. */
    {
        je_ep_channel *wrec = (je_ep_channel *)0; uint8_t wside = 0u;
        uint64_t wrdi, wrsi, wrdx, wr8, wr9, wprip;
        if (je_sched_wait_pending(&wrec, &wside, &wrdi, &wrsi, &wrdx, &wr8, &wr9, &wprip)) {
            /* The restart must have landed exactly on the saved post-int RIP; any
             * mismatch is a kernel-invariant break, not a user error. */
            if (f->rip != wprip) { je_arch_console_write("JeiOS fatal: recv continuation RIP\r\n"); je_arch_halt(); }
            je_syscall_finish_blocked_recv(f, wrec, wside, wrsi, wrdx, wr8, wr9);
            je_sched_wait_finish();
            return je_sched_return_to_user(f);
        }
    }
    uint64_t args[6] = { f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9 };
    je_abi_status status; uint64_t result;
    je_syscall_action action = je_syscall_decode(f->rax, args, &status, &result);

    if (action == JE_SYSCALL_PROC_STATUS)
        return dispatch_proc_status(f);
    if (action == JE_SYSCALL_PROC_KILL)
        return dispatch_proc_kill(f);
    if (action == JE_SYSCALL_DEV_ASSIGN)
        return dispatch_device_assign(f);
    if (action == JE_SYSCALL_DEV_REVOKE)
        return dispatch_device_revoke(f);
    if (action == JE_SYSCALL_DEV_IRQ_ACK)
        return dispatch_device_ack(f);

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
        uint32_t type = 0u, rights = 0u; void *object = (void *)0;
        je_handle_status resolve = je_handle_resolve_any(t, h, &type, &rights, &object);
        if (resolve != JE_HANDLE_OK) {
            f->rax=(uint64_t)handle_status_to_abi(resolve); f->rdx=0;
        } else if (type == JE_ABI_OBJ_ENDPOINT) {
            je_ep_status s = je_endpoint_kern_close(vm, index, h);
            f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
        } else if (type == JE_ABI_OBJ_PROCESS) {
            je_proc_id id = JE_PROC_ID_NONE;
            if (!process_id_from_object(process_pool(), object, &id)
                || process_pool()->rec[id.index].handle_refs == 0u)
                syscall_invariant("JeiOS fatal: PROCESS close invariant\r\n");
            if (je_handle_close(t, h) != JE_HANDLE_OK)
                syscall_invariant("JeiOS fatal: PROCESS close mismatch\r\n");
            dispatch_release_process(object);
            f->rax=(uint64_t)JE_ABI_OK; f->rdx=0;
        } else if (type == JE_ABI_OBJ_DEVICE) {
            je_proc_id caller = JE_PROC_ID_NONE;
            if (je_sched_current_proc_id(&caller) != JE_OK)
                syscall_invariant("JeiOS fatal: DEVICE close caller invariant\r\n");
            bool revoke = je_device_close_matches(
                je_kernel_device(), caller, vm, index, h);
            je_handle_status s = je_handle_close(t, h);
            if (s == JE_HANDLE_OK && revoke)
                (void)je_device_revoke_close(je_kernel_device());
            f->rax = (uint64_t)handle_status_to_abi(s); f->rdx = 0u;
        } else if (type == JE_ABI_OBJ_SELF || type == JE_ABI_OBJ_LAUNCHER) {
            je_handle_status s = je_handle_close(t, h);
            f->rax = (uint64_t)handle_status_to_abi(s); f->rdx = 0;
        } else {
            f->rax = (uint64_t)JE_ABI_H_WRONGTYPE; f->rdx = 0;
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
        uint32_t type = 0u, rights = 0u; void *object = (void *)0;
        je_handle_status s = je_handle_resolve_any(t, (je_handle)f->rdi,
                                                   &type, &rights, &object);
        (void)rights; (void)object;
        if (s == JE_HANDLE_OK && dispatch_for_type(type) == (const je_type_dispatch *)0)
            s = JE_HANDLE_WRONGTYPE;
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (s == JE_HANDLE_OK) ? (uint64_t)type : 0u;
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
        uint32_t type = 0u, rights = 0u; void *object = (void *)0;
        je_handle_status s = je_handle_resolve_any(t, (je_handle)f->rdi,
                                                   &type, &rights, &object);
        const je_type_dispatch *dispatch = (s == JE_HANDLE_OK)
                                          ? dispatch_for_type(type)
                                          : (const je_type_dispatch *)0;
        if (s == JE_HANDLE_OK && !dispatch) s = JE_HANDLE_WRONGTYPE;
        if (s == JE_HANDLE_OK && (rights & JE_ABI_RIGHT_INSPECT) == 0u)
            s = JE_HANDLE_DENIED;
        f->rax = (uint64_t)handle_status_to_abi(s);
        f->rdx = (s == JE_HANDLE_OK) ? (uint64_t)(rights & dispatch->known_rights) : 0u;
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
        je_handle nh = 0u;
        uint32_t type = 0u, source_rights = 0u; void *object = (void *)0;
        je_handle_status resolve = je_handle_resolve_any(t, (je_handle)f->rdi,
                                                          &type, &source_rights, &object);
        const je_type_dispatch *dispatch = (resolve == JE_HANDLE_OK)
                                          ? dispatch_for_type(type)
                                          : (const je_type_dispatch *)0;
        if (resolve != JE_HANDLE_OK) {
            f->rax=(uint64_t)handle_status_to_abi(resolve); f->rdx=0;
            return je_sched_return_to_user(f);
        }
        uint32_t new_rights = (uint32_t)f->rsi;
        if (!dispatch) {
            f->rax=(uint64_t)JE_ABI_H_WRONGTYPE; f->rdx=0;
            return je_sched_return_to_user(f);
        }
        if ((new_rights & ~source_rights) != 0u) {
            f->rax=(uint64_t)JE_ABI_H_DENIED; f->rdx=0;
            return je_sched_return_to_user(f);
        }
        if (dispatch->dup == JE_DUP_POLICY_DENIED) {
            f->rax=(uint64_t)JE_ABI_H_DENIED; f->rdx=0;
            return je_sched_return_to_user(f);
        }
        if (dispatch->dup == JE_DUP_POLICY_SELF) {
            je_handle_status s = je_handle_dup_self(t, (je_handle)f->rdi,
                                                    JE_HANDLE_OBJ_SELF, new_rights, &nh);
            f->rax = (uint64_t)handle_status_to_abi(s);
        } else if (dispatch->dup == JE_DUP_POLICY_ENDPOINT) {
            je_vm *vm; uint32_t index;
            if (!current_vm_index(&vm, &index)) {
                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0;
                return je_sched_return_to_user(f);
            }
            je_ep_status s = je_endpoint_kern_dup(vm, index, (je_handle)f->rdi,
                                                  new_rights, &nh);
            f->rax=(uint64_t)ep_status_to_abi(s);
        } else {
            je_proc_pool *pool = process_pool();
            je_proc_id id = JE_PROC_ID_NONE;
            if (!process_id_from_object(pool, object, &id)) {
                syscall_invariant("JeiOS fatal: PROCESS dup invariant\r\n");
            }
            je_status retain = dispatch_retain_process(object);
            if (retain == JE_ERR_BUSY) {
                f->rax=(uint64_t)JE_ABI_H_REFOVERFLOW; f->rdx=0;
                return je_sched_return_to_user(f);
            }
            if (retain != JE_OK) {
                syscall_invariant("JeiOS fatal: PROCESS dup retain invariant\r\n");
            }
            je_handle_status a = je_handle_alloc(t, object, JE_PROCESS_TYPE,
                                                  new_rights, &nh);
            if (a != JE_HANDLE_OK) {
                dispatch_release_process(object);
                f->rax=(uint64_t)handle_status_to_abi(a); f->rdx=0;
                return je_sched_return_to_user(f);
            }
            f->rax=(uint64_t)JE_ABI_OK;
        }
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
    case JE_SYSCALL_CLOCK_TICKS:
        f->rax = (uint64_t)JE_ABI_OK;
        f->rdx = je_sched_ticks();
        return je_sched_return_to_user(f);
    case JE_SYSCALL_PROC_SPAWN: {
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) {
            f->rax = (uint64_t)JE_ABI_INVALID; f->rdx = 0u;
            return je_sched_return_to_user(f);
        }
        uint32_t count = (uint32_t)f->r10;
        void *launcher_object = (void *)0;
        je_handle_status launcher_status = je_handle_lookup(
            &vm->procs[index].htable, (je_handle)f->rdi,
            JE_ABI_OBJ_LAUNCHER, JE_ABI_RIGHT_SPAWN, &launcher_object);
        (void)launcher_object;
        if (launcher_status != JE_HANDLE_OK) {
            f->rax = (uint64_t)handle_status_to_abi(launcher_status);
            f->rdx = 0u;
            return je_sched_return_to_user(f);
        }
        je_abi_spawn_grant grants[JE_ABI_SPAWN_GRANTS_MAX];
        if (count != 0u
            && je_copy_from_user(grants, f->rdx,
                                 (uint64_t)count * sizeof grants[0]) != JE_ABI_OK) {
            f->rax = (uint64_t)JE_ABI_INVALID; f->rdx = 0u;
            return je_sched_return_to_user(f);
        }
        je_handle child = 0u;
        je_abi_status s = je_proc_spawn(
            vm, index, (je_handle)f->rdi, (uint32_t)f->rsi, grants, count,
            JE_ABI_SPAWN_QUOTA_HANDLES(f->r8),
            JE_ABI_SPAWN_QUOTA_EPS(f->r8), &child);
        f->rax = (uint64_t)s;
        f->rdx = (uint64_t)child;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_TRY_SEND: {
        /* A5-4 send: resolve caps (if any) in the sender's table FIRST (read-only
         * -> TRANSFER + attenuation), preflight bytes+caps, copy bytes to stack
         * staging, then pure all-or-nothing enqueue (bytes + cap descriptors +
         * per-cap inflight retain). Cap-array copy fault ranks last (after a clean
         * preflight), per the frozen precedence. */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = (je_handle)f->rdi;
        uint64_t ubuf = f->rsi, len = f->rdx;
        uint8_t ncaps = (uint8_t)(f->r10 & 0xffu);
        uint64_t caparr = f->r8;
        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
        /* Resolve each user cap entry {low32 handle, high32 rights} -> descriptor.
         * Copy the array in first (a fault here is INVALID but only after the main
         * handle is validated by the preflight below — so do the base preflight
         * (len + main handle SEND/state/full) FIRST). */
        je_ep_status bpf = je_endpoint_kern_send_preflight(vm, index, h, (size_t)len);
        if (bpf != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(bpf); f->rdx=0; return je_sched_return_to_user(f); }
        if (ncaps != 0u) {
            uint64_t raw[JE_ABI_MSG_CAPS_MAX];
            je_abi_status cs = je_copy_from_user(raw, caparr,
                                                 (uint64_t)ncaps * sizeof(uint64_t));
            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
            for (uint8_t c = 0; c < ncaps; ++c) {
                je_handle ch = (je_handle)(raw[c] & 0xffffffffu);
                uint32_t crights = (uint32_t)(raw[c] >> 32);
                if (crights & ~(uint32_t)JE_ABI_RIGHTS_KNOWN) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
                je_ep_status rc = je_endpoint_kern_resolve_send_cap(vm, index, ch, crights, &caps[c]);
                if (rc != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(rc); f->rdx=0; return je_sched_return_to_user(f); }
            }
        }
        uint8_t staging[JE_EP_MSG_MAX];
        if (len != 0u) {
            je_abi_status cs = je_copy_from_user(staging, ubuf, len);
            if (cs != JE_ABI_OK) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        }
        je_ep_status s = je_endpoint_kern_send_caps(vm, index, h, staging,
                                                    (size_t)len, caps, ncaps);
        f->rax = (uint64_t)ep_status_to_abi(s); f->rdx = 0;
        return je_sched_return_to_user(f);
    }
    case JE_SYSCALL_TRY_RECV: {
        /* A5-4 recv: PEEK (bytes + cap descriptors, no dequeue) -> stage all live
         * mints in the receiver's table (dead caps -> handle 0, A5-2) -> copy bytes
         * out -> copy handle array out -> INFALLIBLE commit (dequeue + release each
         * cap's inflight). Any transient fault rolls back every staged mint and
         * leaves the message queued. */
        je_vm *vm; uint32_t index;
        if (!current_vm_index(&vm, &index)) { f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f); }
        je_handle h = (je_handle)f->rdi;
        uint64_t ubuf = f->rsi, cap = f->rdx, hbuf = f->r8, hcap = f->r9;
        uint64_t timeout = f->r10;   /* A6-ABI: relative tick duration; 0 = poll */
        /* A6-3: bound + overflow-check a nonzero timeout BEFORE anything else, so
         * the rejection does not depend on queue state (a huge R10 is INVALID
         * whether or not a message happens to be queued). JE_IPC_MAX_WAIT keeps
         * the absolute deadline well below the UINT64_MAX tick the ISR aborts on. */
        uint64_t now = je_sched_ticks();
        if (timeout != 0u && (timeout > JE_IPC_MAX_WAIT || timeout > (UINT64_MAX - now))) {
            f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
        }
        /* A6-ABI entry snapshot: evaluate the end once via the plain try_recv path
         * BEFORE registering any waiter. If it is EMPTY (would block) and a nonzero
         * timeout was given, register a per-end waiter and PARK (constrained
         * restartable syscall); on resume the continuation prologue delivers. A
         * zero timeout, or a non-EMPTY probe, falls through to the normal recv. */
        if (timeout != 0u) {
            je_ep_status probe = je_endpoint_kern_recv_entry_probe(vm, index, h);
            if (probe == JE_EP_EMPTY) {
                uint32_t slot = 0u;
                if (je_sched_current_slot(&slot) != JE_OK) {
                    f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
                }
                je_ep_channel *wrec = (je_ep_channel *)0; uint8_t wside = 0u;
                je_ep_status reg = je_endpoint_kern_wait_register(vm, index, h, slot, &wrec, &wside);
                if (reg == JE_EP_WAITERS_BUSY) {   /* A6-4: slot occupied -> non-blocking EMPTY */
                    f->rax=(uint64_t)JE_ABI_H_EMPTY; f->rdx=0; return je_sched_return_to_user(f);
                }
                if (reg != JE_EP_OK) {
                    f->rax=(uint64_t)ep_status_to_abi(reg); f->rdx=0; return je_sched_return_to_user(f);
                }
                /* Park: saves args + owned rec/side, rewinds int 0x80, switches. */
                return je_sched_block_current(f, wrec, wside, now + timeout);
            }
            /* probe != EMPTY: deliverable / SHORT_BUFFER / PEER_CLOSED / error ->
             * fall through to the normal recv below (which re-derives the exact
             * result and, for a deliverable message, does the full mint/copy). */
        }
        uint8_t staging[JE_EP_MSG_MAX];
        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
        size_t mlen = 0u; uint8_t mcaps = 0u;
        je_ep_recv_lease lease;
        je_ep_status s = je_endpoint_kern_recv_peek_caps(vm, index, h, staging,
                                                         (size_t)cap, &mlen,
                                                         (uint8_t)hcap, caps, &mcaps,
                                                         &lease);
        if (s == JE_EP_SHORT_BUFFER) {
            f->rax=(uint64_t)JE_ABI_H_SHORT_BUFFER;
            f->rdx=(uint64_t)mlen | ((uint64_t)mcaps << 32);
            return je_sched_return_to_user(f);
        }
        if (s != JE_EP_OK) { f->rax=(uint64_t)ep_status_to_abi(s); f->rdx=0; return je_sched_return_to_user(f); }
        /* Stage mints: a live cap mints a fresh handle; a dead cap stages 0 (A5-2).
         * Roll back all minted-so-far on any transient failure (QUOTA/NOSLOTS/
         * REFOVERFLOW). */
        je_handle minted[JE_ABI_MSG_CAPS_MAX];
        bool was_live[JE_ABI_MSG_CAPS_MAX];
        for (uint8_t c = 0; c < mcaps; ++c) { minted[c] = 0u; was_live[c] = false; }
        je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
        for (uint8_t c = 0; c < mcaps; ++c) {
            if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c] = 0u; continue; }
            je_handle nh = 0u;
            je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
            if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
            minted[c] = nh; was_live[c] = true; done = (uint8_t)(c + 1u);
        }
        if (fail != JE_ABI_OK) {
            for (uint8_t c = 0; c < done; ++c)
                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
            f->rax=(uint64_t)fail; f->rdx=0; return je_sched_return_to_user(f);
        }
        /* Copy bytes out. */
        if (mlen != 0u) {
            je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
            if (cs != JE_ABI_OK) {
                for (uint8_t c = 0; c < mcaps; ++c)
                    if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
            }
        }
        /* Copy minted handle array out (u32 each). */
        if (mcaps != 0u) {
            uint32_t hout[JE_ABI_MSG_CAPS_MAX];
            for (uint8_t c = 0; c < mcaps; ++c) hout[c] = (uint32_t)minted[c];
            je_abi_status cs = je_copy_to_user(hbuf, hout,
                                               (uint64_t)mcaps * sizeof(uint32_t));
            if (cs != JE_ABI_OK) {
                for (uint8_t c = 0; c < mcaps; ++c)
                    if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
                f->rax=(uint64_t)JE_ABI_INVALID; f->rdx=0; return je_sched_return_to_user(f);
            }
        }
        /* Infallible commit: dequeue + release each cap's inflight ref. */
        je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
        if (cm != JE_EP_OK) {
            je_arch_console_write("JeiOS fatal: recv commit mismatch\r\n");
            je_arch_halt();
        }
#if JE_TEST_MODE == 13
        for (uint8_t c = 0u; c < mcaps; ++c)
            if (was_live[c] && minted[c])
                je_m4g_record_cap_delivery(vm, index, &caps[c]);
        je_m4g_record_control_receive(vm, index, &lease, staging, mlen, mcaps);
#endif
#if JE_TEST_MODE == 15
        for (uint8_t c = 0u; c < mcaps; ++c)
            if (was_live[c] && minted[c])
                je_m4g_record_cap_delivery(vm, index, &caps[c]);
        je_m4i_record_control_receive(vm, index, staging, mlen, mcaps);
#endif
        f->rax = (uint64_t)JE_ABI_OK;
        f->rdx = (uint64_t)mlen | ((uint64_t)mcaps << 32);
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
