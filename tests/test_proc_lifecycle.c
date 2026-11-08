/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADR 0014 D3 neutral conformance suite: drives the extracted arch-neutral
 * process-lifecycle module (kernel/core/lifecycle.c) DIRECTLY, linked as its own
 * translation unit (not #included), against recording fakes for the endpoint
 * waiter release and the device death hooks, plus the REAL kernel/core/proc.c.
 *
 * This is the neutral-module binding of the shared conformance gate; the x86
 * binding is tests/test_lifecycle_order.c (which drives the same side effects
 * through sched.c's mark_dying). The ARM binding is an MP3 entry criterion
 * (there is no ARM user/lifecycle path yet).
 *
 * It pins, with its own mutants in tools/mutants.json:
 *   - je_lc_on_dying ORDER: release -> device fault-note (fault only) -> device
 *     dying -> je_proc_on_dying, and the record is still LIVE when the device
 *     hooks run (closes the M07 "on_dying moved before hooks" gap);
 *   - je_lc_on_dying propagates a je_proc_on_dying FAILURE (closes M05);
 *   - legacy (gen==0): release only, no hooks, no proc transition;
 *   - fault vs non-fault: fault-note only on JE_ABI_DEATH_FAULT;
 *   - registry writers: register idempotence / same-vm / wrong-vm unregister;
 *   - je_lc_kill_check D8 decisions (INVALID/ALREADY/OK) from the record alone;
 *   - je_lc_reaped order (destroy then on_reaped) and gen==0 skip.
 *
 * Compiled WITHOUT JE_SCHED_HOSTED_TEST so the device hooks inside je_lc_on_dying
 * are present and observed.
 */
#include <jeios/abi.h>
#include <jeios/device.h>
#include <jeios/lifecycle.h>
#include <jeios/proc.h>
#include <jeios/vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(cond, what)                                                       \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            fprintf(stderr, "test_proc_lifecycle: FAIL %s:%d: %s [%s]\n",       \
                    __FILE__, __LINE__, (what), #cond);                         \
        }                                                                       \
    } while (0)

/* ---- ordered event log ------------------------------------------------------ */
typedef enum {
    EV_RELEASE = 1, EV_DEV_FAULT_NOTE, EV_DEV_DYING
} ev_kind;
#define EV_MAX 32u
static ev_kind g_ev[EV_MAX];
static uint32_t g_ev_arg[EV_MAX];
static uint32_t g_ev_recstate[EV_MAX]; /* victim record state observed at the hook */
static unsigned g_ev_n;
static je_proc_pool *g_obs_pool;        /* so hooks can observe the record state */
static je_proc_id g_obs_victim;

static uint32_t record_state_of(je_proc_id id)
{
    uint32_t st = UINT32_MAX, rs = 0u; uint64_t code = 0u;
    if (g_obs_pool && je_proc_status(g_obs_pool, id, &st, &rs, &code) == JE_OK)
        return st;
    return UINT32_MAX;
}
static void ev_push(ev_kind k, uint32_t arg)
{
    if (g_ev_n < EV_MAX) {
        g_ev[g_ev_n] = k; g_ev_arg[g_ev_n] = arg;
        g_ev_recstate[g_ev_n] = record_state_of(g_obs_victim);
    }
    ++g_ev_n;
}
static int ev_index_of(ev_kind k)
{
    for (unsigned i = 0u; i < g_ev_n && i < EV_MAX; ++i) if (g_ev[i] == k) return (int)i;
    return -1;
}
static unsigned ev_count(ev_kind k)
{
    unsigned n = 0u;
    for (unsigned i = 0u; i < g_ev_n && i < EV_MAX; ++i) if (g_ev[i] == k) ++n;
    return n;
}

/* ---- fakes ------------------------------------------------------------------ */
static je_device g_dev;
je_device *je_kernel_device(void) { return &g_dev; }
void je_device_note_fault_after_admin_revoke(je_device *d, je_proc_id id, uint64_t v)
{
    (void)d; (void)v; ev_push(EV_DEV_FAULT_NOTE, id.index);
}
void je_device_on_process_dying(je_device *d, je_proc_id id)
{
    (void)d; ev_push(EV_DEV_DYING, id.index);
}
bool je_endpoint_kern_waiter_release_by_id(uint32_t slot)
{
    ev_push(EV_RELEASE, slot);
    return true;
}
/* je_vm_destroy_owned: record order relative to on_reaped via a counter. */
static unsigned g_destroy_seq;
static unsigned g_destroy_at;      /* g_destroy_seq when destroy ran */
static je_status g_destroy_status;
je_status je_vm_destroy_owned(je_vm *vm, uint32_t index, je_proc_id owner)
{
    (void)vm; (void)index; (void)owner;
    g_destroy_at = ++g_destroy_seq;
    return g_destroy_status;
}

static je_proc_pool g_pool;
static je_vm g_vm;

static void reset(void)
{
    je_proc_pool_init(&g_pool);
    g_obs_pool = &g_pool;
    g_obs_victim = JE_PROC_ID_NONE;
    memset(&g_dev, 0, sizeof g_dev);
    memset(&g_vm, 0, sizeof g_vm);
    g_ev_n = 0u;
    memset(g_ev, 0, sizeof g_ev);
    memset(g_ev_arg, 0, sizeof g_ev_arg);
    memset(g_ev_recstate, 0, sizeof g_ev_recstate);
    g_destroy_seq = 0u; g_destroy_at = 0u; g_destroy_status = JE_OK;
    je_sched_set_proc_pool(&g_pool);
}

static je_proc_id live_proc(bool supervisor)
{
    je_proc_id id = JE_PROC_ID_NONE;
    CHECK(je_proc_alloc(&g_pool, 0u, supervisor, &id) == JE_OK, "proc alloc");
    return id;
}

/* ===== registry writers ====================================================== */
static void test_registry(void)
{
    reset();
    je_proc_id id = live_proc(false);
    CHECK(je_sched_register_process(&g_vm, 2u, id) == JE_OK, "register ok");
    CHECK(je_lc_registered_vm(2u) == &g_vm, "registered vm readable");
    CHECK(je_lc_registered_proc(2u).index == id.index, "registered proc readable");
    CHECK(je_lc_proc_pool() == &g_pool && je_sched_proc_pool() == &g_pool,
          "proc pool getter + forwarder agree");
    /* same (vm,index) re-register with the same vm is allowed */
    CHECK(je_sched_register_process(&g_vm, 2u, id) == JE_OK, "same-vm re-register ok");
    /* a DIFFERENT vm at an occupied index is rejected */
    je_vm other; memset(&other, 0, sizeof other);
    je_proc_id id2 = live_proc(false);
    CHECK(je_sched_register_process(&other, 2u, id2) == JE_ERR_INVALID,
          "different vm at an occupied index is rejected");
    /* unregister with the wrong vm is a no-op OK; with the right vm clears it */
    CHECK(je_sched_unregister_process(&other, 2u) == JE_OK, "wrong-vm unregister no-op");
    CHECK(je_lc_registered_vm(2u) == &g_vm, "slot unchanged after wrong-vm unregister");
    CHECK(je_sched_unregister_process(&g_vm, 2u) == JE_OK, "right-vm unregister ok");
    CHECK(je_lc_registered_vm(2u) == (je_vm *)0, "slot cleared after unregister");
    CHECK(je_lc_registered_proc(2u).gen == 0u, "proc id cleared after unregister");
    /* out-of-range index */
    CHECK(je_lc_registered_vm(JE_VM_MAX_PROCESSES) == (je_vm *)0, "oob vm read is NULL");
    CHECK(je_sched_register_process(&g_vm, JE_VM_MAX_PROCESSES, id) == JE_ERR_INVALID,
          "oob register rejected");
}

/* ===== je_lc_on_dying: order + record-live-at-hooks + fault split =========== */
static void test_on_dying_fault(void)
{
    reset();
    je_proc_id v = live_proc(false);
    g_obs_victim = v;
    je_status s = je_lc_on_dying(3u, v, JE_ABI_DEATH_FAULT, 0u, 0xfeed);
    CHECK(s == JE_OK, "on_dying returns OK on a healthy fault death");
    int ir = ev_index_of(EV_RELEASE), ifn = ev_index_of(EV_DEV_FAULT_NOTE),
        idy = ev_index_of(EV_DEV_DYING);
    CHECK(ir == 0, "release is the FIRST side effect");
    CHECK(ifn > ir && idy > ifn, "order: release < fault-note < device-dying");
    CHECK(g_ev_arg[ir] == 3u, "release keyed on the thread slot");
    /* M07 guard: the record must still be LIVE when the device hooks run (the
     * proc transition happens AFTER the hooks). */
    CHECK(g_ev_recstate[ifn] == JE_PROC_LIVE,
          "record is LIVE when the fault-note hook runs (on_dying ran after)");
    CHECK(g_ev_recstate[idy] == JE_PROC_LIVE,
          "record is LIVE when the device-dying hook runs");
    CHECK(record_state_of(v) == JE_PROC_DYING, "record is DYING after on_dying");
}

static void test_on_dying_nonfault(void)
{
    reset();
    je_proc_id v = live_proc(false);
    g_obs_victim = v;
    CHECK(je_lc_on_dying(4u, v, JE_ABI_DEATH_KILLED, 0u, 0u) == JE_OK,
          "on_dying ok on a KILLED death");
    CHECK(ev_count(EV_DEV_FAULT_NOTE) == 0u,
          "a non-fault death does NOT call the fault-note hook");
    CHECK(ev_index_of(EV_RELEASE) == 0 && ev_index_of(EV_DEV_DYING) > 0,
          "release then device-dying on a non-fault death");
    CHECK(record_state_of(v) == JE_PROC_DYING, "record DYING after KILLED on_dying");
}

static void test_on_dying_legacy(void)
{
    reset();
    /* gen==0: a legacy thread with no PROCESS record. */
    CHECK(je_lc_on_dying(5u, JE_PROC_ID_NONE, JE_ABI_DEATH_FAULT, 0u, 0u) == JE_OK,
          "legacy on_dying returns OK");
    CHECK(ev_count(EV_RELEASE) == 1u, "legacy thread still releases its waiter");
    CHECK(ev_count(EV_DEV_FAULT_NOTE) == 0u && ev_count(EV_DEV_DYING) == 0u,
          "legacy thread skips BOTH device hooks");
}

/* M05 guard: a je_proc_on_dying FAILURE must propagate (not be swallowed). Drive
 * it by passing an out-of-range reason, which proc.c rejects. */
static void test_on_dying_failure_propagates(void)
{
    reset();
    je_proc_id v = live_proc(false);
    g_obs_victim = v;
    je_status s = je_lc_on_dying(3u, v, JE_PROC_REASON_MAX + 1u, 0u, 0u);
    CHECK(s != JE_OK, "an invalid reason makes je_proc_on_dying fail and propagate");
    CHECK(ev_count(EV_RELEASE) == 1u, "the waiter was still released before the failure");
}

/* ===== je_lc_kill_check ====================================================== */
static void test_kill_check(void)
{
    reset();
    je_proc_id caller = live_proc(false);
    je_proc_id victim = live_proc(false);
    uint32_t slot = 999u, vmi = 999u;

    /* not run-linked yet -> INVALID (LIVE but no thread) */
    CHECK(je_lc_kill_check(victim, caller, &slot, &vmi) == JE_LC_KILL_INVALID,
          "LIVE but unlinked victim is INVALID");
    /* link it, then OK with slot/vm_index */
    CHECK(je_proc_link(&g_pool, victim, 6u, 1u) == JE_OK, "link victim");
    CHECK(je_lc_kill_check(victim, caller, &slot, &vmi) == JE_LC_KILL_OK,
          "linked LIVE victim is OK");
    CHECK(slot == 6u && vmi == 1u, "kill_check returns the linked slot/vm_index");
    /* caller == victim -> INVALID */
    CHECK(je_lc_kill_check(victim, victim, &slot, &vmi) == JE_LC_KILL_INVALID,
          "self-kill is INVALID");
    /* supervisor victim -> INVALID */
    je_proc_id sup = live_proc(true);
    CHECK(je_proc_link(&g_pool, sup, 7u, 2u) == JE_OK, "link supervisor");
    CHECK(je_lc_kill_check(sup, caller, &slot, &vmi) == JE_LC_KILL_INVALID,
          "supervisor victim is INVALID");
    /* already DYING -> ALREADY */
    CHECK(je_proc_on_dying(&g_pool, victim, JE_ABI_DEATH_KILLED, 0u) == JE_OK,
          "transition victim to DYING");
    CHECK(je_lc_kill_check(victim, caller, &slot, &vmi) == JE_LC_KILL_ALREADY,
          "DYING victim is ALREADY (idempotent)");
}

/* ===== je_lc_reaped: destroy then on_reaped; gen==0 skip ===================== */
static void test_reaped(void)
{
    reset();
    je_proc_id v = live_proc(false);
    CHECK(je_proc_link(&g_pool, v, 6u, 1u) == JE_OK, "link");
    CHECK(je_proc_on_dying(&g_pool, v, JE_ABI_DEATH_KILLED, 0u) == JE_OK, "dying");
    /* reap requires the record be DYING; on_reaped moves it to DEAD/FREE */
    je_status s = je_lc_reaped(&g_vm, 1u, v);
    CHECK(s == JE_OK, "reaped ok");
    CHECK(g_destroy_at == 1u, "destroy_owned ran (seq 1) before on_reaped");
    CHECK(!je_proc_valid(&g_pool, v), "on_reaped ran: identity no longer valid");

    /* destroy failure propagates and on_reaped is NOT reached */
    reset();
    je_proc_id v2 = live_proc(false);
    CHECK(je_proc_link(&g_pool, v2, 6u, 1u) == JE_OK, "link2");
    CHECK(je_proc_on_dying(&g_pool, v2, JE_ABI_DEATH_KILLED, 0u) == JE_OK, "dying2");
    g_destroy_status = JE_ERR_INVALID;
    CHECK(je_lc_reaped(&g_vm, 1u, v2) == JE_ERR_INVALID, "destroy failure propagates");
    CHECK(je_proc_status(&g_pool, v2, &(uint32_t){0}, &(uint32_t){0}, &(uint64_t){0}) == JE_OK,
          "record still present (on_reaped not reached)");

    /* gen==0: destroy only, no on_reaped, returns OK */
    reset();
    CHECK(je_lc_reaped(&g_vm, 0u, JE_PROC_ID_NONE) == JE_OK, "legacy reaped ok");
    CHECK(g_destroy_at == 1u, "legacy reaped still destroys");
}

int main(void)
{
    test_registry();
    test_on_dying_fault();
    test_on_dying_nonfault();
    test_on_dying_legacy();
    test_on_dying_failure_propagates();
    test_kill_check();
    test_reaped();
    if (g_failures != 0u) {
        fprintf(stderr, "test_proc_lifecycle: FAIL (%u of %u checks)\n",
                g_failures, g_checks);
        return 1;
    }
    printf("test_proc_lifecycle: PASS (%u checks)\n", g_checks);
    return 0;
}
