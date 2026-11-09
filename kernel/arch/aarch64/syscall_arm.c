/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP3b ARM-local syscall glue (ADR 0016 D2).
 *
 * This TU is intentionally separate from kernel/core/syscall.c.  The shared
 * decoder is compiled on ARM with JE_SYSCALL_HOSTED_TEST, which excludes the
 * x86 trap-frame dispatcher and its endpoint/device lifetime dependencies.
 * ARM MP3b exposes only the resource-free SELF object plus COPY_PROBE; every
 * other object type fails closed as WRONGTYPE.
 */
#include <stdint.h>

#include <jeios/abi.h>
#include <jeios/handle.h>
#include <jeios/sched.h>
#include <jeios/status.h>
#include <jeios/syscall.h>
#include <jeios/usercopy.h>
#include <jeios/vm.h>

static je_abi_status arm_handle_status(je_handle_status status)
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

static je_handle_table *arm_current_htable(void)
{
    je_vm *vm = NULL;
    uint32_t index = UINT32_MAX;

    if (je_sched_current_user(&vm, &index) != JE_OK ||
        vm == NULL || index >= JE_VM_MAX_PROCESSES)
        return NULL;
    return &vm->procs[index].htable;
}

static je_abi_status arm_resolve_self(je_handle_table *table, je_handle handle,
                                      uint32_t *out_rights)
{
    uint32_t type = 0u;
    uint32_t rights = 0u;
    void *object = NULL;
    je_handle_status status;

    status = je_handle_resolve_any(table, handle, &type, &rights, &object);
    (void)object;
    if (status != JE_HANDLE_OK) return arm_handle_status(status);
    if (type != JE_ABI_OBJ_SELF) return JE_ABI_H_WRONGTYPE;
    if (out_rights) *out_rights = rights;
    return JE_ABI_OK;
}

/*
 * Decode and dispatch one ARM SVC.  The caller writes the returned status and
 * scalar result to x0/x1 and separately handles the scheduler actions YIELD and
 * EXIT.  No user pointer is retained; COPY_PROBE reaches the checked portable
 * copy core synchronously while the current SVC owner/root are live.
 */
je_syscall_action je_arm_syscall_dispatch(uint64_t op, const uint64_t args[6],
                                           je_abi_status *out_status,
                                           uint64_t *out_result)
{
    const je_syscall_action action =
        je_syscall_decode(op, args, out_status, out_result);
    je_handle_table *table;

    if (action == JE_SYSCALL_RETURN || action == JE_SYSCALL_YIELD ||
        action == JE_SYSCALL_EXIT)
        return action;
    if (*out_status != JE_ABI_OK)
        return JE_SYSCALL_RETURN;

    switch (action) {
    case JE_SYSCALL_COPY_PROBE:
        *out_status = je_copy_probe(args[0], args[1], args[2]);
        *out_result = 0u;
        return JE_SYSCALL_RETURN;

    case JE_SYSCALL_HANDLE_SELF: {
        je_handle handle = 0u;
        table = arm_current_htable();
        if (!table) {
            *out_status = JE_ABI_INVALID;
            *out_result = 0u;
            return JE_SYSCALL_RETURN;
        }
        const je_handle_status status = je_handle_alloc(
            table, NULL, JE_HANDLE_OBJ_SELF, JE_ABI_RIGHT_INSPECT, &handle);
        *out_status = arm_handle_status(status);
        *out_result = (uint64_t)handle;
        return JE_SYSCALL_RETURN;
    }

    case JE_SYSCALL_HANDLE_CLOSE: {
        uint32_t rights = 0u;
        table = arm_current_htable();
        if (!table) {
            *out_status = JE_ABI_INVALID;
            *out_result = 0u;
            return JE_SYSCALL_RETURN;
        }
        const je_abi_status resolved =
            arm_resolve_self(table, (je_handle)args[0], &rights);
        (void)rights;
        if (resolved != JE_ABI_OK) {
            *out_status = resolved;
        } else {
            *out_status = arm_handle_status(
                je_handle_close(table, (je_handle)args[0]));
        }
        *out_result = 0u;
        return JE_SYSCALL_RETURN;
    }

    case JE_SYSCALL_HANDLE_QUERY: {
        uint32_t rights = 0u;
        table = arm_current_htable();
        if (!table) {
            *out_status = JE_ABI_INVALID;
            *out_result = 0u;
            return JE_SYSCALL_RETURN;
        }
        const je_abi_status resolved =
            arm_resolve_self(table, (je_handle)args[0], &rights);
        (void)rights;
        *out_status = resolved;
        *out_result = resolved == JE_ABI_OK ? JE_ABI_OBJ_SELF : 0u;
        return JE_SYSCALL_RETURN;
    }

    case JE_SYSCALL_HANDLE_RIGHTS: {
        uint32_t rights = 0u;
        table = arm_current_htable();
        if (!table) {
            *out_status = JE_ABI_INVALID;
            *out_result = 0u;
            return JE_SYSCALL_RETURN;
        }
        const je_abi_status resolved =
            arm_resolve_self(table, (je_handle)args[0], &rights);
        if (resolved != JE_ABI_OK) {
            *out_status = resolved;
            *out_result = 0u;
        } else if ((rights & JE_ABI_RIGHT_INSPECT) == 0u) {
            *out_status = JE_ABI_H_DENIED;
            *out_result = 0u;
        } else {
            *out_status = JE_ABI_OK;
            *out_result = (uint64_t)(rights & JE_ABI_RIGHTS_KNOWN);
        }
        return JE_SYSCALL_RETURN;
    }

    case JE_SYSCALL_HANDLE_DUP: {
        je_handle duplicate = 0u;
        uint32_t rights = 0u;
        table = arm_current_htable();
        if (!table) {
            *out_status = JE_ABI_INVALID;
            *out_result = 0u;
            return JE_SYSCALL_RETURN;
        }
        const je_abi_status resolved =
            arm_resolve_self(table, (je_handle)args[0], &rights);
        if (resolved != JE_ABI_OK) {
            *out_status = resolved;
            *out_result = 0u;
            return JE_SYSCALL_RETURN;
        }
        const je_handle_status status = je_handle_dup_self(
            table, (je_handle)args[0], JE_HANDLE_OBJ_SELF,
            (uint32_t)args[1], &duplicate);
        *out_status = arm_handle_status(status);
        *out_result = (uint64_t)duplicate;
        return JE_SYSCALL_RETURN;
    }

    default:
        *out_status = JE_ABI_UNSUPPORTED;
        *out_result = 0u;
        return JE_SYSCALL_RETURN;
    }
}
