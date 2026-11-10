/* SPDX-License-Identifier: Apache-2.0 */
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <jeios/sched_rr.h>

typedef struct {
    uint32_t slot;
    uint32_t magic;
} fake_frame;

static je_rr_table table;
static fake_frame fake_frames[JE_RR_MAX];
static unsigned char fake_stacks[JE_RR_MAX][128];
static bool stack_live[JE_RR_MAX];
static unsigned stack_alloc_count;
static unsigned stack_free_count;
static unsigned frame_init_count;
static unsigned frame_check_count;
static unsigned dispatch_count;
static unsigned idle_wait_count;
static uint32_t last_prev;
static uint32_t last_next;
static uint32_t fail_stack_slot;
static uint32_t fail_frame_slot;
static bool fail_dispatch;

static je_status fake_stack_alloc(je_rr_ctx *ctx)
{
    if (ctx == NULL || ctx->slot >= JE_RR_MAX) return JE_ERR_INVALID;
    ++stack_alloc_count;
    if (ctx->slot == fail_stack_slot) return JE_ERR_NOMEM;
    ctx->usable_base = (uintptr_t)&fake_stacks[ctx->slot][0];
    ctx->top = (uintptr_t)&fake_stacks[ctx->slot][sizeof fake_stacks[ctx->slot]];
    stack_live[ctx->slot] = true;
    return JE_OK;
}

static void fake_stack_free(je_rr_ctx *ctx)
{
    assert(ctx != NULL && ctx->slot < JE_RR_MAX);
    assert(stack_live[ctx->slot]);
    stack_live[ctx->slot] = false;
    ++stack_free_count;
}

static je_status fake_frame_init(je_rr_ctx *ctx, void (*entry)(void *),
                                 void *arg, void **out_frame)
{
    (void)entry;
    (void)arg;
    if (ctx == NULL || out_frame == NULL || ctx->slot >= JE_RR_MAX)
        return JE_ERR_INVALID;
    ++frame_init_count;
    if (ctx->slot == fail_frame_slot) return JE_ERR_IO;
    fake_frames[ctx->slot] = (fake_frame){ctx->slot, UINT32_C(0xfeedbabe)};
    *out_frame = &fake_frames[ctx->slot];
    return JE_OK;
}

static je_status fake_frame_check(const je_rr_ctx *ctx, void *frame,
                                  unsigned phase)
{
    uintptr_t address;
    uintptr_t first;
    uintptr_t last;
    fake_frame *candidate;

    assert(ctx != NULL);
    ++frame_check_count;
    if (ctx->slot == 0u)
        return frame == NULL && phase == 1u ? JE_OK : JE_ERR_INVALID;
    if (frame == NULL || phase > 1u) return JE_ERR_INVALID;
    address = (uintptr_t)frame;
    first = (uintptr_t)&fake_frames[0];
    last = (uintptr_t)&fake_frames[JE_RR_MAX];
    if (address < first || address >= last ||
        (address - first) % sizeof(fake_frame) != 0u)
        return JE_ERR_INVALID;
    if (frame != ctx->frame) return JE_ERR_INVALID;
    candidate = (fake_frame *)frame;
    if (candidate->magic != UINT32_C(0xfeedbabe) ||
        candidate->slot != ctx->slot)
        return JE_ERR_INVALID;
    return JE_OK;
}

static je_status fake_dispatch_install(je_rr_ctx *prev, je_rr_ctx *next)
{
    if (prev == NULL || next == NULL) return JE_ERR_INVALID;
    ++dispatch_count;
    last_prev = prev->slot;
    last_next = next->slot;
    return fail_dispatch ? JE_ERR_IO : JE_OK;
}

static void fake_idle_wait(void)
{
    ++idle_wait_count;
}

static const je_rr_hooks fake_hooks = {
    .frame_init = fake_frame_init,
    .frame_check = fake_frame_check,
    .dispatch_install = fake_dispatch_install,
    .stack_alloc = fake_stack_alloc,
    .stack_free = fake_stack_free,
    .idle_wait = fake_idle_wait
};

static void fake_entry(void *arg)
{
    (void)arg;
}

static void reset_fake(void)
{
    size_t i;

    memset(fake_frames, 0, sizeof fake_frames);
    memset(fake_stacks, 0, sizeof fake_stacks);
    memset(stack_live, 0, sizeof stack_live);
    stack_alloc_count = 0u;
    stack_free_count = 0u;
    frame_init_count = 0u;
    frame_check_count = 0u;
    dispatch_count = 0u;
    idle_wait_count = 0u;
    last_prev = UINT32_MAX;
    last_next = UINT32_MAX;
    fail_stack_slot = UINT32_MAX;
    fail_frame_slot = UINT32_MAX;
    fail_dispatch = false;
    for (i = 0u; i < JE_RR_MAX; ++i) {
        fake_frames[i].slot = (uint32_t)i;
        fake_frames[i].magic = UINT32_C(0xfeedbabe);
    }
    je_rr_init(&table, &fake_hooks);
}

static uint32_t create_one(void)
{
    uint32_t slot = UINT32_MAX;

    assert(je_rr_create(&table, fake_entry, NULL, &slot) == JE_OK);
    assert(slot != 0u && slot < JE_RR_MAX);
    assert(je_rr_get_state(&table, slot) == JE_RR_READY);
    assert(stack_live[slot]);
    return slot;
}

static void create_two(uint32_t *first, uint32_t *second)
{
    *first = create_one();
    *second = create_one();
    assert(*first != *second);
}

static void switch_to(uint32_t slot)
{
    void *resume = (void *)(uintptr_t)UINTPTR_MAX;

    assert(je_rr_switch(&table, slot, &resume) == JE_OK);
    assert(resume == table.slots[slot].frame);
    assert(je_rr_current(&table) == slot);
    assert(je_rr_get_state(&table, slot) == JE_RR_RUNNING);
}

static void test_rr_order(void)
{
    uint32_t a;
    uint32_t b;

    reset_fake();
    create_two(&a, &b);
    assert(je_rr_pick(&table) == a);
    assert(je_rr_pick(&table) == b);
    switch_to(a);
    assert(je_rr_pick(&table) == b);
    switch_to(b);
    assert(je_rr_pick(&table) == a);
    switch_to(a);
    assert(je_rr_pick(&table) == b);
    switch_to(b);
    assert(last_prev == a && last_next == b);
}

static void test_idle_fallback(void)
{
    reset_fake();
    assert(je_rr_pick(&table) == 0u);
    assert(idle_wait_count == 1u);
    assert(je_rr_current(&table) == 0u);
}

static void test_sleep_deadline(void)
{
    uint32_t a;

    reset_fake();
    a = create_one();
    switch_to(a);
    je_rr_sleep(&table, a, UINT64_C(10), UINT64_C(3));
    assert(table.slots[a].wake == UINT64_C(13));
    assert(je_rr_get_state(&table, a) == JE_RR_SLEEPING);
    switch_to(0u);
    /* With wake=now+n, now=12 is n-1 and now=13 is the first wake tick. */
    je_rr_tick(&table, UINT64_C(12));
    assert(je_rr_get_state(&table, a) == JE_RR_SLEEPING);
    je_rr_tick(&table, UINT64_C(13));
    assert(je_rr_get_state(&table, a) == JE_RR_READY);
    assert(je_rr_pick(&table) == a);
}

static void test_zombie_exclusion(void)
{
    uint32_t a;
    uint32_t b;

    reset_fake();
    create_two(&a, &b);
    je_rr_exit(&table, a);
    assert(je_rr_get_state(&table, a) == JE_RR_ZOMBIE);
    assert(je_rr_pick(&table) == b);
    switch_to(b);
    je_rr_sleep(&table, b, UINT64_C(20), UINT64_C(2));
    switch_to(0u);
    assert(je_rr_pick(&table) == 0u);
    je_rr_tick(&table, UINT64_C(21));
    assert(je_rr_get_state(&table, a) == JE_RR_ZOMBIE);
    assert(je_rr_get_state(&table, b) == JE_RR_SLEEPING);
    je_rr_tick(&table, UINT64_C(22));
    assert(je_rr_pick(&table) == b);
}

static void test_blocked_exclusion(void)
{
    uint32_t a;
    uint32_t b;

    reset_fake();
    create_two(&a, &b);
    switch_to(a);
    je_rr_block(&table, a);
    assert(je_rr_get_state(&table, a) == JE_RR_BLOCKED);
    assert(je_rr_pick(&table) == b);
    switch_to(b);
    assert(je_rr_pick(&table) == 0u);
}

static void test_blocked_switch_rejected(void)
{
    uint32_t a;
    void *resume = (void *)(uintptr_t)UINTPTR_MAX;

    reset_fake();
    a = create_one();
    switch_to(a);
    je_rr_block(&table, a);
    assert(je_rr_switch(&table, a, &resume) == JE_ERR_INVALID);
    assert(resume == NULL);
    assert(je_rr_current(&table) == a);
    assert(je_rr_get_state(&table, a) == JE_RR_BLOCKED);
}

static void test_blocked_wake_idempotent(void)
{
    uint32_t a;

    reset_fake();
    a = create_one();
    switch_to(a);
    je_rr_block(&table, a);
    je_rr_wake(&table, a);
    assert(je_rr_get_state(&table, a) == JE_RR_READY);
    je_rr_wake(&table, a);
    assert(je_rr_get_state(&table, a) == JE_RR_READY);
}

static void test_sleeping_wake_is_not_blocked_wake(void)
{
    uint32_t a;

    reset_fake();
    a = create_one();
    switch_to(a);
    je_rr_sleep(&table, a, UINT64_C(10), UINT64_C(1));
    assert(je_rr_get_state(&table, a) == JE_RR_SLEEPING);
    je_rr_wake(&table, a);
    assert(je_rr_get_state(&table, a) == JE_RR_SLEEPING);
}

static void test_blocked_death(void)
{
    uint32_t a;

    reset_fake();
    a = create_one();
    switch_to(a);
    je_rr_block(&table, a);
    je_rr_exit(&table, a);
    assert(je_rr_get_state(&table, a) == JE_RR_ZOMBIE);
    je_rr_wake(&table, a);
    assert(je_rr_get_state(&table, a) == JE_RR_ZOMBIE);
}

static void test_counters(void)
{
    const je_rr_counters *a_counts;
    const je_rr_counters *b_counts;
    uint32_t a;
    uint32_t b;

    reset_fake();
    create_two(&a, &b);
    switch_to(a);
    je_rr_tick(&table, 1u);
    switch_to(je_rr_pick(&table));
    je_rr_tick(&table, 2u);
    switch_to(je_rr_pick(&table));
    je_rr_tick(&table, 3u);
    switch_to(je_rr_pick(&table));
    je_rr_tick(&table, 4u);

    a_counts = je_rr_get_counters(&table, a);
    b_counts = je_rr_get_counters(&table, b);
    assert(a_counts != NULL && b_counts != NULL);
    assert(a_counts->starts == 1u && b_counts->starts == 1u);
    assert(a_counts->dispatched_to == 2u && b_counts->dispatched_to == 2u);
    assert(a_counts->resumed == 1u && b_counts->resumed == 1u);
    assert(a_counts->observed_running == 2u);
    assert(b_counts->observed_running == 2u);
    assert(a_counts->interleave >= 1u && b_counts->interleave >= 1u);
}

static void test_frame_init_rollback(void)
{
    uint32_t slot = UINT32_MAX;

    reset_fake();
    fail_frame_slot = 1u;
    assert(je_rr_create(&table, fake_entry, NULL, &slot) == JE_ERR_IO);
    assert(slot == UINT32_MAX);
    assert(je_rr_get_state(&table, 1u) == JE_RR_EMPTY);
    assert(!stack_live[1u]);
    assert(stack_alloc_count == 1u && stack_free_count == 1u);
}

static void test_dispatch_rollback(void)
{
    uint32_t a;
    uint32_t b;
    void *resume = (void *)(uintptr_t)UINTPTR_MAX;

    reset_fake();
    create_two(&a, &b);
    switch_to(a);
    fail_dispatch = true;
    assert(je_rr_pick(&table) == b);
    assert(je_rr_switch(&table, b, &resume) == JE_ERR_IO);
    assert(resume == NULL);
    assert(je_rr_current(&table) == a);
    assert(je_rr_get_state(&table, a) == JE_RR_RUNNING);
    assert(je_rr_get_state(&table, b) == JE_RR_READY);
    fail_dispatch = false;
    switch_to(b);
}

static void test_frame_check_rejection(void)
{
    uint32_t a;
    uint32_t b;
    void *original;
    void *resume = (void *)(uintptr_t)UINTPTR_MAX;

    reset_fake();
    create_two(&a, &b);
    switch_to(a);
    original = table.slots[b].frame;
    table.slots[b].frame = (void *)(uintptr_t)1u;
    assert(je_rr_switch(&table, b, &resume) == JE_ERR_INVALID);
    assert(resume == NULL);
    assert(je_rr_current(&table) == a);
    assert(je_rr_get_state(&table, a) == JE_RR_RUNNING);
    assert(je_rr_get_state(&table, b) == JE_RR_READY);
    table.slots[b].frame = table.slots[a].frame;
    assert(je_rr_switch(&table, b, &resume) == JE_ERR_INVALID);
    assert(resume == NULL);
    assert(je_rr_current(&table) == a);
    table.slots[b].frame = original;
    switch_to(b);
}

static void test_reap_rules(void)
{
    uint32_t a;

    reset_fake();
    a = create_one();
    je_rr_exit(&table, a);
    assert(je_rr_reap(&table, a, a) == JE_ERR_BUSY);
    assert(je_rr_get_state(&table, a) == JE_RR_ZOMBIE);
    assert(stack_live[a]);
    assert(je_rr_reap(&table, a, 0u) == JE_OK);
    assert(je_rr_get_state(&table, a) == JE_RR_EMPTY);
    assert(!stack_live[a]);
}

static void test_sleep_bounds(void)
{
    uint32_t a;
    uint64_t original_wake;

    reset_fake();
    a = create_one();
    switch_to(a);
    original_wake = table.slots[a].wake;
    je_rr_sleep(&table, a, 4u, 0u);
    je_rr_sleep(&table, a, 4u, JE_RR_SLEEP_MAX + UINT64_C(1));
    je_rr_sleep(&table, a, UINT64_MAX - UINT64_C(1), UINT64_C(2));
    assert(je_rr_get_state(&table, a) == JE_RR_RUNNING);
    assert(table.slots[a].wake == original_wake);
    table.ticks = UINT64_MAX - UINT64_C(1);
    je_rr_tick(&table, 4u);
    je_rr_tick(&table, 5u);
    assert(table.ticks == UINT64_MAX);
}

static void test_resurrect_guard(void)
{
    uint32_t a;
    uint32_t b;
    void *resume = NULL;

    reset_fake();
    create_two(&a, &b);
    switch_to(a);
    je_rr_exit(&table, a);
    assert(je_rr_pick(&table) == b);
    switch_to(b);
    je_rr_sleep(&table, b, UINT64_C(30), UINT64_C(2));
    switch_to(0u);
    assert(je_rr_get_state(&table, a) == JE_RR_ZOMBIE);
    assert(je_rr_get_state(&table, b) == JE_RR_SLEEPING);
    je_rr_tick(&table, UINT64_C(31));
    assert(je_rr_get_state(&table, a) == JE_RR_ZOMBIE);
    assert(je_rr_get_state(&table, b) == JE_RR_SLEEPING);
    assert(je_rr_switch(&table, a, &resume) == JE_ERR_INVALID);
    assert(resume == NULL);
}

int main(void)
{
    test_rr_order();
    test_idle_fallback();
    test_sleep_deadline();
    test_zombie_exclusion();
    test_blocked_exclusion();
    test_blocked_switch_rejected();
    test_blocked_wake_idempotent();
    test_sleeping_wake_is_not_blocked_wake();
    test_blocked_death();
    test_counters();
    test_frame_init_rollback();
    test_dispatch_rollback();
    test_frame_check_rejection();
    test_reap_rules();
    test_sleep_bounds();
    test_resurrect_guard();
    puts("test_sched_rr: PASS");
    return 0;
}
