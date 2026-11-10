/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP3c AArch64 capability/endpoint syscall glue.
 *
 * This adapter is kept separate from the earlier MP3b SELF-only file while
 * the shared dispatcher is being migrated.  It exposes no PROCESS or DEVICE
 * authority and uses the frame-neutral endpoint kernel bridge.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jeios/abi.h>
#include <jeios/arch.h>
#include <jeios/endpoint_kern.h>
#include <jeios/handle.h>
#include <jeios/sched.h>
#include <jeios/syscall.h>
#include <jeios/usercopy.h>
#include <jeios/vm.h>

/* Test-only evidence callbacks. They are invoked from the real endpoint
 * commit/release path in syscall_glue.inc; they are not ring-3 entry points. */
#ifdef JE_ARM_EL0_TEST
extern void je_arm_el0_test_note_wait_terminal(uint32_t index,
                                               je_ep_channel *rec,
                                               uint8_t side,
                                               je_ep_wait_cause cause);
extern void je_arm_el0_test_note_short_buffer(uint32_t index,
                                              je_ep_channel *rec,
                                              uint8_t side,
                                              size_t required_bytes,
                                              uint8_t required_caps);
extern void je_arm_el0_test_note_recv_caps(uint32_t index,
                                           je_ep_channel *rec,
                                           uint8_t side,
                                           uint8_t count,
                                           const je_ep_capdesc *caps,
                                           const je_handle *minted,
                                           const bool *was_live);
#endif

static je_abi_status arm_ipc_handle_status(je_handle_status status)
{
    switch (status) {
    case JE_HANDLE_OK:        return JE_ABI_OK;
    case JE_HANDLE_STALE:     return JE_ABI_H_STALE;
    case JE_HANDLE_BADINDEX:  return JE_ABI_H_BADINDEX;
    case JE_HANDLE_WRONGTYPE: return JE_ABI_H_WRONGTYPE;
    case JE_HANDLE_DENIED:    return JE_ABI_H_DENIED;
    case JE_HANDLE_QUOTA:     return JE_ABI_H_QUOTA;
    case JE_HANDLE_NOSLOTS:   return JE_ABI_H_NOSLOTS;
    }
    return JE_ABI_INVALID;
}

static je_abi_status arm_ipc_ep_status(je_ep_status status)
{
    switch (status) {
    case JE_EP_OK:            return JE_ABI_OK;
    case JE_EP_BADINDEX:      return JE_ABI_H_BADINDEX;
    case JE_EP_STALE:         return JE_ABI_H_STALE;
    case JE_EP_WRONGTYPE:     return JE_ABI_H_WRONGTYPE;
    case JE_EP_DENIED:        return JE_ABI_H_DENIED;
    case JE_EP_QUOTA:         return JE_ABI_H_QUOTA;
    case JE_EP_NOSLOTS:       return JE_ABI_H_NOSLOTS;
    case JE_EP_REFOVERFLOW:   return JE_ABI_H_REFOVERFLOW;
    case JE_EP_QUEUE_FULL:    return JE_ABI_H_QUEUE_FULL;
    case JE_EP_SHORT_BUFFER:  return JE_ABI_H_SHORT_BUFFER;
    case JE_EP_PEER_CLOSED:   return JE_ABI_H_PEER_CLOSED;
    case JE_EP_EMPTY:         return JE_ABI_H_EMPTY;
    case JE_EP_WAITERS_BUSY:  return JE_ABI_H_EMPTY;
    case JE_EP_INVALID:       return JE_ABI_INVALID;
    case JE_EP_UNWIRED:       return JE_ABI_UNSUPPORTED;
    case JE_EP_CORRUPT:
        je_arch_console_write("JeiOS fatal: endpoint internal corruption\r\n");
        je_arch_halt();
    }
    return JE_ABI_INVALID;
}

static bool arm_ipc_current(je_vm **out_vm, uint32_t *out_index)
{
    return je_sched_current_user(out_vm, out_index) == JE_OK;
}

static je_handle_table *arm_ipc_table(void)
{
    je_vm *vm = NULL;
    uint32_t index = UINT32_MAX;
    if (!arm_ipc_current(&vm, &index)) return NULL;
    return &vm->procs[index].htable;
}

static size_t arm_ipc_staging_cap(uint64_t user_cap, size_t staging_size)
{
    return user_cap > (uint64_t)staging_size
               ? staging_size : (size_t)user_cap;
}

#define JE_G_IPC_FATAL(text) do { \
    je_arch_console_write("JeiOS/aarch64: FATAL IPC "); \
    je_arch_console_write(text); \
    je_arch_console_write("\r\n"); \
    je_arch_halt(); \
} while (0)

#define current_vm_index arm_ipc_current
#define current_htable arm_ipc_table
#define handle_status_to_abi arm_ipc_handle_status
#define ep_status_to_abi arm_ipc_ep_status
#define je_syscall_recv_staging_cap arm_ipc_staging_cap
#define JE_G_ARG_0 args[0]
#define JE_G_ARG_1 args[1]
#define JE_G_ARG_2 args[2]
#define JE_G_ARG_3 args[3]
#define JE_G_ARG_4 args[4]
#define JE_G_ARG_5 args[5]
#define JE_G_ARG(i) JE_G_ARG_##i
#define JE_G_SET_STATUS(v) *out_status = (v)
#define JE_G_SET_RESULT(v) *out_result = (uint64_t)(v)
#define JE_G_RETURN() return JE_SYSCALL_RETURN
#define JE_G_BLOCK(rec, side, deadline) do { \
    if (block_rec) *block_rec = (rec); \
    if (block_side) *block_side = (side); \
    if (block_deadline) *block_deadline = (deadline); \
    return JE_SYSCALL_BLOCK; \
} while (0)
#define JE_G_FINISH_FN je_arm_finish_blocked_recv
#define JE_G_FINISH_STORAGE
#define JE_G_FINISH_PARAMS \
    je_ep_channel *rec, uint8_t side, uint64_t ubuf, uint64_t cap, \
    uint64_t hbuf, uint64_t hcap, je_abi_status *out_status, \
    uint64_t *out_result
#ifdef JE_ARM_EL0_TEST
#define JE_G_NOTE_WAIT_TERMINAL(index, rec, side, cause) \
    do { je_arm_el0_test_note_wait_terminal((index), (rec), (side), (cause)); } while (0)
#define JE_G_NOTE_SHORT_BUFFER(index, rec, side, bytes, caps) \
    do { je_arm_el0_test_note_short_buffer((index), (rec), (side), (bytes), (caps)); } while (0)
#define JE_G_NOTE_RECV_CAPS(index, rec, side, count, caps, minted, was_live) \
    do { je_arm_el0_test_note_recv_caps((index), (rec), (side), (count), \
                                         (caps), (minted), (was_live)); } while (0)
#else
#define JE_G_NOTE_WAIT_TERMINAL(index, rec, side, cause) do { } while (0)
#define JE_G_NOTE_SHORT_BUFFER(index, rec, side, bytes, caps) do { } while (0)
#define JE_G_NOTE_RECV_CAPS(index, rec, side, count, caps, minted, was_live) \
    do { } while (0)
#endif
#if defined(JE_ARM_IPC_FAULT) && JE_ARM_IPC_FAULT == 6
#define JE_G_FAULT_RESUMED_COPY() \
    JE_G_IPC_FATAL("copy fault in resumed continuation")
#else
#define JE_G_FAULT_RESUMED_COPY() do { } while (0)
#endif
#if defined(JE_ARM_IPC_FAULT) && JE_ARM_IPC_FAULT == 5
#define JE_G_FAULT_RECV_COMMIT(lease) \
    do { (lease).len = (uint8_t)((lease).len + 1u); } while (0)
#else
#define JE_G_FAULT_RECV_COMMIT(lease) do { } while (0)
#endif
#define JE_G_CLOSE_EXTRA(t, h, type, rights, object) \
        } else if ((type) == JE_ABI_OBJ_SELF || (type) == JE_ABI_OBJ_LAUNCHER) { \
            je_handle_status s = je_handle_close((t), (h)); \
            JE_G_SET_STATUS(handle_status_to_abi(s)); \
            JE_G_SET_RESULT(0);
#define JE_G_CLOSE_FALLBACK() JE_G_SET_STATUS(JE_ABI_UNSUPPORTED)
#define JE_G_QUERY_RESULT(s, type, rights, object) do { \
    (void)object; \
    if ((s) != JE_HANDLE_OK) { \
        JE_G_SET_STATUS(handle_status_to_abi(s)); \
        JE_G_SET_RESULT(0); \
    } else if ((type) == JE_ABI_OBJ_PROCESS \
               || (type) == JE_ABI_OBJ_DEVICE) { \
        JE_G_SET_STATUS(JE_ABI_UNSUPPORTED); \
        JE_G_SET_RESULT(0); \
    } else { \
        JE_G_SET_STATUS(JE_ABI_OK); \
        JE_G_SET_RESULT((uint64_t)(type)); \
    } \
} while (0)
#define JE_G_RIGHTS_RESULT(s, type, rights, object) do { \
    (void)object; \
    if ((s) != JE_HANDLE_OK) { \
        JE_G_SET_STATUS(handle_status_to_abi(s)); \
        JE_G_SET_RESULT(0); \
    } else if ((type) == JE_ABI_OBJ_PROCESS \
               || (type) == JE_ABI_OBJ_DEVICE) { \
        JE_G_SET_STATUS(JE_ABI_UNSUPPORTED); \
        JE_G_SET_RESULT(0); \
    } else if (((rights) & JE_ABI_RIGHT_INSPECT) == 0u) { \
        JE_G_SET_STATUS(JE_ABI_H_DENIED); \
        JE_G_SET_RESULT(0); \
    } else { \
        JE_G_SET_STATUS(JE_ABI_OK); \
        JE_G_SET_RESULT((uint64_t)(rights)); \
    } \
} while (0)
#define JE_G_DUP_PREPARE(resolve, type)
#define JE_G_DUP_RESOLVED(t, type, source_rights, object, new_rights, nh) do { \
    if ((type) == JE_ABI_OBJ_SELF) { \
        je_handle_status s = je_handle_dup_self( \
            (t), (je_handle)JE_G_ARG(0), JE_HANDLE_OBJ_SELF, \
            (new_rights), &(nh)); \
        JE_G_SET_STATUS(handle_status_to_abi(s)); \
    } else if ((type) == JE_ABI_OBJ_ENDPOINT) { \
        je_vm *vm; \
        uint32_t index; \
        if (!current_vm_index(&vm, &index)) { \
            JE_G_SET_STATUS(JE_ABI_INVALID); \
            JE_G_SET_RESULT(0); \
            JE_G_RETURN(); \
        } \
        je_ep_status s = je_endpoint_kern_dup( \
            vm, index, (je_handle)JE_G_ARG(0), (new_rights), &(nh)); \
        JE_G_SET_STATUS(ep_status_to_abi(s)); \
    } else { \
        JE_G_SET_STATUS(JE_ABI_UNSUPPORTED); \
    } \
} while (0)
#define JE_G_EMIT_FINISH
#include "../../core/syscall_glue.inc"
#undef JE_G_EMIT_FINISH

je_syscall_action je_arm_syscall_dispatch(
    uint64_t op, const uint64_t args[6], je_abi_status *out_status,
    uint64_t *out_result, je_ep_channel **block_rec, uint8_t *block_side,
    uint64_t *block_deadline)
{
    je_syscall_action action =
        je_syscall_decode(op, args, out_status, out_result);
    je_handle_table *table;
    if (block_rec) *block_rec = NULL;
    if (block_side) *block_side = 0u;
    if (block_deadline) *block_deadline = 0u;
    if (action == JE_SYSCALL_RETURN || action == JE_SYSCALL_YIELD ||
        action == JE_SYSCALL_EXIT)
        return action;
    if (*out_status != JE_ABI_OK) return JE_SYSCALL_RETURN;

    switch (action) {
    case JE_SYSCALL_COPY_PROBE:
        *out_status = je_copy_probe(args[0], args[1], args[2]);
        *out_result = 0u;
        return JE_SYSCALL_RETURN;

    case JE_SYSCALL_HANDLE_SELF: {
        je_handle handle = 0u;
        table = arm_ipc_table();
        if (!table) {
            *out_status = JE_ABI_INVALID;
            return JE_SYSCALL_RETURN;
        }
        *out_status = arm_ipc_handle_status(je_handle_alloc(
            table, NULL, JE_HANDLE_OBJ_SELF, JE_ABI_RIGHT_INSPECT, &handle));
        *out_result = (uint64_t)handle;
        return JE_SYSCALL_RETURN;
    }
#define JE_G_EMIT_CASES_EARLY
#include "../../core/syscall_glue.inc"
#undef JE_G_EMIT_CASES_EARLY
    case JE_SYSCALL_CLOCK_TICKS:
        *out_status = JE_ABI_OK;
        *out_result = je_sched_ticks();
        return JE_SYSCALL_RETURN;
#define JE_G_EMIT_CASES_LATE
#include "../../core/syscall_glue.inc"
#undef JE_G_EMIT_CASES_LATE
    default:
        *out_status = JE_ABI_UNSUPPORTED;
        *out_result = 0u;
        return JE_SYSCALL_RETURN;
    }
}
