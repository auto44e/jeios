#include <jeios/spawn.h>

#include <jeios/arch.h>
#include <jeios/endpoint.h>
#include <jeios/endpoint_kern.h>
#include <jeios/image.h>
#include <jeios/proc.h>
#include <jeios/sched.h>

extern je_status je_sched_register_process(je_vm *vm, uint32_t index,
                                            je_proc_id id);
extern je_status je_sched_unregister_process(je_vm *vm, uint32_t index);
extern je_status je_sched_drop_user(je_vm *vm, uint32_t index);
extern je_status je_process_handle_mint(je_vm *vm, uint32_t index,
                                        je_proc_id id, uint32_t rights,
                                        je_handle *out,
                                        je_handle_status *out_alloc_status);
extern je_proc_pool *je_sched_proc_pool(void);
extern _Noreturn void syscall_invariant(const char *message);

typedef struct {
    uint32_t step;
    uint32_t grant_index;
} je_spawn_failpoint;

static je_spawn_failpoint spawn_failpoint;

static je_abi_status spawn_finish(je_abi_status status)
{
    spawn_failpoint = (je_spawn_failpoint){0};
    return status;
}

static bool spawn_failpoint_hit(uint32_t step, uint32_t grant_index)
{
    if (spawn_failpoint.step != step
        || (step == 3u && spawn_failpoint.grant_index != grant_index))
        return false;
    spawn_failpoint = (je_spawn_failpoint){0};
    return true;
}

void je_spawn_failpoint_set(uint32_t step, uint32_t grant_index)
{
    if (step == 0u || step > 5u || grant_index >= JE_ABI_SPAWN_GRANTS_MAX) {
        spawn_failpoint = (je_spawn_failpoint){0};
    } else {
        spawn_failpoint = (je_spawn_failpoint){step, grant_index};
    }
}

static je_abi_status endpoint_status_to_abi(je_ep_status status)
{
    switch (status) {
    case JE_EP_OK:          return JE_ABI_OK;
    case JE_EP_BADINDEX:    return JE_ABI_H_BADINDEX;
    case JE_EP_STALE:       return JE_ABI_H_STALE;
    case JE_EP_WRONGTYPE:   return JE_ABI_H_WRONGTYPE;
    case JE_EP_DENIED:      return JE_ABI_H_DENIED;
    case JE_EP_QUOTA:       return JE_ABI_H_QUOTA;
    case JE_EP_NOSLOTS:     return JE_ABI_H_NOSLOTS;
    case JE_EP_REFOVERFLOW: return JE_ABI_H_REFOVERFLOW;
    case JE_EP_INVALID:     return JE_ABI_INVALID;
    default:
        syscall_invariant("JeiOS fatal: PROC_SPAWN endpoint status invariant\r\n");
    }
}

static je_abi_status internal_status_to_abi(je_status status)
{
    switch (status) {
    case JE_OK:              return JE_ABI_OK;
    case JE_ERR_UNSUPPORTED: return JE_ABI_UNSUPPORTED;
    case JE_ERR_NOMEM:
    case JE_ERR_BUSY:        return JE_ABI_H_NOSLOTS;
    default:
        syscall_invariant("JeiOS fatal: PROC_SPAWN internal status invariant\r\n");
    }
}

static void spawn_undo(je_vm *vm, uint32_t index, bool registered,
                       bool vm_ready, bool thread_created,
                       je_proc_pool *pool, je_proc_id id)
{
    if (thread_created) {
        if (je_sched_drop_user(vm, index) != JE_OK)
            je_arch_halt();
        if (vm_ready) {
            if (je_vm_destroy(vm, index) != JE_OK)
                je_arch_halt();
        }
        if (je_proc_on_dying(pool, id, JE_DEATH_KILLED, 0u) != JE_OK)
            je_arch_halt();
        if (je_proc_on_reaped(pool, id) != JE_OK)
            je_arch_halt();
        return;
    } else if (registered) {
        if (je_sched_unregister_process(vm, index) != JE_OK)
            je_arch_halt();
    }
    if (vm_ready) {
        if (je_vm_destroy(vm, index) != JE_OK)
            je_arch_halt();
    }
    if (je_proc_abandon(pool, id) != JE_OK)
        je_arch_halt();
}

je_abi_status je_proc_spawn(je_vm *vm, uint32_t caller_idx, je_handle launcher,
                            uint32_t image_id, const je_abi_spawn_grant *grants,
                            uint32_t n, uint32_t handle_quota, uint32_t ep_quota,
                            je_handle *out)
{
    if (out) *out = 0u;
    if (!out || !vm || caller_idx >= JE_VM_MAX_PROCESSES
        || n > JE_ABI_SPAWN_GRANTS_MAX)
        return spawn_finish(JE_ABI_H_BADINDEX);

    je_handle_table *caller_table = &vm->procs[caller_idx].htable;
    void *launcher_object = (void *)0;
    je_handle_status launcher_status = je_handle_lookup(
        caller_table, launcher, JE_ABI_OBJ_LAUNCHER, JE_ABI_RIGHT_SPAWN,
        &launcher_object);
    (void)launcher_object;
    if (launcher_status != JE_HANDLE_OK) {
        switch (launcher_status) {
        case JE_HANDLE_BADINDEX:  return spawn_finish(JE_ABI_H_BADINDEX);
        case JE_HANDLE_STALE:     return spawn_finish(JE_ABI_H_STALE);
        case JE_HANDLE_WRONGTYPE: return spawn_finish(JE_ABI_H_WRONGTYPE);
        case JE_HANDLE_DENIED:    return spawn_finish(JE_ABI_H_DENIED);
        default:
            syscall_invariant("JeiOS fatal: PROC_SPAWN launcher status invariant\r\n");
        }
    }

    const je_builtin_image *image = je_builtin_image_get(image_id);
    if (!image) return spawn_finish(JE_ABI_INVALID);
    if (handle_quota == 0u) handle_quota = JE_VM_HANDLE_QUOTA_DEFAULT;
    if (handle_quota > JE_VM_HANDLE_QUOTA_MAX || ep_quota > JE_EP_CHILD_QUOTA_MAX)
        return spawn_finish(JE_ABI_INVALID);
    if (n > handle_quota) return spawn_finish(JE_ABI_INVALID);
    if (n != 0u && !grants) return spawn_finish(JE_ABI_INVALID);

    je_ep_capdesc descriptors[JE_ABI_SPAWN_GRANTS_MAX];
    for (uint32_t i = 0u; i < n; ++i) {
        if (grants[i].rights & ~(uint32_t)JE_ABI_RIGHTS_KNOWN)
            return spawn_finish(JE_ABI_INVALID);
        je_ep_status resolved = je_endpoint_kern_resolve_send_cap(
            vm, caller_idx, (je_handle)grants[i].handle, grants[i].rights,
            &descriptors[i]);
        if (resolved != JE_EP_OK)
            return spawn_finish(endpoint_status_to_abi(resolved));
    }

    je_handle_status caller_preflight = je_handle_can_alloc(caller_table);
    if (caller_preflight != JE_HANDLE_OK) {
        switch (caller_preflight) {
        case JE_HANDLE_QUOTA:   return spawn_finish(JE_ABI_H_QUOTA);
        case JE_HANDLE_NOSLOTS: return spawn_finish(JE_ABI_H_NOSLOTS);
        default:
            syscall_invariant("JeiOS fatal: PROC_SPAWN caller quota invariant\r\n");
        }
    }

    if (spawn_failpoint_hit(1u, 0u))
        return spawn_finish(JE_ABI_H_NOSLOTS);
    je_proc_pool *pool = je_sched_proc_pool();
    je_proc_id id = JE_PROC_ID_NONE;
    je_status status = je_proc_alloc(pool, ep_quota, false, &id);
    if (status != JE_OK) return spawn_finish(internal_status_to_abi(status));

    bool registered = false;
    bool vm_ready = false;
    bool thread_created = false;
    uint32_t child_index = 0u;
    if (spawn_failpoint_hit(2u, 0u)) {
        spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
        return spawn_finish(JE_ABI_H_NOSLOTS);
    }
    status = je_vm_build_quota(vm, &image->desc, image->blob, image->blob_len,
                               je_arch_has_nx(), handle_quota, &child_index);
    if (status != JE_OK) {
        spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
        return spawn_finish(internal_status_to_abi(status));
    }
    vm_ready = true;
    status = je_sched_register_process(vm, child_index, id);
    if (status != JE_OK) {
        spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
        return spawn_finish(internal_status_to_abi(status));
    }
    registered = true;

    je_handle minted[JE_ABI_SPAWN_GRANTS_MAX] = {0u, 0u, 0u, 0u};
    for (uint32_t i = 0u; i < n; ++i) {
        if (spawn_failpoint_hit(3u, i)) {
            spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
            return spawn_finish(JE_ABI_H_NOSLOTS);
        }
        je_ep_status mint = je_endpoint_kern_mint_recv_cap(
            vm, child_index, &descriptors[i], &minted[i]);
        if (mint != JE_EP_OK) {
            spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
            return spawn_finish(endpoint_status_to_abi(mint));
        }
    }

    if (spawn_failpoint_hit(4u, 0u)) {
        spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
        return spawn_finish(JE_ABI_H_NOSLOTS);
    }
    je_user_startup startup = {
        JE_ABI_STARTUP_V1, n, minted[0], minted[1], minted[2], minted[3]
    };
    status = je_thread_create_user_start(vm, child_index, &startup,
                                         (uint32_t *)0);
    if (status != JE_OK) {
        spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
        return spawn_finish(internal_status_to_abi(status));
    }
    thread_created = true;

    if (spawn_failpoint_hit(5u, 0u)) {
        spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
        return spawn_finish(JE_ABI_H_NOSLOTS);
    }
    je_handle process_handle = 0u;
    je_handle_status process_handle_alloc = JE_HANDLE_OK;
    status = je_process_handle_mint(
        vm, caller_idx, id, JE_ABI_RIGHT_INSPECT | JE_ABI_RIGHT_KILL,
        &process_handle, &process_handle_alloc);
    if (status != JE_OK) {
        spawn_undo(vm, child_index, registered, vm_ready, thread_created, pool, id);
        if (process_handle_alloc == JE_HANDLE_QUOTA)
            return spawn_finish(JE_ABI_H_QUOTA);
        return spawn_finish(internal_status_to_abi(status));
    }
    *out = process_handle;
    return spawn_finish(JE_ABI_OK);
}
