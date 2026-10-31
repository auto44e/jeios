/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADR 0010 D4 generation retirement for the PROCESS-record pool. Written from
 * include/jeios/proc.h only. Retirement is unreachable at the production
 * JE_PROC_GEN_MAX (2^32 - 1 reuses of a slot), so this binary is built with a
 * tiny JE_PROC_GEN_MAX applied to BOTH this file and kernel/core/proc.c (the
 * header's #ifndef override). Required Makefile rule (lead wires it):
 *
 *   $(BUILD)/test_proc_retire: tests/test_proc_retire.c kernel/core/proc.c $(HDRS)
 *   	@mkdir -p $(@D)
 *   	$(CC) $(CPPFLAGS) -DJE_PROC_GEN_MAX=3u -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_proc_retire.c kernel/core/proc.c -o $@
 *
 * where $(HDRS) stands for the same include/jeios header wildcard the test_proc
 * rule uses (spelled out it would close this comment), plus
 * $(BUILD)/test_proc_retire in the `test:` prerequisites and its run line.
 */
#ifndef JE_PROC_GEN_MAX
#error "build with -DJE_PROC_GEN_MAX=3u for proc.c AND this file (see header comment)"
#endif
#include <jeios/proc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(JE_PROC_GEN_MAX == 3u, "this suite assumes JE_PROC_GEN_MAX == 3");

static je_proc_pool g_pool;
static je_proc_pool g_snap;

static je_proc_id mkid(uint32_t index, uint32_t gen)
{
    je_proc_id id = { index, gen };
    return id;
}

static je_proc_id alloc_ok(void)
{
    je_proc_id id = mkid(0xdeadu, 0xbeefu);
    assert(je_proc_alloc(&g_pool, 1u, false, &id) == JE_OK);
    assert(id.gen >= 1u && id.gen <= JE_PROC_GEN_MAX);
    return id;
}

static void free_it(je_proc_id id)
{
    assert(je_proc_on_dying(&g_pool, id, 1u, 0u) == JE_OK);
    assert(je_proc_on_reaped(&g_pool, id) == JE_OK);
    assert(g_pool.rec[id.index].state == JE_PROC_FREE);
}

/* Field-by-field record equality (no memcmp: struct padding is unspecified). */
static bool rec_eq(const je_proc_rec *a, const je_proc_rec *b)
{
    return a->state == b->state && a->gen == b->gen && a->retired == b->retired
        && a->reason == b->reason && a->code == b->code
        && a->run_linked == b->run_linked && a->thread_slot == b->thread_slot
        && a->vm_index == b->vm_index && a->supervisor == b->supervisor
        && a->handle_refs == b->handle_refs && a->ep_charge == b->ep_charge
        && a->ep_limit == b->ep_limit;
}

static uint32_t count_not_free(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i)
        n += (g_pool.rec[i].state != JE_PROC_FREE) ? 1u : 0u;
    return n;
}

/* Free slot `idx` (currently LIVE at some gen) and re-allocate it until it is LIVE
 * at JE_PROC_GEN_MAX. Requires every other slot to be non-FREE, so the lowest FREE
 * non-retired slot is always `idx`. */
static je_proc_id cycle_to_last_gen(je_proc_id id)
{
    while (id.gen < JE_PROC_GEN_MAX) {
        uint32_t idx = id.index;
        free_it(id);
        id = alloc_ok();
        assert(id.index == idx);
    }
    assert(id.gen == JE_PROC_GEN_MAX);
    return id;
}

/* proc.h pinned [Codex P2-8, Claude A9]: a failing (NOMEM) search may retire the
 * slots it passes over, and NOTHING ELSE changes. Built so the failing search
 * passes two FREE slots at the last generation (0 and 5), one slot retired
 * earlier (10), and records in every non-FREE state with nonzero link, charge,
 * refs, reason and code fields, so a NOMEM path that touched any of them fails. */
static void test_retire_during_failing_search(void)
{
    je_proc_id ids[JE_PROC_REC_MAX];
    je_proc_pool_init(&g_pool);
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i) {
        ids[i] = alloc_ok();
        assert(ids[i].index == i && ids[i].gen == 1u);
    }
    ids[5]  = cycle_to_last_gen(ids[5]);
    ids[0]  = cycle_to_last_gen(ids[0]);
    ids[10] = cycle_to_last_gen(ids[10]);
    /* retire slot 10 in a SUCCESSFUL search: free 10 (last gen) and 11; the next
     * alloc retires 10 and lands on 11 at gen 2 */
    free_it(ids[10]);
    free_it(ids[11]);
    ids[11] = alloc_ok();
    assert(ids[11].index == 11u && ids[11].gen == 2u);
    assert(g_pool.rec[10].retired && g_pool.rec[10].state == JE_PROC_FREE);

    /* rich non-FREE state */
    assert(je_proc_link(&g_pool, ids[20], 3u, 4u) == JE_OK);
    assert(je_proc_on_dying(&g_pool, ids[20], 2u, 0x55u) == JE_OK);              /* DYING, linked */
    assert(je_proc_retain(&g_pool, ids[21]) == JE_OK);
    assert(je_proc_link(&g_pool, ids[21], 5u, 6u) == JE_OK);
    assert(je_proc_on_dying(&g_pool, ids[21], 1u, 0xffffffffffffffffu) == JE_OK);
    assert(je_proc_on_reaped(&g_pool, ids[21]) == JE_OK);                        /* DEAD, ref 1 */
    assert(g_pool.rec[21].state == JE_PROC_DEAD);
    assert(je_proc_link(&g_pool, ids[22], 7u, 8u) == JE_OK);                     /* LIVE, linked */
    assert(je_proc_charge(&g_pool, ids[22]) == JE_OK);                           /* charged 1/1 */
    assert(je_proc_retain(&g_pool, ids[22]) == JE_OK);
    assert(je_proc_retain(&g_pool, ids[22]) == JE_OK);                           /* refs 2 */

    /* the two last-generation slots go FREE, not yet retired */
    free_it(ids[0]);
    free_it(ids[5]);
    assert(!g_pool.rec[0].retired && g_pool.rec[0].gen == JE_PROC_GEN_MAX);
    assert(!g_pool.rec[5].retired && g_pool.rec[5].gen == JE_PROC_GEN_MAX);
    assert(g_pool.used_count == JE_PROC_REC_MAX - 3u);   /* 0, 5, 10 FREE */
    assert(count_not_free() == g_pool.used_count);

    memcpy(&g_snap, &g_pool, sizeof g_pool);
    {
        je_proc_id o = mkid(9u, 9u);
        assert(je_proc_alloc(&g_pool, 4u, true, &o) == JE_ERR_NOMEM);
        assert(o.index == 0u && o.gen == 0u);
    }
    /* expected: the snapshot with exactly slots 0 and 5 newly retired */
    je_proc_pool expect;
    memcpy(&expect, &g_snap, sizeof expect);
    expect.rec[0].retired = true;
    expect.rec[5].retired = true;
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i)
        assert(rec_eq(&expect.rec[i], &g_pool.rec[i]));
    assert(g_pool.used_count == expect.used_count);
    /* retired slots are FREE, keep their generation, and are not in used_count */
    assert(g_pool.rec[0].state == JE_PROC_FREE && g_pool.rec[0].gen == JE_PROC_GEN_MAX);
    assert(g_pool.rec[5].state == JE_PROC_FREE && g_pool.rec[5].gen == JE_PROC_GEN_MAX);
    assert(count_not_free() == g_pool.used_count);

    /* a second failing search has nothing left to retire: nothing changes */
    memcpy(&g_snap, &g_pool, sizeof g_pool);
    {
        je_proc_id o = mkid(9u, 9u);
        assert(je_proc_alloc(&g_pool, 0u, false, &o) == JE_ERR_NOMEM);
        assert(o.index == 0u && o.gen == 0u);
    }
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i)
        assert(rec_eq(&g_snap.rec[i], &g_pool.rec[i]));
    assert(g_pool.used_count == g_snap.used_count);

    /* freeing a never-retired slot makes it (and only it) allocatable again */
    free_it(ids[30]);
    je_proc_id n = alloc_ok();
    assert(n.index == 30u && n.gen == 2u);
}

int main(void)
{
    je_proc_pool_init(&g_pool);

    /* Slot 0 issues exactly generations 1..GEN_MAX, then retires. */
    je_proc_id seen[JE_PROC_GEN_MAX];
    for (uint32_t g = 1u; g <= JE_PROC_GEN_MAX; ++g) {
        je_proc_id x = alloc_ok();
        assert(x.index == 0u && x.gen == g);
        seen[g - 1u] = x;
        free_it(x);
    }
    /* The next alloc must skip slot 0 (its next generation would exceed the max)
     * and land on slot 1 at generation 1; slot 0 is now retired and FREE. */
    je_proc_id y = alloc_ok();
    assert(y.index == 1u && y.gen == 1u);
    assert(g_pool.rec[0].retired);
    assert(g_pool.rec[0].state == JE_PROC_FREE);
    assert(g_pool.rec[0].gen == JE_PROC_GEN_MAX);       /* never wrapped */
    assert(g_pool.used_count == 1u);                     /* retired is not "used" */

    /* Every identity the retired slot ever issued, and gen 0 / gen max+1 beyond,
     * is rejected by every op. */
    for (uint32_t k = 0; k < JE_PROC_GEN_MAX; ++k) {
        je_proc_id s = seen[k];
        uint32_t st, rs; uint64_t cd;
        assert(!je_proc_valid(&g_pool, s));
        memcpy(&g_snap, &g_pool, sizeof g_pool);
        assert(je_proc_retain(&g_pool, s) == JE_ERR_INVALID);
        assert(je_proc_link(&g_pool, s, 0u, 0u) == JE_ERR_INVALID);
        assert(je_proc_on_dying(&g_pool, s, 0u, 0u) == JE_ERR_INVALID);
        assert(je_proc_on_reaped(&g_pool, s) == JE_ERR_INVALID);
        assert(je_proc_abandon(&g_pool, s) == JE_ERR_INVALID);
        assert(je_proc_status(&g_pool, s, &st, &rs, &cd) == JE_ERR_INVALID);
        assert(je_proc_charge(&g_pool, s) == JE_ERR_INVALID);
        je_proc_release(&g_pool, s);
        je_proc_uncharge(&g_pool, s);
        assert(memcmp(&g_snap, &g_pool, sizeof g_pool) == 0);
    }
    assert(!je_proc_valid(&g_pool, mkid(0u, 0u)));
    assert(!je_proc_valid(&g_pool, mkid(0u, JE_PROC_GEN_MAX + 1u)));

    /* A retired slot stays skipped forever: free slot 1 and reallocate; the
     * lowest non-retired FREE slot is 1 again (gen 2), never 0. */
    free_it(y);
    je_proc_id y2 = alloc_ok();
    assert(y2.index == 1u && y2.gen == 2u);
    assert(g_pool.rec[0].retired && g_pool.rec[0].state == JE_PROC_FREE);

    /* An abandoned record at the last generation also retires on its next
     * search (abandon keeps the generation; alloc would bump past the max). */
    {
        free_it(y2);
        je_proc_id z = alloc_ok();                     /* slot 1, gen 3 == GEN_MAX */
        assert(z.index == 1u && z.gen == JE_PROC_GEN_MAX);
        assert(je_proc_abandon(&g_pool, z) == JE_OK);
        je_proc_id w = alloc_ok();
        assert(w.index == 2u && w.gen == 1u);
        assert(g_pool.rec[1].retired);
        free_it(w);
    }

    /* Retire every slot: then alloc reports NOMEM with *out = NONE, and a
     * further alloc changes nothing. Bounded: REC_MAX * GEN_MAX allocs. */
    bool hit_nomem = false;
    for (uint32_t guard = 0; guard < JE_PROC_REC_MAX * (JE_PROC_GEN_MAX + 1u); ++guard) {
        je_proc_id o = mkid(9u, 9u);
        je_status s = je_proc_alloc(&g_pool, 0u, false, &o);
        if (s == JE_ERR_NOMEM) {
            assert(o.index == 0u && o.gen == 0u);
            hit_nomem = true;
            break;
        }
        assert(s == JE_OK);
        free_it(o);
    }
    assert(hit_nomem);
    for (uint32_t i = 0; i < JE_PROC_REC_MAX; ++i) {
        assert(g_pool.rec[i].retired);
        assert(g_pool.rec[i].state == JE_PROC_FREE);
        assert(g_pool.rec[i].gen == JE_PROC_GEN_MAX);
    }
    assert(g_pool.used_count == 0u && count_not_free() == 0u);
    memcpy(&g_snap, &g_pool, sizeof g_pool);
    {
        je_proc_id o = mkid(9u, 9u);
        assert(je_proc_alloc(&g_pool, 0u, false, &o) == JE_ERR_NOMEM);
        assert(o.index == 0u && o.gen == 0u);
    }
    assert(memcmp(&g_snap, &g_pool, sizeof g_pool) == 0);

    test_retire_during_failing_search();

    printf("proc_retire: PASS (gens 1..%u per slot, retire not wrap, retired slot "
           "skipped forever, stale identities rejected, all-retired NOMEM, failing "
           "search retires and changes nothing else) "
           "[JE_PROC_GEN_MAX=%u]\n", (unsigned)JE_PROC_GEN_MAX, (unsigned)JE_PROC_GEN_MAX);
    return 0;
}
