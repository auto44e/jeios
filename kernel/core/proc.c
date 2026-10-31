/* ADR 0010 D4 PROCESS records. Pre-wiring STUB (lead): fail closed until the PR-B
 * implementation lands. Contract: include/jeios/proc.h. */
#include <jeios/proc.h>

void je_proc_pool_init(je_proc_pool *pool) { (void)pool; }
je_status je_proc_alloc(je_proc_pool *pool, uint32_t ep_limit, bool supervisor, je_proc_id *out)
{ (void)pool; (void)ep_limit; (void)supervisor; if (out) *out = JE_PROC_ID_NONE; return JE_ERR_NOMEM; }
bool je_proc_valid(const je_proc_pool *pool, je_proc_id id) { (void)pool; (void)id; return false; }
je_status je_proc_abandon(je_proc_pool *pool, je_proc_id id) { (void)pool; (void)id; return JE_ERR_INVALID; }
je_status je_proc_link(je_proc_pool *pool, je_proc_id id, uint32_t thread_slot, uint32_t vm_index)
{ (void)pool; (void)id; (void)thread_slot; (void)vm_index; return JE_ERR_INVALID; }
je_status je_proc_on_dying(je_proc_pool *pool, je_proc_id id, uint32_t reason, uint64_t code)
{ (void)pool; (void)id; (void)reason; (void)code; return JE_ERR_INVALID; }
je_status je_proc_on_reaped(je_proc_pool *pool, je_proc_id id) { (void)pool; (void)id; return JE_ERR_INVALID; }
je_status je_proc_retain(je_proc_pool *pool, je_proc_id id) { (void)pool; (void)id; return JE_ERR_INVALID; }
void je_proc_release(je_proc_pool *pool, je_proc_id id) { (void)pool; (void)id; }
je_status je_proc_status(const je_proc_pool *pool, je_proc_id id, uint32_t *state, uint32_t *reason, uint64_t *code)
{ (void)pool; (void)id; (void)state; (void)reason; (void)code; return JE_ERR_INVALID; }
je_status je_proc_charge(je_proc_pool *pool, je_proc_id id)
{ (void)pool; return (id.gen == 0u) ? JE_OK : JE_ERR_INVALID; }
void je_proc_uncharge(je_proc_pool *pool, je_proc_id id) { (void)pool; (void)id; }
