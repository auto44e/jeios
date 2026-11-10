/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_SCHED_RR_H
#define JEIOS_SCHED_RR_H

#include <stdbool.h>
#include <stdint.h>
#include <jeios/status.h>

/*
 * Pure round-robin policy state.  This interface is deliberately independent
 * of an interrupt frame layout, stack allocator, CPU mode, or wait instruction.
 * A caller owns the table storage and must serialize calls (the MP2 caller
 * does so on its single CPU with the appropriate interrupt discipline).
 */

#define JE_RR_MAX 16u
#define JE_RR_SLEEP_MAX UINT64_C(0x100000000)

_Static_assert(JE_RR_MAX >= 2u, "slot zero must leave one worker slot");

typedef enum {
    JE_RR_EMPTY = 0,
    JE_RR_RUNNING,
    JE_RR_READY,
    JE_RR_SLEEPING,
    JE_RR_ZOMBIE,
    JE_RR_BLOCKED
} je_rr_state;

typedef struct {
    uint64_t starts;
    uint64_t dispatched_to;
    uint64_t resumed;
    uint64_t observed_running;
    uint64_t interleave;
} je_rr_counters;

/*
 * Architecture context seam.  The core stores these records and never
 * dereferences frame.  Hooks may populate usable_base/top and frame during
 * allocation/initialization; the core only forwards them to frame_check and
 * dispatch_install.
 *
 * `phase` is zero until the first successful dispatch to this slot and one
 * thereafter.  `owner` and `slot` are scheduler-owned identity fields and are
 * initialized to the slot number for a newly-created context.  A hook may
 * inspect, but must not change, those identities.
 */
typedef struct {
    volatile je_rr_state state;
    uint32_t slot;
    uint32_t owner;
    uintptr_t usable_base;
    uintptr_t top;
    unsigned phase;
    void *frame;
    volatile uint64_t wake;
    je_rr_counters counters;
    uint64_t last_worker_dispatch;
} je_rr_ctx;

typedef struct {
    /*
     * frame_init constructs an architecture-specific first-run frame.  The
     * returned pointer is retained by the table until reap.  It may fail; the
     * core then calls stack_free and leaves the slot EMPTY.
     */
    je_status (*frame_init)(je_rr_ctx *ctx, void (*entry)(void *), void *arg,
                            void **out_frame);
    /*
     * frame_check is called for every switch target before dispatch_install.
     * It must validate bounds, frame kind/architecture state, and exact
     * equality with ctx->frame.  A failure is returned to the architecture
     * caller and no resume frame is returned.
     */
    je_status (*frame_check)(const je_rr_ctx *ctx, void *frame,
                             unsigned phase);
    /*
     * Install the target context.  It may fail.  On failure the core restores
     * the previous RUNNING state and does not publish a new current slot.
     */
    je_status (*dispatch_install)(je_rr_ctx *prev, je_rr_ctx *next);
    /*
     * stack_alloc fills the context's usable_base/top bounds and establishes
     * ownership of the stack.  stack_free is called exactly once for every
     * successful stack_alloc that is later rolled back or reaped.
     */
    je_status (*stack_alloc)(je_rr_ctx *ctx);
    void (*stack_free)(je_rr_ctx *ctx);
    /*
     * Called when no worker is READY and slot zero is selected as fallback.
     * The core never embeds WFI or any architecture wait instruction.
     */
    void (*idle_wait)(void);
} je_rr_hooks;

typedef struct {
    const je_rr_hooks *hooks;
    je_rr_ctx slots[JE_RR_MAX];
    uint64_t ticks;
    uint64_t worker_dispatch_serial;
    uint32_t current;
    uint32_t cursor;
} je_rr_table;

/*
 * Initialize caller-owned table storage.  Slot zero is the always-selectable
 * idle context and starts RUNNING; it has no core-created frame.  `hooks` and
 * all hook members are required for create/switch/reap operations.  The caller
 * must provide stable hook storage for the table lifetime.
 */
void je_rr_init(je_rr_table *t, const je_rr_hooks *hooks);

/*
 * Create a worker in the first EMPTY slot in [1, JE_RR_MAX).  The core calls
 * stack_alloc followed by frame_init; on either failure it rolls back all
 * ownership and leaves the slot EMPTY.  On success the table owns the context
 * and saved frame, the worker is READY, and *out_slot receives its identity.
 * `entry` must remain a valid architecture entry target for the frame lifetime.
 * This is a bounded, nonblocking core operation; allocation behavior belongs
 * to the hook.
 */
je_status je_rr_create(je_rr_table *t, void (*entry)(void *), void *arg,
                       uint32_t *out_slot);

/*
 * Pick the next READY worker after the scheduler cursor, wrapping across the
 * bounded table.  Only READY workers are candidates; ZOMBIE and SLEEPING
 * contexts are never selected.  The cursor advances to the selected slot, so
 * two READY workers alternate instead of repeatedly selecting the first one.
 * If none is READY, return slot zero and invoke idle_wait when available.
 */
uint32_t je_rr_pick(je_rr_table *t);

/*
 * Record one timer observation.  The internal tick counter saturates at
 * UINT64_MAX rather than wrapping.  Every SLEEPING slot with now >= wake is
 * changed to READY (plain unsigned 64-bit comparison); ZOMBIE and EMPTY slots
 * are untouched.  The currently RUNNING slot's observed_running counter is
 * incremented.  Caller supplies a monotonic hardware time value; deadline
 * comparisons use `now`, not frame internals.
 */
void je_rr_tick(je_rr_table *t, uint64_t now);

/*
 * Validate and install `next`, returning its already-validated opaque frame.
 * next must be READY, except slot zero is always eligible as idle fallback.
 * If the current context was RUNNING it is changed to READY; a SLEEPING or
 * ZOMBIE previous context is never resurrected.  frame_check runs before
 * dispatch_install.  Any validation or install failure rolls back the
 * previous state, leaves current unchanged, sets *out_resume_frame to NULL,
 * and returns the hook error.  A non-NULL returned frame is therefore a
 * frame validated by the architecture hook.  This is a serialized, privileged
 * scheduler operation; hardware context installation remains in the hook.
 */
je_status je_rr_switch(je_rr_table *t, uint32_t next,
                       void **out_resume_frame);

/*
 * Put the current RUNNING worker to sleep until wake = now + n.  n must be in
 * [1, JE_RR_SLEEP_MAX] and the addition must not overflow.  Invalid requests,
 * non-current/non-RUNNING slots, slot zero, and terminal slots are rejected
 * without changing state.  For a valid request wake is stored before the
 * SLEEPING state, so a tick cannot observe a sleeping slot without a deadline.
 */
void je_rr_sleep(je_rr_table *t, uint32_t slot, uint64_t now, uint64_t n);

/*
 * Mark the current non-idle RUNNING worker BLOCKED.  BLOCKED is deliberately
 * separate from SLEEPING: only an endpoint/deadline owner may make it READY.
 * Invalid identities and non-RUNNING slots are ignored without changing state.
 */
void je_rr_block(je_rr_table *t, uint32_t slot);

/*
 * Make one BLOCKED worker READY.  This is a guarded, idempotent transition:
 * READY, SLEEPING, ZOMBIE, EMPTY, and slot zero are never changed.
 */
void je_rr_wake(je_rr_table *t, uint32_t slot);

/*
 * Mark a non-idle, non-EMPTY context ZOMBIE.  It is excluded from all future
 * picks and wake sweeps.  Slot zero exit is a fatal scheduler misuse and
 * fail-stops with a compiler trap; invalid/non-live non-idle slots are refused.
 */
void je_rr_exit(je_rr_table *t, uint32_t slot);

/*
 * Return the scheduler-owned current identity, or slot zero for a NULL table.
 * This value is independent of any saved frame contents.
 */
uint32_t je_rr_current(const je_rr_table *t);

/*
 * Reap a ZOMBIE.  The stack is freed before the slot is returned to EMPTY.
 * Reaping slot == current_stack_slot is refused with JE_ERR_BUSY because the
 * current frame may still reside on that stack; the caller must switch away
 * and retry.  No allocation or hardware operation is performed by the core.
 */
je_status je_rr_reap(je_rr_table *t, uint32_t slot,
                     uint32_t current_stack_slot);

/*
 * Read-only proof accessors.  The returned counter pointer remains valid until
 * the slot is reaped or the table is reinitialized.  Counters are maintained
 * in the core: starts/resumed are first/subsequent successful switch-ins,
 * dispatched_to counts successful installs, observed_running counts timer
 * observations, and interleave proves another worker ran between switch-ins.
 */
je_rr_state je_rr_get_state(const je_rr_table *t, uint32_t slot);
const je_rr_counters *je_rr_get_counters(const je_rr_table *t,
                                         uint32_t slot);

#endif
