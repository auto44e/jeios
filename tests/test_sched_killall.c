/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hosted coverage for ADR 0010 D12's slot-0 kill-all mechanism and for the
 * bootstrap reaper je_sched_reap_dead (fix-gate F03).
 *
 * The scheduler owns architecture state and its thread table, so this test
 * includes the production scheduler translation unit with its private linkage
 * exposed only inside this hosted binary. The arch, endpoint waiter and vm
 * destroy hooks below are deliberately small fakes that RECORD their calls;
 * PROCESS identity and lifecycle behavior come from the production
 * kernel/core/proc.c implementation, and je_sched_drop_user /
 * je_sched_unregister_process are the real sched.c code.
 *
 * Fail-stop harness: abort_scheduler() ends in je_arch_halt(). The fake halt
 * counts the abort and longjmps back to reap_catching(), so a test can assert
 * "the reaper fail-stopped" without hanging. A halt reached while no catch is
 * armed is itself a test failure and terminates the binary non-zero.
 */
#include <jeios/arch.h>
#include <jeios/endpoint_kern.h>
#include <jeios/paging.h>
#include <jeios/proc.h>
#include <jeios/vm.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- fail-stop (abort_scheduler -> je_arch_halt) catch ---------------------- */
static jmp_buf g_halt_jmp;
static bool g_halt_armed;
static unsigned g_aborts;

/* ---- arch fakes (test-controlled; defaults restored by reset_fixture) -------- */
/* Contract (include/jeios/arch.h): the hosted override DEFAULTS to interrupts
 * off; only test_reap_if_enabled_fail_stop sets it true. */
static bool g_irqs_enabled;
static unsigned g_irqs_queries;
static uint64_t g_active_cr3;
static unsigned g_cr3_reads;
static uint64_t g_active_rsp0;
static unsigned g_rsp0_reads;
/* je_arch_page_restore: call N (1-based) returns g_page_restore_fail_status;
 * 0 = never fail. Every call's address is recorded in order. */
static unsigned g_page_restore_calls;
static unsigned g_page_restore_fail_call;
static je_status g_page_restore_fail_status;
static uintptr_t g_page_restore_addr[JE_THREAD_MAX];

/* ---- vm destroy fake --------------------------------------------------------- */
typedef struct {
    je_vm *vm;
    uint32_t index;
    je_proc_id owner;
    bool sched_still_had_process; /* je_sched_has_process at call time */
    uint32_t owner_state;         /* PROCESS record state at call time */
} destroy_call;
static unsigned g_destroy_calls;
static je_status g_destroy_status;
static destroy_call g_destroy[JE_THREAD_MAX];

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
    ++g_aborts;
    if (!g_halt_armed) {
        /* An abort_scheduler outside a reap_catching() window is unexpected:
         * fail loudly (non-zero exit) instead of spinning or returning. */
        fprintf(stderr, "test_sched_killall: FAIL unexpected scheduler fail-stop\n");
        abort();
    }
    g_halt_armed = false;
    longjmp(g_halt_jmp, 1);
}

bool je_arch_irqs_enabled(void)
{
    ++g_irqs_queries;
    return g_irqs_enabled;
}

uint64_t je_arch_cr3_read(void)
{
    ++g_cr3_reads;
    return g_active_cr3;
}

uint64_t je_arch_tss_get_rsp0(void)
{
    ++g_rsp0_reads;
    return g_active_rsp0;
}

je_status je_arch_page_restore(uintptr_t address)
{
    unsigned n = g_page_restore_calls++;
    if (n < JE_THREAD_MAX) g_page_restore_addr[n] = address;
    if (g_page_restore_fail_call != 0u && n + 1u == g_page_restore_fail_call)
        return g_page_restore_fail_status;
    return JE_OK;
}

/* F08 fakes for je_thread_create_user_start. NX is reported present (ADR 0007
 * D2 precondition). je_arch_page_guard RECORDS every call and always succeeds:
 * it deliberately does NOT model "guarding an already-guarded page fails", so
 * the only thing that can stop a duplicate create in this harness is the
 * explicit F08 (vm,index) scan — not the incidental downstream failure the scan
 * replaced. */
static unsigned g_page_guard_calls;
static uintptr_t g_page_guard_addr[JE_THREAD_MAX];

int je_arch_has_nx(void)
{
    return 1;
}

je_status je_arch_page_guard(uintptr_t address)
{
    unsigned n = g_page_guard_calls++;
    if (n < JE_THREAD_MAX) g_page_guard_addr[n] = address;
    return JE_OK;
}

static je_proc_pool g_pool;

je_status je_vm_destroy_owned(je_vm *vm, uint32_t index, je_proc_id owner)
{
    unsigned n = g_destroy_calls++;
    if (n < JE_THREAD_MAX) {
        g_destroy[n].vm = vm;
        g_destroy[n].index = index;
        g_destroy[n].owner = owner;
        g_destroy[n].sched_still_had_process = je_sched_has_process(vm, index);
        g_destroy[n].owner_state = owner.index < JE_PROC_REC_MAX
            ? (uint32_t)g_pool.rec[owner.index].state : UINT32_MAX;
    }
    return g_destroy_status;
}

bool je_endpoint_kern_waiter_release_by_id(uint32_t slot)
{
    (void)slot;
    ++g_waiter_releases;
    return true;
}

static je_trap_frame g_frames[JE_THREAD_MAX];
static je_vm g_vm;

#define KERNEL_CR3   UINT64_C(0x0000000000001000)
#define KERNEL_RSP0  UINT64_C(0xffff800000070000)
#define SLOT_ROOT(s) (UINT64_C(0x0000000000400000) + (uint64_t)(s) * UINT64_C(0x1000))
#define SLOT_RSP0(s) (UINT64_C(0xffff800001000000) + (uint64_t)(s) * UINT64_C(0x10000))
#define KBASE_OF(top) ((uintptr_t)((top) - (uint64_t)STACK_PAGES * 4096u))

static void reset_fixture(void)
{
    memset(threads, 0, sizeof threads);
    memset(g_frames, 0, sizeof g_frames);
    memset(registered_vm, 0, sizeof registered_vm);
    memset(registered_proc, 0, sizeof registered_proc);
    memset(&g_vm, 0, sizeof g_vm);
    je_proc_pool_init(&g_pool);
    process_pool = &g_pool;
    current = 0u;
    ready = true;
    g_waiter_releases = 0u;
    g_halt_armed = false;
    g_aborts = 0u;
    g_irqs_enabled = false;
    g_irqs_queries = 0u;
    g_active_cr3 = KERNEL_CR3;
    g_cr3_reads = 0u;
    g_active_rsp0 = KERNEL_RSP0;
    g_rsp0_reads = 0u;
    g_page_restore_calls = 0u;
    g_page_restore_fail_call = 0u;
    g_page_restore_fail_status = JE_OK;
    memset(g_page_restore_addr, 0, sizeof g_page_restore_addr);
    g_page_guard_calls = 0u;
    memset(g_page_guard_addr, 0, sizeof g_page_guard_addr);
    g_destroy_calls = 0u;
    g_destroy_status = JE_OK;
    memset(g_destroy, 0, sizeof g_destroy);
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

/* ======================= je_sched_reap_dead (F03) ============================ */

/* Run the reaper with the fail-stop catch armed. Returns true and stores the
 * result if it returned normally; returns false if abort_scheduler fired. Only
 * globals and `out` (never written after setjmp) are touched across longjmp. */
static bool reap_catching(int *out)
{
    g_halt_armed = true;
    if (setjmp(g_halt_jmp) != 0) return false;
    int r = je_sched_reap_dead();
    g_halt_armed = false;
    *out = r;
    return true;
}

/* A non-supervisor user slot that has died: thread DYING, PROCESS record DYING,
 * owned by g_vm[vm_index], registered with the scheduler, with a distinct CR3
 * and kernel-stack top so the liveness guards can be driven per slot. */
static je_proc_id add_dying_user(uint32_t slot, uint32_t vm_index)
{
    je_proc_id id = add_process(slot, vm_index, false, DYING);
    CHECK(je_proc_on_dying(&g_pool, id, JE_DEATH_KILLED, 0u) == JE_OK,
          "fixture: PROCESS record becomes DYING");
    threads[slot].vm = &g_vm;
    threads[slot].root = SLOT_ROOT(slot);
    threads[slot].rsp0_top = SLOT_RSP0(slot);
    CHECK(je_sched_register_process(&g_vm, vm_index, id) == JE_OK,
          "fixture: process registered with the scheduler");
    return id;
}

static bool slot_wiped(uint32_t slot)
{
    const thread *t = &threads[slot];
    return t->state == EMPTY && t->kind == THREAD_KERNEL && t->vm == (je_vm *)0
        && t->vm_index == 0u && t->root == 0u && t->rsp0_top == 0u
        && t->proc_id.gen == 0u && t->frame == (je_trap_frame *)0;
}

/* Reaping fixtures deliberately give each DYING slot a vm_index DISTINCT from
 * its slot number (and from every other slot number in the case), so a reaper
 * that tore down the loop slot `i` instead of threads[i].vm_index would destroy
 * / unregister the wrong process and be caught. JE_VM_MAX_PROCESSES is 8, so the
 * indices must stay below 8 to register; slot s -> vm_index 8 - s. */
#define REAP_VMI(s) (8u - (uint32_t)(s))

static bool same_id(je_proc_id a, je_proc_id b)
{
    return a.index == b.index && a.gen == b.gen;
}

/* Snapshot of everything the reaper may mutate, for "no side effect" checks. */
typedef struct {
    thread threads[JE_THREAD_MAX];
    je_proc_pool pool;
    je_vm *reg_vm[JE_VM_MAX_PROCESSES];
    je_proc_id reg_proc[JE_VM_MAX_PROCESSES];
} sched_snapshot;
static sched_snapshot g_before;

static void snapshot(void)
{
    memcpy(g_before.threads, threads, sizeof g_before.threads);
    memcpy(&g_before.pool, &g_pool, sizeof g_before.pool);
    memcpy(g_before.reg_vm, registered_vm, sizeof g_before.reg_vm);
    memcpy(g_before.reg_proc, registered_proc, sizeof g_before.reg_proc);
}

static bool unchanged_since_snapshot(void)
{
    return memcmp(g_before.threads, threads, sizeof g_before.threads) == 0
        && memcmp(&g_before.pool, &g_pool, sizeof g_before.pool) == 0
        && memcmp(g_before.reg_vm, registered_vm, sizeof g_before.reg_vm) == 0
        && memcmp(g_before.reg_proc, registered_proc, sizeof g_before.reg_proc) == 0;
}

/* Case 1: wrong caller context -> exactly -JE_ERR_BUSY before any arch query. */
static void test_reap_wrong_context(void)
{
    for (unsigned variant = 0u; variant < 2u; ++variant) {
        reset_fixture();
        (void)add_dying_user(1u, 1u);
        (void)add_process(2u, 2u, false, READY);
        if (variant == 0u) ready = false;
        else current = 2u;
        snapshot();
        int r = 0;
        bool returned = reap_catching(&r);
        CHECK(returned && g_aborts == 0u, "wrong context is an error, not a fail-stop");
        CHECK(r == -(int)JE_ERR_BUSY, "not-ready / current!=0 returns exactly -5");
        CHECK(g_irqs_queries == 0u && g_cr3_reads == 0u && g_rsp0_reads == 0u,
              "context is refused before any arch state is read");
        CHECK(g_page_restore_calls == 0u && g_destroy_calls == 0u
              && g_waiter_releases == 0u,
              "wrong context touches no guard, waiter or vm stub");
        CHECK(unchanged_since_snapshot(),
              "wrong context leaves threads, PROCESS pool and registry unchanged");
        CHECK(threads[1].state == DYING, "the DYING slot stays DYING");
    }
}

/* Case 2: interrupts enabled at entry is a fail-stop misuse. */
static void test_reap_if_enabled_fail_stop(void)
{
    reset_fixture();
    (void)add_dying_user(1u, 1u);
    g_irqs_enabled = true;
    snapshot();
    int r = 12345;
    bool returned = reap_catching(&r);
    g_irqs_enabled = false;
    CHECK(!returned, "reaper does not return with IF set");
    CHECK(g_aborts == 1u, "abort_scheduler fired exactly once (IF-on misuse)");
    CHECK(g_irqs_queries == 1u, "the IF precondition was actually queried");
    CHECK(g_destroy_calls == 0u, "no vm process destroyed under IF=1");
    CHECK(g_page_restore_calls == 0u && g_waiter_releases == 0u,
          "no kstack guard restore or waiter release under IF=1");
    CHECK(threads[1].state == DYING && threads[1].kind == THREAD_USER
          && threads[1].vm == &g_vm,
          "slot still DYING and still owned after the fail-stop");
    CHECK(unchanged_since_snapshot(),
          "IF-on fail-stop leaves threads, PROCESS pool and registry unchanged");
}

/* Cases 3 and 4: a DYING slot that is still the live address space / kernel
 * stack must not be reaped; the same slot with mismatched arch state is. */
static void check_guard_refusal(bool cr3_guard)
{
    reset_fixture();
    (void)add_dying_user(1u, 1u);
    if (cr3_guard) g_active_cr3 = SLOT_ROOT(1u);
    else g_active_rsp0 = SLOT_RSP0(1u);
    snapshot();
    int r = 0;
    bool returned = reap_catching(&r);
    CHECK(returned && g_aborts == 0u, "liveness guard is an error, not a fail-stop");
    CHECK(r == -(int)JE_ERR_BUSY,
          cr3_guard ? "slot whose root is the active CR3 -> -5"
                    : "slot whose rsp0_top is the active TSS.RSP0 -> -5");
    CHECK(g_page_restore_calls == 0u && g_destroy_calls == 0u
          && g_waiter_releases == 0u,
          "guard refusal: no page_restore, no destroy, no waiter release");
    CHECK(unchanged_since_snapshot(),
          "guard refusal leaves the slot, PROCESS record and registry unchanged");
}

static void check_guard_mismatch_reaps(void)
{
    reset_fixture();
    je_proc_id id = add_dying_user(1u, REAP_VMI(1u));
    /* Active CR3/RSP0 are the kernel's (reset defaults), not this slot's. */
    int r = 0;
    bool returned = reap_catching(&r);
    CHECK(returned && g_aborts == 0u, "clean slot reaps without fail-stop");
    CHECK(r == 1, "mismatched CR3 and RSP0 -> the slot IS reaped (guard not always -5)");
    CHECK(slot_wiped(1u), "reaped slot is wiped");
    CHECK(g_destroy_calls == 1u && g_destroy[0].vm == &g_vm
          && g_destroy[0].index == REAP_VMI(1u) && same_id(g_destroy[0].owner, id),
          "destroy called once with exact (vm, RECORDED vm_index, owner)");
    CHECK(registered_vm[REAP_VMI(1u)] == (je_vm *)0,
          "reaped process unregistered at its recorded vm_index");
    CHECK(!je_proc_valid(&g_pool, id), "reaped PROCESS identity is no longer valid");
}

static void test_reap_active_cr3(void)
{
    check_guard_refusal(true);
    check_guard_mismatch_reaps();
}

static void test_reap_active_rsp0(void)
{
    check_guard_refusal(false);
    check_guard_mismatch_reaps();
}

/* Case 5: je_sched_drop_user fails (kstack guard restore -> JE_ERR_IO). */
static void test_reap_drop_failure(void)
{
    reset_fixture();
    (void)add_dying_user(1u, 1u);
    g_page_restore_fail_call = 1u;
    g_page_restore_fail_status = JE_ERR_IO;
    snapshot();
    int r = 0;
    bool returned = reap_catching(&r);
    CHECK(returned && g_aborts == 0u, "drop failure is reported, not a fail-stop");
    CHECK(r == -(int)JE_ERR_IO, "drop_user failure is returned NEGATIVE: exactly -6");
    CHECK(r < 0, "the idle loop's `< 0` guard would fire");
    CHECK(g_page_restore_calls == 1u
          && g_page_restore_addr[0] == KBASE_OF(SLOT_RSP0(1u)),
          "guard restore attempted once at kbase = rsp0_top - STACK_PAGES*4096");
    CHECK(g_destroy_calls == 0u, "vm process NOT destroyed after drop failure");
    CHECK(memcmp(&g_before.pool, &g_pool, sizeof g_pool) == 0
          && g_pool.rec[threads[1].proc_id.index].state == JE_PROC_DYING,
          "je_proc_on_reaped NOT called: PROCESS record still DYING, pool unchanged");
    CHECK(memcmp(g_before.threads, threads, sizeof g_before.threads) == 0
          && threads[1].state == DYING && threads[1].kind == THREAD_USER,
          "slot NOT wiped: still DYING with its owner/root/rsp0 intact");
    CHECK(registered_vm[1] == &g_vm, "process still registered (not unregistered)");
}

/* Case 6: partial progress. Slot 1 reaps, slot 2 hits the CR3 guard. */
static void test_reap_partial_progress(void)
{
    reset_fixture();
    je_proc_id id1 = add_dying_user(1u, REAP_VMI(1u));
    je_proc_id id2 = add_dying_user(2u, REAP_VMI(2u));
    g_active_cr3 = SLOT_ROOT(2u);
    snapshot();
    int r = 0;
    bool returned = reap_catching(&r);
    CHECK(returned && g_aborts == 0u, "partial progress is an error, not a fail-stop");
    CHECK(r == -(int)JE_ERR_BUSY, "second slot's guard -> -5 after one reap");
    CHECK(slot_wiped(1u), "slot 1 reaped (wiped)");
    CHECK(g_destroy_calls == 1u && g_destroy[0].vm == &g_vm
          && g_destroy[0].index == REAP_VMI(1u) && same_id(g_destroy[0].owner, id1),
          "slot 1 destroyed exactly once with its RECORDED vm_index (not the slot no.)");
    CHECK(g_page_restore_calls == 1u
          && g_page_restore_addr[0] == KBASE_OF(SLOT_RSP0(1u)),
          "only slot 1's kstack guard restored");
    CHECK(g_pool.rec[id1.index].state == JE_PROC_FREE && !je_proc_valid(&g_pool, id1),
          "slot 1 PROCESS record reaped (DEAD, no refs -> FREE)");
    CHECK(registered_vm[REAP_VMI(1u)] == (je_vm *)0,
          "slot 1's recorded vm_index unregistered");
    CHECK(memcmp(&g_before.threads[2], &threads[2], sizeof threads[2]) == 0
          && threads[2].state == DYING,
          "slot 2 untouched and still DYING");
    CHECK(memcmp(&g_before.pool.rec[id2.index], &g_pool.rec[id2.index],
                 sizeof g_pool.rec[id2.index]) == 0
          && g_pool.rec[id2.index].state == JE_PROC_DYING,
          "slot 2 PROCESS record untouched (DYING)");
    CHECK(registered_vm[REAP_VMI(2u)] == &g_vm
          && same_id(registered_proc[REAP_VMI(2u)], id2),
          "slot 2 still registered at its recorded vm_index");
}

/* Case 7: success paths, 0 and N reaps, then idempotent re-run. */
static void test_reap_success_counts(void)
{
    reset_fixture();
    (void)add_process(1u, 0u, true, READY);
    (void)add_process(2u, 1u, false, BLOCKED_IPC);
    snapshot();
    int r = -1;
    bool returned = reap_catching(&r);
    CHECK(returned && g_aborts == 0u && r == 0, "no DYING slot -> returns 0");
    CHECK(g_page_restore_calls == 0u && g_destroy_calls == 0u
          && g_waiter_releases == 0u && unchanged_since_snapshot(),
          "0-reap touches nothing");

    enum { N = 3 };
    reset_fixture();
    je_proc_id ids[N + 1];
    for (uint32_t s = 1u; s <= (uint32_t)N; ++s) ids[s] = add_dying_user(s, REAP_VMI(s));
    je_proc_id live = add_process(4u, 4u, false, READY);
    /* A PROCESS handle still names slot 2: on_reaped must leave it DEAD, not FREE. */
    CHECK(je_proc_retain(&g_pool, ids[2]) == JE_OK, "fixture: retain slot 2 identity");
    thread live_before = threads[4];
    r = -1;
    returned = reap_catching(&r);
    CHECK(returned && g_aborts == 0u, "N clean reaps without fail-stop");
    CHECK(r == N, "N DYING slots all clean -> returns N");
    CHECK(g_destroy_calls == (unsigned)N, "destroy called N times");
    CHECK(g_page_restore_calls == (unsigned)N, "kstack guard restored N times");
    CHECK(g_waiter_releases == (unsigned)N, "each dropped slot's waiter released once");
    for (uint32_t s = 1u; s <= (uint32_t)N; ++s) {
        unsigned k = s - 1u;
        CHECK(g_destroy[k].vm == &g_vm && g_destroy[k].index == REAP_VMI(s)
              && same_id(g_destroy[k].owner, ids[s]),
              "destroy args exact (vm, RECORDED vm_index, proc_id) in slot order");
        CHECK(!g_destroy[k].sched_still_had_process,
              "slot dropped from scheduling BEFORE its vm process is destroyed");
        CHECK(g_destroy[k].owner_state == JE_PROC_DYING,
              "PROCESS still DYING at destroy (on_reaped runs after destroy)");
        CHECK(g_page_restore_addr[k] == KBASE_OF(SLOT_RSP0(s)),
              "page_restore at kbase = rsp0_top - STACK_PAGES*4096");
        CHECK(slot_wiped(s), "reaped slot wiped");
        CHECK(registered_vm[REAP_VMI(s)] == (je_vm *)0,
              "reaped process unregistered at its recorded vm_index");
    }
    CHECK(g_pool.rec[ids[1].index].state == JE_PROC_FREE
          && g_pool.rec[ids[3].index].state == JE_PROC_FREE,
          "unreferenced reaped records are FREE");
    CHECK(g_pool.rec[ids[2].index].state == JE_PROC_DEAD
          && !g_pool.rec[ids[2].index].run_linked,
          "referenced reaped record is DEAD with its run link cleared");
    CHECK(memcmp(&live_before, &threads[4], sizeof live_before) == 0
          && g_pool.rec[live.index].state == JE_PROC_LIVE,
          "non-DYING peer slot and its record are untouched");

    unsigned destroys = g_destroy_calls, restores = g_page_restore_calls;
    r = -1;
    returned = reap_catching(&r);
    CHECK(returned && g_aborts == 0u && r == 0, "second call reaps nothing -> 0");
    CHECK(g_destroy_calls == destroys && g_page_restore_calls == restores,
          "second call destroys/restores nothing (no double free)");
    je_proc_release(&g_pool, ids[2]);
}

/* Extra: failures AFTER the slot was dropped are unrecoverable -> fail-stop. */
static void test_reap_post_drop_fail_stop(void)
{
    reset_fixture();
    (void)add_dying_user(1u, 1u);
    g_destroy_status = JE_ERR_INVALID;
    int r = 0;
    bool returned = reap_catching(&r);
    CHECK(!returned && g_aborts == 1u, "vm destroy failure is a fail-stop");
    CHECK(g_destroy_calls == 1u, "destroy was attempted once");

    reset_fixture();
    je_proc_id id = add_process(1u, 1u, false, DYING); /* record left LIVE */
    threads[1].vm = &g_vm;
    threads[1].root = SLOT_ROOT(1u);
    threads[1].rsp0_top = SLOT_RSP0(1u);
    returned = reap_catching(&r);
    CHECK(!returned && g_aborts == 1u,
          "on_reaped rejecting a non-DYING record is a fail-stop");
    CHECK(g_pool.rec[id.index].state == JE_PROC_LIVE,
          "the inconsistent record is not silently transitioned");
}

/* ---- F08: one user thread per (vm, index) ------------------------------------
 * je_thread_create_user_start must EXPLICITLY reject a second THREAD_USER for a
 * (vm, index) that already has one, BEFORE any side effect: no second slot, no
 * second guard of the (shared) held kstack, no page_restore of the first
 * thread's live guard page, no re-synthesized frame, no proc_link attempt.
 * The fake page_guard never fails (see above), so without the scan a duplicate
 * create on an UNREGISTERED process returns JE_OK with a second slot, and on a
 * REGISTERED one guards the kstack twice and then page_restore()s the FIRST
 * thread's guard page on the je_proc_link failure — the latter returns the same
 * JE_ERR_INVALID code, which is why every case asserts state, not just status. */
#define F08_KSTACK_BYTES ((size_t)JE_VM_KSTACK_PAGES * 4096u)
static _Alignas(4096) uint8_t g_f08_kstack[3][F08_KSTACK_BYTES];
static je_vm g_vm2;

static void f08_ready(je_vm *vm, uint32_t index, unsigned buf)
{
    je_vm_process *p = &vm->procs[index];
    p->state = JE_VM_READY;
    p->held_kstack = JE_VM_KSTACK_PAGES;
    p->root = (uint32_t)SLOT_ROOT(index + 1u);
    p->entry = 0x100u;
    p->kstack_physical = (uint64_t)(uintptr_t)g_f08_kstack[buf];
}

static unsigned used_slots(void)
{
    unsigned n = 0u;
    for (unsigned i = 1u; i < JE_THREAD_MAX; ++i)
        if (threads[i].state != EMPTY) ++n;
    return n;
}

/* Snapshot covering the scheduler/proc/registry state AND the held kstack
 * bytes (the synthesized first frame) the duplicate must not rewrite. */
static uint8_t g_f08_kstack_before[F08_KSTACK_BYTES];

static void check_duplicate_refused(je_vm *vm, uint32_t index, unsigned buf,
                                    uint32_t first_slot, const char *what)
{
    snapshot();
    memcpy(g_f08_kstack_before, g_f08_kstack[buf], F08_KSTACK_BYTES);
    unsigned guards = g_page_guard_calls, restores = g_page_restore_calls;
    unsigned used = used_slots();
    uint32_t out = UINT32_MAX;

    je_status s = je_thread_create_user_start(vm, index, (const je_user_startup *)0, &out);
    CHECK(s == JE_ERR_INVALID, what);
    CHECK(out == UINT32_MAX, "a refused duplicate create does not write out_slot");
    CHECK(used_slots() == used, "a refused duplicate create consumes no thread slot");
    CHECK(g_page_guard_calls == guards,
          "a refused duplicate create does not re-guard the held kstack");
    CHECK(g_page_restore_calls == restores,
          "a refused duplicate create never un-guards the first thread's kstack");
    CHECK(unchanged_since_snapshot(),
          "a refused duplicate create leaves threads/proc pool/registry unchanged");
    CHECK(memcmp(g_f08_kstack_before, g_f08_kstack[buf], F08_KSTACK_BYTES) == 0,
          "a refused duplicate create does not rewrite the first thread's frame");
    CHECK(threads[first_slot].kind == THREAD_USER && threads[first_slot].state == READY
          && threads[first_slot].vm == vm && threads[first_slot].vm_index == index,
          "the first thread for (vm,index) is untouched");
}

static void test_create_user_one_thread_per_process(void)
{
    reset_fixture();

    /* (A) REGISTERED process: the first create links the PROCESS record. */
    f08_ready(&g_vm, 2u, 0u);
    je_proc_id id = JE_PROC_ID_NONE;
    CHECK(je_proc_alloc(&g_pool, 0u, false, &id) == JE_OK, "F08 process allocation");
    CHECK(je_sched_register_process(&g_vm, 2u, id) == JE_OK, "F08 process registration");
    uint32_t s1 = UINT32_MAX;
    CHECK(je_thread_create_user_start(&g_vm, 2u, (const je_user_startup *)0, &s1) == JE_OK,
          "first user thread for (vm,2) is created");
    CHECK(s1 != UINT32_MAX && s1 >= 1u && s1 < JE_THREAD_MAX, "first create reports a slot");
    if (s1 == UINT32_MAX || s1 == 0u || s1 >= JE_THREAD_MAX) return;
    uintptr_t kbase0 = (uintptr_t)g_f08_kstack[0];
    CHECK(threads[s1].kind == THREAD_USER && threads[s1].state == READY
          && threads[s1].vm == &g_vm && threads[s1].vm_index == 2u
          && same_id(threads[s1].proc_id, id)
          && threads[s1].rsp0_top == (uint64_t)kbase0 + F08_KSTACK_BYTES,
          "first thread records (vm, index, PROCESS id, RSP0 top)");
    CHECK(g_page_guard_calls == 1u && g_page_guard_addr[0] == kbase0,
          "first create guards the held kstack base exactly once");
    CHECK(g_pool.rec[id.index].run_linked && g_pool.rec[id.index].thread_slot == s1
          && g_pool.rec[id.index].vm_index == 2u,
          "first create links the PROCESS record to its slot");
    CHECK(used_slots() == 1u, "exactly one user slot is in use");

    check_duplicate_refused(&g_vm, 2u, 0u, s1,
                            "second create for a REGISTERED (vm,2) is JE_ERR_INVALID");
    CHECK(g_pool.rec[id.index].run_linked && g_pool.rec[id.index].thread_slot == s1,
          "the PROCESS run link still names the first slot");

    /* (B) UNREGISTERED process (no proc_link to fail downstream): without the
     * scan a duplicate would return JE_OK with a second slot. */
    f08_ready(&g_vm, 3u, 1u);
    uint32_t s2 = UINT32_MAX;
    CHECK(je_thread_create_user_start(&g_vm, 3u, (const je_user_startup *)0, &s2) == JE_OK,
          "a DIFFERENT index (vm,3) still creates: the check is identity-scoped");
    CHECK(s2 != UINT32_MAX && s2 != s1 && s2 >= 1u && s2 < JE_THREAD_MAX,
          "the (vm,3) thread gets its own slot");
    if (s2 == UINT32_MAX || s2 == 0u || s2 >= JE_THREAD_MAX) return;
    CHECK(threads[s2].vm == &g_vm && threads[s2].vm_index == 3u
          && threads[s2].proc_id.gen == 0u,
          "the unregistered (vm,3) thread carries no PROCESS id");
    CHECK(g_page_guard_calls == 2u && g_page_guard_addr[1] == (uintptr_t)g_f08_kstack[1],
          "the (vm,3) create guards its own kstack");
    check_duplicate_refused(&g_vm, 3u, 1u, s2,
                            "second create for an UNREGISTERED (vm,3) is JE_ERR_INVALID");

    /* (C) Same index, DIFFERENT vm: the identity is the (vm,index) pair, not
     * the index alone. (Unregistered: the registry is per-index.) */
    f08_ready(&g_vm2, 2u, 2u);
    uint32_t s3 = UINT32_MAX;
    CHECK(je_thread_create_user_start(&g_vm2, 2u, (const je_user_startup *)0, &s3) == JE_OK,
          "same index on a DIFFERENT vm still creates");
    CHECK(s3 != UINT32_MAX && s3 != s1 && s3 != s2, "the (vm2,2) thread gets its own slot");
    CHECK(used_slots() == 3u, "three distinct (vm,index) identities -> three slots");
    CHECK(threads[s1].vm == &g_vm && threads[s1].vm_index == 2u
          && same_id(threads[s1].proc_id, id),
          "the first (vm,2) thread is still untouched after the other creates");
    memset(&g_vm2, 0, sizeof g_vm2);
}

int main(void)
{
    test_live_and_spare();
    test_dying_and_woken();
    test_fail_stop_no_change();
    test_reap_wrong_context();
    test_reap_if_enabled_fail_stop();
    test_reap_active_cr3();
    test_reap_active_rsp0();
    test_reap_drop_failure();
    test_reap_partial_progress();
    test_reap_success_counts();
    test_reap_post_drop_fail_stop();
    test_create_user_one_thread_per_process();
    if (g_failures != 0u) {
        fprintf(stderr, "test_sched_killall: FAIL (%u of %u checks)\n",
                g_failures, g_checks);
        return 1;
    }
    printf("test_sched_killall: PASS (%u checks)\n", g_checks);
    return 0;
}
