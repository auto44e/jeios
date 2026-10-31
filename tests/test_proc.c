/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hosted suite for the ADR 0010 D4 / D-DEV PROCESS-record pool (kernel/core/proc.c).
 * Written from include/jeios/proc.h ONLY (the contract), not from the
 * implementation, so a defect in proc.c cannot also shape its check.
 *
 * Every failing call is checked for "status AND no change": the whole pool is
 * snapshotted with memcpy before the call and compared with memcmp after it.
 * Generation retirement needs a tiny JE_PROC_GEN_MAX applied to proc.c itself,
 * so it lives in a separate binary: tests/test_proc_retire.c.
 */
#include <jeios/proc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static je_proc_pool g_pool;
static je_proc_pool g_snap;   /* memcpy snapshot for "no change" checks */

static void snap(void) { memcpy(&g_snap, &g_pool, sizeof g_pool); }
static void assert_unchanged(void) { assert(memcmp(&g_snap, &g_pool, sizeof g_pool) == 0); }

static void fresh(void) { je_proc_pool_init(&g_pool); }

static je_proc_id mkid(uint32_t index, uint32_t gen)
{
    je_proc_id id = { index, gen };
    return id;
}

static bool id_eq(je_proc_id a, je_proc_id b) { return a.index == b.index && a.gen == b.gen; }

/* Allocate and assert OK, returning the identity. */
static je_proc_id alloc_ok(uint32_t ep_limit, bool supervisor)
{
    je_proc_id id = mkid(0xdeadu, 0xbeefu);
    assert(je_proc_alloc(&g_pool, ep_limit, supervisor, &id) == JE_OK);
    assert(id.gen != 0u && id.index < JE_PROC_REC_MAX);
    assert(je_proc_valid(&g_pool, id));
    return id;
}

static uint32_t state_of(je_proc_id id) { return g_pool.rec[id.index].state; }

/* Drive a LIVE record to FREE through the normal path (no handle refs). */
static void kill_and_reap(je_proc_id id, uint32_t reason, uint64_t code)
{
    assert(je_proc_on_dying(&g_pool, id, reason, code) == JE_OK);
    assert(je_proc_on_reaped(&g_pool, id) == JE_OK);
}

/* Every operation that takes an identity must reject `id` (invalid, stale or
 * NONE-where-not-special) with no change. charge/uncharge are excluded for NONE
 * by the caller because NONE is their documented "uncharged" value. */
static void assert_rejected_everywhere(je_proc_id id, bool include_charge)
{
    uint32_t st = 0xa1u, rs = 0xa2u; uint64_t cd = 0xa3u;
    assert(!je_proc_valid(&g_pool, id));
    snap();
    assert(je_proc_abandon(&g_pool, id) == JE_ERR_INVALID);            assert_unchanged();
    assert(je_proc_link(&g_pool, id, 1u, 2u) == JE_ERR_INVALID);       assert_unchanged();
    assert(je_proc_on_dying(&g_pool, id, 1u, 9u) == JE_ERR_INVALID);   assert_unchanged();
    assert(je_proc_on_reaped(&g_pool, id) == JE_ERR_INVALID);          assert_unchanged();
    assert(je_proc_retain(&g_pool, id) == JE_ERR_INVALID);             assert_unchanged();
    je_proc_release(&g_pool, id);                                      assert_unchanged();
    assert(je_proc_status(&g_pool, id, &st, &rs, &cd) == JE_ERR_INVALID); assert_unchanged();
    if (include_charge) {
        assert(je_proc_charge(&g_pool, id) == JE_ERR_INVALID);         assert_unchanged();
        je_proc_uncharge(&g_pool, id);                                 assert_unchanged();
    }
}

/* ---- init ------------------------------------------------------------------ */
static void test_init(void)
{
    memset(&g_pool, 0x5a, sizeof g_pool);   /* garbage: init must overwrite all */
    je_proc_pool_init(&g_pool);
    assert(g_pool.used_count == 0u);
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i) {
        const je_proc_rec *r = &g_pool.rec[i];
        assert(r->state == JE_PROC_FREE && r->gen == 0u && !r->retired);
        assert(r->handle_refs == 0u && r->ep_charge == 0u);
        assert(!je_proc_valid(&g_pool, mkid(i, 0u)));
        assert(!je_proc_valid(&g_pool, mkid(i, 1u)));
    }
    assert(!je_proc_valid(&g_pool, JE_PROC_ID_NONE));
    /* JE_PROC_ID_NONE is the all-zero identity */
    { je_proc_id n = JE_PROC_ID_NONE; assert(n.index == 0u && n.gen == 0u); }
    assert(JE_PROC_REC_MAX == 48u && JE_PROC_REF_MAX == 40u);
    assert(JE_PROC_GEN_MAX == 0xffffffffu);   /* production build of proc.c */
}

/* ---- alloc ----------------------------------------------------------------- */
static void test_alloc(void)
{
    fresh();
    /* lowest free first: 0, 1, 2 ... each at generation 1 */
    je_proc_id a = alloc_ok(5u, true);
    je_proc_id b = alloc_ok(0u, false);
    je_proc_id c = alloc_ok(7u, false);
    assert(id_eq(a, mkid(0u, 1u)) && id_eq(b, mkid(1u, 1u)) && id_eq(c, mkid(2u, 1u)));
    assert(g_pool.used_count == 3u);

    /* fields after alloc: LIVE, ep_limit/supervisor from the args, rest zero */
    {
        const je_proc_rec *r = &g_pool.rec[0];
        assert(r->state == JE_PROC_LIVE && r->gen == 1u && !r->retired);
        assert(r->ep_limit == 5u && r->supervisor);
        assert(r->reason == 0u && r->code == 0u && !r->run_linked);
        assert(r->thread_slot == 0u && r->vm_index == 0u);
        assert(r->handle_refs == 0u && r->ep_charge == 0u);
        assert(!g_pool.rec[1].supervisor && g_pool.rec[1].ep_limit == 0u);
        assert(g_pool.rec[2].ep_limit == 7u);
    }

    /* free the middle one; the next alloc takes index 1 again, generation 2,
     * and the old {1,1} identity no longer validates */
    kill_and_reap(b, 1u, 3u);
    assert(g_pool.used_count == 2u && state_of(b) == JE_PROC_FREE);
    assert(!je_proc_valid(&g_pool, b));
    je_proc_id b2 = alloc_ok(2u, false);
    assert(id_eq(b2, mkid(1u, 2u)));
    assert(!je_proc_valid(&g_pool, b) && je_proc_valid(&g_pool, b2));

    /* reuse zeroes everything the previous incarnation left behind */
    {
        je_proc_id d = alloc_ok(9u, true);              /* index 3 */
        assert(je_proc_link(&g_pool, d, 11u, 12u) == JE_OK);
        assert(je_proc_charge(&g_pool, d) == JE_OK);
        je_proc_uncharge(&g_pool, d);
        assert(je_proc_on_dying(&g_pool, d, 2u, 0x77u) == JE_OK);
        assert(je_proc_on_reaped(&g_pool, d) == JE_OK);
        assert(state_of(d) == JE_PROC_FREE);
        je_proc_id d2 = alloc_ok(0u, false);
        assert(id_eq(d2, mkid(3u, 2u)));
        const je_proc_rec *r = &g_pool.rec[3];
        assert(r->state == JE_PROC_LIVE && !r->supervisor && r->ep_limit == 0u);
        assert(r->reason == 0u && r->code == 0u && !r->run_linked);
        assert(r->thread_slot == 0u && r->vm_index == 0u);
        assert(r->handle_refs == 0u && r->ep_charge == 0u);
    }

    /* gen++ on every reuse of the same slot (several cycles) */
    fresh();
    for (uint32_t g = 1u; g <= 6u; ++g) {
        je_proc_id x = alloc_ok(0u, false);
        assert(id_eq(x, mkid(0u, g)));
        assert(!je_proc_valid(&g_pool, mkid(0u, g - 1u)));
        kill_and_reap(x, 1u, 0u);
        assert(g_pool.rec[0].gen == g);   /* free keeps the generation */
    }
    assert(g_pool.used_count == 0u);

    /* NULL arguments -> INVALID; NULL out changes nothing */
    fresh();
    snap();
    assert(je_proc_alloc(&g_pool, 1u, false, (je_proc_id *)0) == JE_ERR_INVALID);
    assert_unchanged();
    { je_proc_id o = mkid(1u, 1u);
      assert(je_proc_alloc((je_proc_pool *)0, 1u, false, &o) == JE_ERR_INVALID); }
    assert_unchanged();

    /* full pool -> NOMEM, *out = NONE, nothing changes */
    fresh();
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i) {
        je_proc_id x = alloc_ok(0u, false);
        assert(x.index == i && x.gen == 1u);
    }
    assert(g_pool.used_count == JE_PROC_REC_MAX);
    snap();
    { je_proc_id o = mkid(7u, 7u);
      assert(je_proc_alloc(&g_pool, 3u, true, &o) == JE_ERR_NOMEM);
      assert(id_eq(o, JE_PROC_ID_NONE)); }
    assert_unchanged();
    /* freeing the LAST slot makes exactly that slot allocatable again */
    kill_and_reap(mkid(JE_PROC_REC_MAX - 1u, 1u), 0u, 0u);
    assert(g_pool.used_count == JE_PROC_REC_MAX - 1u);
    assert(id_eq(alloc_ok(0u, false), mkid(JE_PROC_REC_MAX - 1u, 2u)));
    /* and a DEAD-but-referenced record still occupies its slot (not FREE) */
    {
        je_proc_id z = mkid(0u, 1u);
        assert(je_proc_retain(&g_pool, z) == JE_OK);
        kill_and_reap(z, 1u, 0u);
        assert(state_of(z) == JE_PROC_DEAD);
        je_proc_id o = mkid(7u, 7u);
        assert(je_proc_alloc(&g_pool, 0u, false, &o) == JE_ERR_NOMEM);
        assert(id_eq(o, JE_PROC_ID_NONE));
        je_proc_release(&g_pool, z);
        assert(state_of(z) == JE_PROC_FREE);
        assert(id_eq(alloc_ok(0u, false), mkid(0u, 2u)));
    }
}

/* ---- valid ----------------------------------------------------------------- */
static void test_valid(void)
{
    fresh();
    je_proc_id a = alloc_ok(0u, false);
    assert(je_proc_valid(&g_pool, a));
    assert(!je_proc_valid(&g_pool, JE_PROC_ID_NONE));            /* gen 0 */
    assert(!je_proc_valid(&g_pool, mkid(a.index, 0u)));          /* gen 0 on a live slot */
    assert(!je_proc_valid(&g_pool, mkid(a.index, a.gen + 1u)));  /* future gen */
    assert(!je_proc_valid(&g_pool, mkid(a.index, 0xffffffffu)));
    assert(!je_proc_valid(&g_pool, mkid(1u, 1u)));               /* FREE slot */
    assert(!je_proc_valid(&g_pool, mkid(JE_PROC_REC_MAX, 1u)));  /* index == max */
    assert(!je_proc_valid(&g_pool, mkid(0xffffffffu, 1u)));
    /* valid in every non-FREE state */
    assert(je_proc_retain(&g_pool, a) == JE_OK);
    assert(je_proc_on_dying(&g_pool, a, 0u, 0u) == JE_OK);
    assert(je_proc_valid(&g_pool, a));
    assert(je_proc_on_reaped(&g_pool, a) == JE_OK);
    assert(state_of(a) == JE_PROC_DEAD && je_proc_valid(&g_pool, a));
    je_proc_release(&g_pool, a);
    assert(!je_proc_valid(&g_pool, a));
    /* invalid identities are rejected by every op, with no change */
    assert_rejected_everywhere(mkid(JE_PROC_REC_MAX, 1u), true);
    assert_rejected_everywhere(mkid(0xffffffffu, 0xffffffffu), true);
    assert_rejected_everywhere(mkid(5u, 1u), true);              /* never allocated */
    assert_rejected_everywhere(a, true);                         /* freed */
    assert_rejected_everywhere(JE_PROC_ID_NONE, false);
}

/* ---- lifecycle and illegal transitions -------------------------------------- */
static void test_lifecycle(void)
{
    fresh();
    je_proc_id a = alloc_ok(0u, false);
    uint32_t st, rs; uint64_t cd;

    /* LIVE: on_reaped is illegal (not DYING) */
    snap();
    assert(je_proc_on_reaped(&g_pool, a) == JE_ERR_INVALID);
    assert_unchanged();

    /* link while LIVE */
    assert(je_proc_link(&g_pool, a, 4u, 6u) == JE_OK);
    assert(g_pool.rec[a.index].run_linked);
    assert(g_pool.rec[a.index].thread_slot == 4u && g_pool.rec[a.index].vm_index == 6u);
    /* re-link while still LIVE is allowed by the contract (LIVE is the only rule) */
    assert(je_proc_link(&g_pool, a, 5u, 7u) == JE_OK);
    assert(g_pool.rec[a.index].thread_slot == 5u && g_pool.rec[a.index].vm_index == 7u);

    /* LIVE -> DYING with {reason, code}; run link survives into DYING */
    assert(je_proc_on_dying(&g_pool, a, 1u, 0x1234567890abcdefu) == JE_OK);
    assert(state_of(a) == JE_PROC_DYING);
    assert(g_pool.rec[a.index].reason == 1u && g_pool.rec[a.index].code == 0x1234567890abcdefu);
    assert(g_pool.rec[a.index].run_linked);
    assert(je_proc_status(&g_pool, a, &st, &rs, &cd) == JE_OK);
    assert(st == JE_PROC_DYING && rs == 1u && cd == 0x1234567890abcdefu);

    /* DYING: first terminal reason wins (OK, no change) */
    snap();
    assert(je_proc_on_dying(&g_pool, a, 3u, 99u) == JE_OK);
    assert_unchanged();
    /* DYING: link and abandon are illegal */
    assert(je_proc_link(&g_pool, a, 8u, 9u) == JE_ERR_INVALID);
    assert_unchanged();
    assert(je_proc_abandon(&g_pool, a) == JE_ERR_INVALID);
    assert_unchanged();

    /* keep a handle so DEAD is observable */
    assert(je_proc_retain(&g_pool, a) == JE_OK);
    /* DYING -> DEAD, run link cleared, record NOT freed (refs 1) */
    assert(je_proc_on_reaped(&g_pool, a) == JE_OK);
    assert(state_of(a) == JE_PROC_DEAD && !g_pool.rec[a.index].run_linked);
    assert(g_pool.used_count == 1u);
    assert(je_proc_status(&g_pool, a, &st, &rs, &cd) == JE_OK);
    assert(st == JE_PROC_DEAD && rs == 1u && cd == 0x1234567890abcdefu);  /* reason survives */

    /* DEAD: every forward/backward transition is illegal or a no-op */
    snap();
    assert(je_proc_on_dying(&g_pool, a, 0u, 1u) == JE_OK);      /* first reason wins */
    assert_unchanged();
    assert(je_proc_on_reaped(&g_pool, a) == JE_ERR_INVALID);    /* double reap */
    assert_unchanged();
    assert(je_proc_link(&g_pool, a, 1u, 1u) == JE_ERR_INVALID);
    assert_unchanged();
    assert(je_proc_abandon(&g_pool, a) == JE_ERR_INVALID);
    assert_unchanged();

    /* DEAD -> FREE on the last release */
    je_proc_release(&g_pool, a);
    assert(state_of(a) == JE_PROC_FREE && g_pool.used_count == 0u);
    assert(g_pool.rec[a.index].gen == 1u);   /* generation kept until next alloc */
    assert_rejected_everywhere(a, true);

    /* on_reaped with refs == 0 frees immediately (DYING -> DEAD -> FREE) */
    je_proc_id b = alloc_ok(0u, false);
    assert(id_eq(b, mkid(0u, 2u)));
    assert(je_proc_link(&g_pool, b, 1u, 1u) == JE_OK);
    assert(je_proc_on_dying(&g_pool, b, 0u, 0u) == JE_OK);
    assert(je_proc_on_reaped(&g_pool, b) == JE_OK);
    assert(state_of(b) == JE_PROC_FREE && !g_pool.rec[b.index].run_linked);
    assert(g_pool.used_count == 0u);
    assert_rejected_everywhere(b, true);

    /* every death reason is recorded verbatim (FAULT 0, EXIT 1, BAD_RETURN 2,
     * KILLED 3); code is 64-bit */
    fresh();
    for (uint32_t reason = 0u; reason <= 3u; ++reason) {
        je_proc_id x = alloc_ok(0u, false);
        uint64_t code = (reason == 1u) ? UINT64_MAX : 0u;
        assert(je_proc_on_dying(&g_pool, x, reason, code) == JE_OK);
        assert(je_proc_status(&g_pool, x, &st, &rs, &cd) == JE_OK);
        assert(st == JE_PROC_DYING && rs == reason && cd == code);
    }
}

/* ---- handle refs ----------------------------------------------------------- */
static void test_refs(void)
{
    fresh();
    je_proc_id a = alloc_ok(0u, false);
    /* retain up to the bound */
    for (uint32_t k = 0; k < JE_PROC_REF_MAX; ++k)
        assert(je_proc_retain(&g_pool, a) == JE_OK);
    assert(g_pool.rec[a.index].handle_refs == JE_PROC_REF_MAX);
    snap();
    assert(je_proc_retain(&g_pool, a) == JE_ERR_BUSY);
    assert_unchanged();
    /* the bound also holds in DYING and DEAD */
    assert(je_proc_on_dying(&g_pool, a, 1u, 0u) == JE_OK);
    snap();
    assert(je_proc_retain(&g_pool, a) == JE_ERR_BUSY);
    assert_unchanged();
    assert(je_proc_on_reaped(&g_pool, a) == JE_OK);
    assert(state_of(a) == JE_PROC_DEAD);
    snap();
    assert(je_proc_retain(&g_pool, a) == JE_ERR_BUSY);
    assert_unchanged();
    /* release one at a time; DEAD stays until the very last */
    for (uint32_t k = JE_PROC_REF_MAX; k > 1u; --k) {
        je_proc_release(&g_pool, a);
        assert(state_of(a) == JE_PROC_DEAD && g_pool.rec[a.index].handle_refs == k - 1u);
        assert(je_proc_valid(&g_pool, a) && g_pool.used_count == 1u);
    }
    je_proc_release(&g_pool, a);
    assert(state_of(a) == JE_PROC_FREE && g_pool.used_count == 0u);
    /* release after free: no-op, no underflow */
    snap();
    je_proc_release(&g_pool, a);
    assert_unchanged();

    /* LIVE: release to 0 does NOT free (only DEAD frees on last release) */
    je_proc_id b = alloc_ok(0u, false);
    assert(je_proc_retain(&g_pool, b) == JE_OK);
    je_proc_release(&g_pool, b);
    assert(state_of(b) == JE_PROC_LIVE && g_pool.rec[b.index].handle_refs == 0u);
    /* release at 0 refs: no-op, no underflow */
    snap();
    je_proc_release(&g_pool, b);
    assert_unchanged();
    assert(g_pool.rec[b.index].handle_refs == 0u);
    /* DYING: release to 0 does not free either; reap then frees (refs 0) */
    assert(je_proc_retain(&g_pool, b) == JE_OK);
    assert(je_proc_on_dying(&g_pool, b, 0u, 0u) == JE_OK);
    je_proc_release(&g_pool, b);
    assert(state_of(b) == JE_PROC_DYING && g_pool.used_count == 1u);
    snap();
    je_proc_release(&g_pool, b);   /* 0 refs in DYING: no-op */
    assert_unchanged();
    assert(je_proc_on_reaped(&g_pool, b) == JE_OK);
    assert(state_of(b) == JE_PROC_FREE && g_pool.used_count == 0u);

    /* retain on a DEAD record is legal (a DUP of a PROCESS handle to a dead child) */
    je_proc_id c = alloc_ok(0u, false);
    assert(je_proc_retain(&g_pool, c) == JE_OK);
    kill_and_reap(c, 1u, 4u);
    assert(je_proc_retain(&g_pool, c) == JE_OK);
    assert(g_pool.rec[c.index].handle_refs == 2u);
    je_proc_release(&g_pool, c);
    assert(state_of(c) == JE_PROC_DEAD);
    je_proc_release(&g_pool, c);
    assert(state_of(c) == JE_PROC_FREE);

    /* refs are per record: releasing one record never touches another */
    fresh();
    je_proc_id p = alloc_ok(0u, false), q = alloc_ok(0u, false);
    assert(je_proc_retain(&g_pool, p) == JE_OK);
    assert(je_proc_retain(&g_pool, q) == JE_OK);
    je_proc_release(&g_pool, p);
    assert(g_pool.rec[p.index].handle_refs == 0u && g_pool.rec[q.index].handle_refs == 1u);
}

/* ---- stale identity after reuse --------------------------------------------- */
static void test_stale_after_reuse(void)
{
    fresh();
    je_proc_id old = alloc_ok(3u, false);
    kill_and_reap(old, 1u, 0u);
    je_proc_id cur = alloc_ok(3u, false);
    assert(cur.index == old.index && cur.gen == old.gen + 1u);
    /* give the new incarnation state the old identity could disturb */
    assert(je_proc_retain(&g_pool, cur) == JE_OK);
    assert(je_proc_charge(&g_pool, cur) == JE_OK);
    assert(je_proc_link(&g_pool, cur, 2u, 3u) == JE_OK);
    assert_rejected_everywhere(old, true);
    /* the new incarnation is untouched by all of that */
    const je_proc_rec *r = &g_pool.rec[cur.index];
    assert(r->state == JE_PROC_LIVE && r->handle_refs == 1u && r->ep_charge == 1u);
    assert(r->run_linked && r->thread_slot == 2u && r->vm_index == 3u);
    /* stale in every state of the new incarnation */
    assert(je_proc_on_dying(&g_pool, cur, 2u, 0u) == JE_OK);
    assert_rejected_everywhere(old, true);
    assert(je_proc_on_reaped(&g_pool, cur) == JE_OK);
    assert(state_of(cur) == JE_PROC_DEAD);
    assert_rejected_everywhere(old, true);
    /* and a later incarnation's identity (gen + 1) is not valid yet either */
    assert_rejected_everywhere(mkid(cur.index, cur.gen + 1u), true);
}

/* ---- abandon (spawn rollback) ----------------------------------------------- */
static void test_abandon(void)
{
    fresh();
    je_proc_id a = alloc_ok(2u, false);
    assert(g_pool.used_count == 1u);
    assert(je_proc_abandon(&g_pool, a) == JE_OK);
    assert(state_of(a) == JE_PROC_FREE && g_pool.used_count == 0u);
    assert(g_pool.rec[a.index].gen == a.gen);       /* keeps its generation */
    assert(!je_proc_valid(&g_pool, a));
    snap();
    assert(je_proc_abandon(&g_pool, a) == JE_ERR_INVALID);   /* double abandon */
    assert_unchanged();
    /* next alloc of that slot bumps the generation */
    je_proc_id a2 = alloc_ok(0u, false);
    assert(a2.index == a.index && a2.gen == a.gen + 1u);

    /* refused with a handle ref outstanding */
    assert(je_proc_retain(&g_pool, a2) == JE_OK);
    snap();
    assert(je_proc_abandon(&g_pool, a2) == JE_ERR_INVALID);
    assert_unchanged();
    je_proc_release(&g_pool, a2);
    /* refused with a creation charge outstanding */
    fresh();
    je_proc_id b = alloc_ok(1u, false);
    assert(je_proc_charge(&g_pool, b) == JE_OK);
    snap();
    assert(je_proc_abandon(&g_pool, b) == JE_ERR_INVALID);
    assert_unchanged();
    je_proc_uncharge(&g_pool, b);
    assert(je_proc_abandon(&g_pool, b) == JE_OK);
    /* refused in DYING and DEAD (covered in test_lifecycle) and when FREE/stale */
    assert_rejected_everywhere(b, true);
    assert_rejected_everywhere(mkid(JE_PROC_REC_MAX, 1u), true);
}

/* ---- status snapshot ---------------------------------------------------------- */
static void test_status(void)
{
    fresh();
    je_proc_id a = alloc_ok(0u, false);
    uint32_t st = 0xffu, rs = 0xffu; uint64_t cd = 0xffu;
    snap();
    assert(je_proc_status(&g_pool, a, &st, &rs, &cd) == JE_OK);
    assert_unchanged();                                     /* read-only */
    assert(st == JE_PROC_LIVE && rs == 0u && cd == 0u);     /* reason 0 while LIVE */
    /* NULL outs: each -> INVALID, nothing changes */
    assert(je_proc_status(&g_pool, a, (uint32_t *)0, &rs, &cd) == JE_ERR_INVALID);
    assert(je_proc_status(&g_pool, a, &st, (uint32_t *)0, &cd) == JE_ERR_INVALID);
    assert(je_proc_status(&g_pool, a, &st, &rs, (uint64_t *)0) == JE_ERR_INVALID);
    assert(je_proc_status(&g_pool, a, (uint32_t *)0, (uint32_t *)0, (uint64_t *)0) == JE_ERR_INVALID);
    assert_unchanged();
    /* states map 1:1 onto the ABI PROC_STATUS values */
    assert(JE_PROC_LIVE == 1 && JE_PROC_DYING == 2 && JE_PROC_DEAD == 3 && JE_PROC_FREE == 0);
    /* EXIT 7, as the M4b-s1 fixture will observe */
    assert(je_proc_retain(&g_pool, a) == JE_OK);
    assert(je_proc_on_dying(&g_pool, a, 1u, 7u) == JE_OK);
    assert(je_proc_status(&g_pool, a, &st, &rs, &cd) == JE_OK && st == JE_PROC_DYING && rs == 1u && cd == 7u);
    assert(je_proc_on_reaped(&g_pool, a) == JE_OK);
    assert(je_proc_status(&g_pool, a, &st, &rs, &cd) == JE_OK && st == JE_PROC_DEAD && rs == 1u && cd == 7u);
    je_proc_release(&g_pool, a);
    /* FREE is never observable */
    st = 0xeeu; rs = 0xeeu; cd = 0xeeu;
    assert(je_proc_status(&g_pool, a, &st, &rs, &cd) == JE_ERR_INVALID);
}

/* ---- D-DEV creation charge ---------------------------------------------------- */
static void test_charge(void)
{
    fresh();
    je_proc_id a = alloc_ok(3u, false);
    for (uint32_t k = 1u; k <= 3u; ++k) {
        assert(je_proc_charge(&g_pool, a) == JE_OK);
        assert(g_pool.rec[a.index].ep_charge == k);
    }
    snap();
    assert(je_proc_charge(&g_pool, a) == JE_ERR_DENIED);   /* at the limit */
    assert_unchanged();
    je_proc_uncharge(&g_pool, a);
    assert(g_pool.rec[a.index].ep_charge == 2u);
    assert(je_proc_charge(&g_pool, a) == JE_OK);           /* room again */
    assert(je_proc_charge(&g_pool, a) == JE_ERR_DENIED);

    /* limit 0: the very first charge is DENIED (M4 children, D-DEV) */
    je_proc_id z = alloc_ok(0u, false);
    snap();
    assert(je_proc_charge(&g_pool, z) == JE_ERR_DENIED);
    assert_unchanged();
    /* uncharge at 0 never underflows */
    je_proc_uncharge(&g_pool, z);
    assert_unchanged();

    /* NONE is "uncharged": charge OK, uncharge no-op, nothing changes */
    snap();
    assert(je_proc_charge(&g_pool, JE_PROC_ID_NONE) == JE_OK);
    assert_unchanged();
    je_proc_uncharge(&g_pool, JE_PROC_ID_NONE);
    assert_unchanged();
    /* NONE must not alias record 0 (which IS allocated here, index 0) */
    assert(a.index == 0u && g_pool.rec[0].ep_charge == 3u);

    /* uncharge works on the same identity in DYING and in DEAD */
    assert(je_proc_retain(&g_pool, a) == JE_OK);
    assert(je_proc_on_dying(&g_pool, a, 1u, 0u) == JE_OK);
    je_proc_uncharge(&g_pool, a);
    assert(g_pool.rec[a.index].ep_charge == 2u);
    assert(je_proc_on_reaped(&g_pool, a) == JE_OK);
    assert(state_of(a) == JE_PROC_DEAD);
    je_proc_uncharge(&g_pool, a);
    assert(g_pool.rec[a.index].ep_charge == 1u);
    /* a stale (wrong-gen) identity on the same index never decrements */
    snap();
    je_proc_uncharge(&g_pool, mkid(a.index, a.gen + 1u));
    je_proc_uncharge(&g_pool, mkid(a.index, 0u));
    je_proc_uncharge(&g_pool, mkid(JE_PROC_REC_MAX, a.gen));
    assert_unchanged();

    /* after free: no-op; after reuse: the new identity's charge is untouched */
    fresh();
    je_proc_id old = alloc_ok(2u, false);
    assert(je_proc_charge(&g_pool, old) == JE_OK);
    /* (abandon is refused with a charge, so free via death + reap) */
    kill_and_reap(old, 0u, 0u);
    assert(state_of(old) == JE_PROC_FREE);
    snap();
    je_proc_uncharge(&g_pool, old);
    assert_unchanged();
    je_proc_id nw = alloc_ok(2u, false);
    assert(nw.index == old.index && nw.gen == old.gen + 1u);
    assert(g_pool.rec[nw.index].ep_charge == 0u);           /* reuse zeroed the charge */
    assert(je_proc_charge(&g_pool, nw) == JE_OK);
    snap();
    je_proc_uncharge(&g_pool, old);                         /* the old channel dies */
    assert_unchanged();
    assert(g_pool.rec[nw.index].ep_charge == 1u);
    /* charge with a stale identity: INVALID, no change */
    assert(je_proc_charge(&g_pool, old) == JE_ERR_INVALID);
    assert_unchanged();
}

/* ---- used_count accounting ---------------------------------------------------- */
static void test_used_count(void)
{
    fresh();
    je_proc_id ids[JE_PROC_REC_MAX];
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i) {
        ids[i] = alloc_ok(0u, false);
        assert(g_pool.used_count == i + 1u);
    }
    /* mix: abandon evens below 8, reap-with-refs (DEAD, still counted) odds */
    uint32_t expect = JE_PROC_REC_MAX;
    for (uint32_t i = 0; i < 8u; ++i) {
        if ((i & 1u) == 0u) {
            assert(je_proc_abandon(&g_pool, ids[i]) == JE_OK);
            --expect;
        } else {
            assert(je_proc_retain(&g_pool, ids[i]) == JE_OK);
            kill_and_reap(ids[i], 1u, 0u);
            assert(state_of(ids[i]) == JE_PROC_DEAD);   /* DEAD still counts */
        }
        assert(g_pool.used_count == expect);
    }
    /* DYING still counts */
    assert(je_proc_on_dying(&g_pool, ids[8], 0u, 0u) == JE_OK);
    assert(g_pool.used_count == expect);
    /* last release of each DEAD odd frees it */
    for (uint32_t i = 1u; i < 8u; i += 2u) {
        je_proc_release(&g_pool, ids[i]);
        --expect;
        assert(g_pool.used_count == expect);
    }
    /* failing ops never move the count */
    snap();
    assert(je_proc_alloc(&g_pool, 0u, false, (je_proc_id *)0) == JE_ERR_INVALID);
    assert(je_proc_on_reaped(&g_pool, ids[9]) == JE_ERR_INVALID);   /* LIVE */
    assert(je_proc_abandon(&g_pool, ids[8]) == JE_ERR_INVALID);     /* DYING */
    assert_unchanged();
    /* used_count equals the number of non-FREE records */
    uint32_t n = 0;
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i)
        n += (g_pool.rec[i].state != JE_PROC_FREE) ? 1u : 0u;
    assert(n == g_pool.used_count);
}

/* ---- pool isolation: two pools never share state ------------------------------- */
static void test_two_pools(void)
{
    static je_proc_pool other;
    fresh();
    je_proc_pool_init(&other);
    je_proc_id a = alloc_ok(1u, false);
    assert(!je_proc_valid(&other, a));
    assert(je_proc_charge(&other, a) == JE_ERR_INVALID);
    assert(g_pool.rec[a.index].ep_charge == 0u && other.used_count == 0u);
}

int main(void)
{
    test_init();
    test_alloc();
    test_valid();
    test_lifecycle();
    test_refs();
    test_stale_after_reuse();
    test_abandon();
    test_status();
    test_charge();
    test_used_count();
    test_two_pools();
    printf("proc: PASS (init, alloc lowest-free/gen++/NOMEM/NULL, valid, lifecycle "
           "LIVE->DYING->DEAD->FREE + illegal transitions, first reason wins, "
           "reap frees iff refs==0, REF_MAX BUSY, no release underflow, stale after "
           "reuse, abandon, link LIVE-only, status, charge/uncharge identity, "
           "used_count)\n");
    return 0;
}
