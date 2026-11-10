/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADR 0013 D1: pure, architecture-neutral round-robin scheduler policy.
 *
 * This file deliberately contains no libc dependency and never interprets a
 * saved frame.  Architecture code supplies all frame/stack knowledge through
 * je_rr_hooks.  Calls are single-CPU/serialized by the caller; this core does
 * not provide SMP locking or interrupt masking.
 */
#include <stddef.h>
#include <jeios/sched_rr.h>

static bool rr_hooks_valid(const je_rr_hooks *hooks)
{
    return hooks != NULL && hooks->frame_init != NULL &&
           hooks->frame_check != NULL && hooks->dispatch_install != NULL &&
           hooks->stack_alloc != NULL && hooks->stack_free != NULL &&
           hooks->idle_wait != NULL;
}

static void rr_counter_inc(uint64_t *value)
{
    if (*value != UINT64_MAX) ++*value;
}

static void rr_clear_ctx(je_rr_ctx *ctx)
{
    *ctx = (je_rr_ctx){0};
}

void je_rr_init(je_rr_table *t, const je_rr_hooks *hooks)
{
    uint32_t slot;

    if (t == NULL) return;
    t->hooks = hooks;
    t->ticks = 0u;
    t->worker_dispatch_serial = 0u;
    t->current = 0u;
    t->cursor = 0u;
    for (slot = 0u; slot < JE_RR_MAX; ++slot) rr_clear_ctx(&t->slots[slot]);

    t->slots[0].state = JE_RR_RUNNING;
    t->slots[0].slot = 0u;
    t->slots[0].owner = 0u;
    t->slots[0].phase = 1u;
    rr_counter_inc(&t->slots[0].counters.starts);
}

je_status je_rr_create(je_rr_table *t, void (*entry)(void *), void *arg,
                       uint32_t *out_slot)
{
    uint32_t slot;
    je_rr_ctx *ctx;
    void *frame = NULL;
    je_status status;

    if (t == NULL || entry == NULL || out_slot == NULL ||
        !rr_hooks_valid(t->hooks))
        return JE_ERR_INVALID;
    for (slot = 1u; slot < JE_RR_MAX; ++slot)
        if (t->slots[slot].state == JE_RR_EMPTY) break;
    if (slot == JE_RR_MAX) return JE_ERR_NOMEM;

    ctx = &t->slots[slot];
    rr_clear_ctx(ctx);
    ctx->slot = slot;
    ctx->owner = slot;
    status = t->hooks->stack_alloc(ctx);
    if (status != JE_OK) {
        rr_clear_ctx(ctx);
        return status;
    }

    status = t->hooks->frame_init(ctx, entry, arg, &frame);
    if (status != JE_OK || frame == NULL) {
        t->hooks->stack_free(ctx);
        rr_clear_ctx(ctx);
        return status == JE_OK ? JE_ERR_INVALID : status;
    }
    ctx->frame = frame;
    ctx->phase = 0u;
    ctx->state = JE_RR_READY;
    *out_slot = slot;
    return JE_OK;
}

uint32_t je_rr_pick(je_rr_table *t)
{
    uint32_t offset;

    if (t == NULL || t->current >= JE_RR_MAX) return 0u;
    for (offset = 1u; offset <= JE_RR_MAX; ++offset) {
        uint32_t candidate = t->cursor + offset;
        if (candidate >= JE_RR_MAX) candidate -= JE_RR_MAX;
        if (candidate != 0u &&
            t->slots[candidate].state == JE_RR_READY) {
            t->cursor = candidate;
            return candidate;
        }
    }
    if (t->hooks != NULL && t->hooks->idle_wait != NULL)
        t->hooks->idle_wait();
    return 0u;
}

void je_rr_tick(je_rr_table *t, uint64_t now)
{
    uint32_t slot;

    if (t == NULL) return;
    rr_counter_inc(&t->ticks);
    for (slot = 1u; slot < JE_RR_MAX; ++slot) {
        je_rr_ctx *ctx = &t->slots[slot];
        if (ctx->state == JE_RR_SLEEPING && now >= ctx->wake) {
            ctx->wake = 0u;
            ctx->state = JE_RR_READY;
        }
    }
    if (t->current < JE_RR_MAX &&
        t->slots[t->current].state == JE_RR_RUNNING)
        rr_counter_inc(&t->slots[t->current].counters.observed_running);
}

je_status je_rr_switch(je_rr_table *t, uint32_t next,
                       void **out_resume_frame)
{
    je_rr_ctx *prev_ctx;
    je_rr_ctx *next_ctx;
    bool prev_was_running;
    je_status status;

    if (out_resume_frame != NULL) *out_resume_frame = NULL;
    if (t == NULL || out_resume_frame == NULL || next >= JE_RR_MAX ||
        t->current >= JE_RR_MAX || !rr_hooks_valid(t->hooks))
        return JE_ERR_INVALID;
    next_ctx = &t->slots[next];
    if (next != 0u && next_ctx->state != JE_RR_READY)
        return JE_ERR_INVALID;

    prev_ctx = &t->slots[t->current];
    prev_was_running = prev_ctx->state == JE_RR_RUNNING;
    if (prev_was_running) prev_ctx->state = JE_RR_READY;

    status = t->hooks->frame_check(next_ctx, next_ctx->frame,
                                   next_ctx->phase);
    if (status != JE_OK) {
        if (prev_was_running) prev_ctx->state = JE_RR_RUNNING;
        return status;
    }
    status = t->hooks->dispatch_install(prev_ctx, next_ctx);
    if (status != JE_OK) {
        if (prev_was_running) prev_ctx->state = JE_RR_RUNNING;
        return status;
    }

    next_ctx->state = JE_RR_RUNNING;
    t->current = next;
    t->cursor = next;
    rr_counter_inc(&next_ctx->counters.dispatched_to);
    if (next_ctx->phase == 0u) {
        rr_counter_inc(&next_ctx->counters.starts);
        next_ctx->phase = 1u;
    } else {
        rr_counter_inc(&next_ctx->counters.resumed);
    }
    if (next != 0u) {
        if (next_ctx->last_worker_dispatch != 0u &&
            t->worker_dispatch_serial > next_ctx->last_worker_dispatch)
            rr_counter_inc(&next_ctx->counters.interleave);
        rr_counter_inc(&t->worker_dispatch_serial);
        next_ctx->last_worker_dispatch = t->worker_dispatch_serial;
    }
    *out_resume_frame = next_ctx->frame;
    return JE_OK;
}

void je_rr_sleep(je_rr_table *t, uint32_t slot, uint64_t now, uint64_t n)
{
    je_rr_ctx *ctx;

    if (t == NULL || slot == 0u || slot >= JE_RR_MAX ||
        slot != t->current || n == 0u || n > JE_RR_SLEEP_MAX ||
        now > UINT64_MAX - n)
        return;
    ctx = &t->slots[slot];
    if (ctx->state != JE_RR_RUNNING) return;

    ctx->wake = now + n;
    ctx->state = JE_RR_SLEEPING;
}

void je_rr_block(je_rr_table *t, uint32_t slot)
{
    je_rr_ctx *ctx;

    if (t == NULL || slot == 0u || slot >= JE_RR_MAX ||
        slot != t->current)
        return;
    ctx = &t->slots[slot];
    if (ctx->state != JE_RR_RUNNING) return;
    ctx->wake = 0u;
    ctx->state = JE_RR_BLOCKED;
}

void je_rr_wake(je_rr_table *t, uint32_t slot)
{
    if (t == NULL || slot == 0u || slot >= JE_RR_MAX) return;
    if (t->slots[slot].state == JE_RR_BLOCKED)
        t->slots[slot].state = JE_RR_READY;
}

void je_rr_exit(je_rr_table *t, uint32_t slot)
{
    je_rr_ctx *ctx;

    if (t == NULL) return;
    if (slot == 0u) {
        __builtin_trap();
    }
    if (slot >= JE_RR_MAX) return;
    ctx = &t->slots[slot];
    if (ctx->state == JE_RR_EMPTY || ctx->state == JE_RR_ZOMBIE) return;
    ctx->wake = 0u;
    ctx->state = JE_RR_ZOMBIE;
}

uint32_t je_rr_current(const je_rr_table *t)
{
    return t == NULL ? 0u : t->current;
}

je_status je_rr_reap(je_rr_table *t, uint32_t slot,
                     uint32_t current_stack_slot)
{
    je_rr_ctx *ctx;

    if (t == NULL || slot == 0u || slot >= JE_RR_MAX ||
        !rr_hooks_valid(t->hooks))
        return JE_ERR_INVALID;
    if (slot == current_stack_slot) return JE_ERR_BUSY;
    ctx = &t->slots[slot];
    if (ctx->state != JE_RR_ZOMBIE) return JE_ERR_INVALID;
    t->hooks->stack_free(ctx);
    rr_clear_ctx(ctx);
    return JE_OK;
}

je_rr_state je_rr_get_state(const je_rr_table *t, uint32_t slot)
{
    if (t == NULL || slot >= JE_RR_MAX) return JE_RR_EMPTY;
    return t->slots[slot].state;
}

const je_rr_counters *je_rr_get_counters(const je_rr_table *t,
                                         uint32_t slot)
{
    if (t == NULL || slot >= JE_RR_MAX) return NULL;
    return &t->slots[slot].counters;
}
