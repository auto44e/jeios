/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADR 0010 D4 PROCESS records. This file is independent of the scheduler and
 * VM: the kernel supplies one static pool, while hosted tests supply their own.
 * Kernel callers serialize operations with IF=0; this pure API allocates nothing.
 */
#include <jeios/proc.h>

static je_proc_rec *record_for(je_proc_pool *pool, je_proc_id id)
{
    if (!pool || id.gen == 0u || id.index >= JE_PROC_REC_MAX) return (je_proc_rec *)0;
    je_proc_rec *rec = &pool->rec[id.index];
    if (rec->state == JE_PROC_FREE || rec->gen != id.gen) return (je_proc_rec *)0;
    return rec;
}

static const je_proc_rec *record_for_const(const je_proc_pool *pool, je_proc_id id)
{
    if (!pool || id.gen == 0u || id.index >= JE_PROC_REC_MAX)
        return (const je_proc_rec *)0;
    const je_proc_rec *rec = &pool->rec[id.index];
    if (rec->state == JE_PROC_FREE || rec->gen != id.gen)
        return (const je_proc_rec *)0;
    return rec;
}

static void free_record(je_proc_pool *pool, je_proc_rec *rec)
{
    uint32_t gen = rec->gen;
    bool retired = rec->retired;
    *rec = (je_proc_rec){0};
    rec->gen = gen;
    rec->retired = retired;
    if (pool->used_count != 0u) pool->used_count--;
}

void je_proc_pool_init(je_proc_pool *pool)
{
    if (!pool) return;
    pool->used_count = 0u;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i)
        pool->rec[i] = (je_proc_rec){0};
}

je_status je_proc_alloc(je_proc_pool *pool, uint32_t ep_limit, bool supervisor,
                        je_proc_id *out)
{
    if (!pool || !out) return JE_ERR_INVALID;
    *out = JE_PROC_ID_NONE;
    for (uint32_t i = 0u; i < JE_PROC_REC_MAX; ++i) {
        je_proc_rec *rec = &pool->rec[i];
        if (rec->state != JE_PROC_FREE || rec->retired) continue;
        if (rec->gen >= JE_PROC_GEN_MAX) {
            rec->retired = true;
            continue;
        }
        uint32_t generation = rec->gen + 1u;
        *rec = (je_proc_rec){0};
        rec->gen = generation;
        rec->state = JE_PROC_LIVE;
        rec->ep_limit = ep_limit;
        rec->supervisor = supervisor;
        pool->used_count++;
        *out = (je_proc_id){i, generation};
        return JE_OK;
    }
    return JE_ERR_NOMEM;
}

bool je_proc_valid(const je_proc_pool *pool, je_proc_id id)
{
    return record_for_const(pool, id) != (const je_proc_rec *)0;
}

je_status je_proc_abandon(je_proc_pool *pool, je_proc_id id)
{
    je_proc_rec *rec = record_for(pool, id);
    if (!rec || rec->state != JE_PROC_LIVE || rec->run_linked
        || rec->handle_refs != 0u || rec->ep_charge != 0u)
        return JE_ERR_INVALID;
    free_record(pool, rec);
    return JE_OK;
}

je_status je_proc_link(je_proc_pool *pool, je_proc_id id, uint32_t thread_slot,
                       uint32_t vm_index)
{
    je_proc_rec *rec = record_for(pool, id);
    if (!rec || rec->state != JE_PROC_LIVE) return JE_ERR_INVALID;
    rec->run_linked = true;
    rec->thread_slot = thread_slot;
    rec->vm_index = vm_index;
    return JE_OK;
}

je_status je_proc_on_dying(je_proc_pool *pool, je_proc_id id, uint32_t reason,
                           uint64_t code)
{
    je_proc_rec *rec = record_for(pool, id);
    if (!rec) return JE_ERR_INVALID;
    if (rec->state == JE_PROC_DYING || rec->state == JE_PROC_DEAD) return JE_OK;
    if (rec->state != JE_PROC_LIVE) return JE_ERR_INVALID;
    rec->state = JE_PROC_DYING;
    rec->reason = reason;
    rec->code = code;
    return JE_OK;
}

je_status je_proc_on_reaped(je_proc_pool *pool, je_proc_id id)
{
    je_proc_rec *rec = record_for(pool, id);
    if (!rec || rec->state != JE_PROC_DYING) return JE_ERR_INVALID;
    rec->state = JE_PROC_DEAD;
    rec->run_linked = false;
    rec->thread_slot = 0u;
    rec->vm_index = 0u;
    if (rec->handle_refs == 0u) free_record(pool, rec);
    return JE_OK;
}

je_status je_proc_retain(je_proc_pool *pool, je_proc_id id)
{
    je_proc_rec *rec = record_for(pool, id);
    if (!rec) return JE_ERR_INVALID;
    if (rec->handle_refs >= JE_PROC_REF_MAX) return JE_ERR_BUSY;
    rec->handle_refs++;
    return JE_OK;
}

void je_proc_release(je_proc_pool *pool, je_proc_id id)
{
    je_proc_rec *rec = record_for(pool, id);
    if (!rec || rec->handle_refs == 0u) return;
    rec->handle_refs--;
    if (rec->state == JE_PROC_DEAD && rec->handle_refs == 0u) free_record(pool, rec);
}

je_status je_proc_status(const je_proc_pool *pool, je_proc_id id, uint32_t *state,
                         uint32_t *reason, uint64_t *code)
{
    const je_proc_rec *rec = record_for_const(pool, id);
    if (!rec || !state || !reason || !code) return JE_ERR_INVALID;
    *state = rec->state;
    *reason = rec->reason;
    *code = rec->code;
    return JE_OK;
}

je_status je_proc_charge(je_proc_pool *pool, je_proc_id id)
{
    if (id.gen == 0u) return JE_OK;
    je_proc_rec *rec = record_for(pool, id);
    if (!rec) return JE_ERR_INVALID;
    if (rec->ep_charge >= rec->ep_limit) return JE_ERR_DENIED;
    rec->ep_charge++;
    return JE_OK;
}

void je_proc_uncharge(je_proc_pool *pool, je_proc_id id)
{
    je_proc_rec *rec = record_for(pool, id);
    if (!rec || rec->ep_charge == 0u) return;
    rec->ep_charge--;
}
