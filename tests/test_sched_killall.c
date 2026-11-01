/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hosted coverage for ADR 0010 D12's slot-0 kill-all mechanism.
 *
 * The scheduler owns architecture state and its thread table, so this test
 * includes the production scheduler translation unit with its private linkage
 * exposed only inside this hosted binary. The arch and endpoint waiter hooks
 * below are deliberately small fakes; PROCESS identity and lifecycle behavior
 * come from the production kernel/core/proc.c implementation.
 */
#include <jeios/arch.h>
#include <jeios/endpoint_kern.h>
#include <jeios/proc.h>
#include <stdio.h>
#include <string.h>

#define static
#include "../kernel/core/sched.c"
#undef static

static unsigned g_checks;
static unsigned g_failures;
static unsigned g_waiter_releases;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            fprintf(stderr, "test_sched_killall: FAIL %s:%d: %s [%s]\n",         \
                    __FILE__, __LINE__, (what), #cond);                         \
        }                                                                        \
    } while (0)

uint64_t je_arch_irq_save(void)
{
    return 0u; /* hosted fake: interrupt state is always disabled */
}

void je_arch_irq_restore(uint64_t flags)
{
    (void)flags;
}

void je_arch_console_write(const char *text)
{
    (void)text;
}

_Noreturn void je_arch_halt(void)
{
    /* No test drives an abort_scheduler path; keep a defined fail-stop stub. */
    for (;;) { }
}

bool je_endpoint_kern_waiter_release_by_id(uint32_t slot)
{
    (void)slot;
    ++g_waiter_releases;
    return true;
}

static je_proc_pool g_pool;
static je_trap_frame g_frames[JE_THREAD_MAX];

static void reset_fixture(void)
{
    memset(threads, 0, sizeof threads);
    memset(g_frames, 0, sizeof g_frames);
    je_proc_pool_init(&g_pool);
    process_pool = &g_pool;
    current = 0u;
    ready = true;
    g_waiter_releases = 0u;
    threads[0].kind = THREAD_KERNEL;
    threads[0].state = RUNNING;
}

static je_proc_id add_process(uint32_t slot, uint32_t vm_index, bool supervisor,
                              state thread_state)
{
    je_proc_id id = JE_PROC_ID_NONE;
    CHECK(je_proc_alloc(&g_pool, 0u, supervisor, &id) == JE_OK,
          "process allocation succeeds");
    CHECK(je_proc_link(&g_pool, id, slot, vm_index) == JE_OK,
          "process run link succeeds");
    threads[slot].kind = THREAD_USER;
    threads[slot].state = thread_state;
    threads[slot].vm_index = vm_index;
    threads[slot].proc_id = id;
    if (thread_state == READY || thread_state == BLOCKED_IPC) {
        g_frames[slot].rip = UINT64_C(0x40000000) + slot;
        g_frames[slot].cs = JE_UFRAME_USER_CS;
        threads[slot].frame = &g_frames[slot];
    }
    return id;
}

static void test_live_and_spare(void)
{
    reset_fixture();
    je_proc_id spare = add_process(1u, 0u, true, READY);
    je_proc_id child_a = add_process(2u, 1u, false, READY);
    je_proc_id child_b = add_process(3u, 2u, false, BLOCKED_IPC);
    uint32_t killed = UINT32_MAX;

    je_status s = je_sched_kill_all_user(spare, &killed);
    CHECK(s == JE_OK, "live linked users are killed");
    CHECK(killed == 2u, "only non-supervisor live users are counted");
    CHECK(threads[1].state == READY, "supervisor thread is untouched");
    CHECK(threads[2].state == DYING && threads[3].state == DYING,
          "live user threads become DYING");
    CHECK(g_pool.rec[child_a.index].state == JE_PROC_DYING
          && g_pool.rec[child_b.index].state == JE_PROC_DYING,
          "live user records become DYING");
    CHECK(g_pool.rec[spare.index].state == JE_PROC_LIVE,
          "supervisor record is untouched");
    CHECK(g_waiter_releases == 2u,
          "mark_dying owns waiter/reservation release for every target");
}

static void test_dying_and_woken(void)
{
    reset_fixture();
    je_proc_id spare = add_process(1u, 0u, true, READY);
    je_proc_id dying = add_process(2u, 1u, false, DYING);
    (void)add_process(3u, 2u, false, READY);
    threads[3].wait_pending = true;
    CHECK(je_proc_on_dying(&g_pool, dying, JE_DEATH_KILLED, 0u) == JE_OK,
          "pre-existing DYING record is valid");
    uint32_t killed = UINT32_MAX;

    je_status s = je_sched_kill_all_user(spare, &killed);
    CHECK(s == JE_OK, "already-DYING users are accepted");
    CHECK(killed == 1u, "already-DYING user is not counted");
    CHECK(threads[2].state == DYING, "already-DYING slot remains DYING");
    CHECK(threads[3].state == DYING && !threads[3].wait_pending,
          "READY waiter reservation follows mark_dying");
    CHECK(g_waiter_releases == 1u,
          "only the newly killed READY waiter is released");
}

static void test_fail_stop_no_change(void)
{
    const state bad_states[] = { RUNNING, SLEEPING, ZOMBIE };
    for (size_t n = 0u; n < sizeof bad_states / sizeof bad_states[0]; ++n) {
        reset_fixture();
        je_proc_id spare = add_process(1u, 0u, true, READY);
        (void)add_process(2u, 1u, false, READY);
        (void)add_process(3u, 2u, false, READY);
        threads[3].state = bad_states[n];

        thread threads_before[JE_THREAD_MAX];
        je_proc_pool pool_before;
        memcpy(threads_before, threads, sizeof threads_before);
        memcpy(&pool_before, &g_pool, sizeof pool_before);
        uint32_t killed = UINT32_MAX;

        je_status s = je_sched_kill_all_user(spare, &killed);
        CHECK(s == JE_ERR_INVALID, "invalid scheduler state is fail-stop");
        CHECK(killed == 0u, "fail-stop reports no transitions");
        CHECK(memcmp(threads_before, threads, sizeof threads_before) == 0,
              "fail-stop leaves scheduler state unchanged");
        CHECK(memcmp(&pool_before, &g_pool, sizeof pool_before) == 0,
              "fail-stop leaves process state unchanged");
        CHECK(g_waiter_releases == 0u, "fail-stop releases no waiter");
    }

    reset_fixture();
    je_proc_id spare = add_process(1u, 0u, true, READY);
    (void)add_process(2u, 1u, false, READY);
    threads[2].proc_id.gen = 0u;
    thread threads_before[JE_THREAD_MAX];
    je_proc_pool pool_before;
    memcpy(threads_before, threads, sizeof threads_before);
    memcpy(&pool_before, &g_pool, sizeof pool_before);
    uint32_t killed = UINT32_MAX;

    CHECK(je_sched_kill_all_user(spare, &killed) == JE_ERR_INVALID,
          "unregistered identity is fail-stop");
    CHECK(killed == 0u, "identity fail-stop reports no transitions");
    CHECK(memcmp(threads_before, threads, sizeof threads_before) == 0,
          "identity fail-stop leaves scheduler state unchanged");
    CHECK(memcmp(&pool_before, &g_pool, sizeof pool_before) == 0,
          "identity fail-stop leaves process state unchanged");
}

int main(void)
{
    test_live_and_spare();
    test_dying_and_woken();
    test_fail_stop_no_change();
    if (g_failures != 0u) {
        fprintf(stderr, "test_sched_killall: FAIL (%u of %u checks)\n",
                g_failures, g_checks);
        return 1;
    }
    printf("test_sched_killall: PASS (%u checks)\n", g_checks);
    return 0;
}
