/* ADR 0010 D5 journaled spawn. Pre-wiring STUB (lead): fails closed until PR-C. */
#include <jeios/spawn.h>
je_abi_status je_proc_spawn(je_vm *vm, uint32_t caller_idx, je_handle launcher,
                            uint32_t image_id, const je_abi_spawn_grant *grants,
                            uint32_t n, uint32_t handle_quota, uint32_t ep_quota,
                            je_handle *out)
{
    (void)vm; (void)caller_idx; (void)launcher; (void)image_id; (void)grants; (void)n;
    (void)handle_quota; (void)ep_quota;
    if (out) *out = 0u;
    return JE_ABI_UNSUPPORTED;
}
void je_spawn_failpoint_set(uint32_t step, uint32_t grant_index) { (void)step; (void)grant_index; }
