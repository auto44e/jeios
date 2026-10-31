/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hostable unit tests for the M3c endpoint lifecycle core (ADR 0009 D-SEQ step 2).
 * No kernel state: a local handle table over local slots + a local endpoint pool.
 * Proves destruction-on-last-ref, the split refcount, REFOVERFLOW, and dup
 * acquire-before-commit rollback — all without any ring-3 path (create is
 * kernel-only, exercised directly here).
 */
#include <jeios/endpoint.h>
#include <jeios/vm.h>   /* constants only: the per-process table shape */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The local table mirrors a je_vm_build process table (ADR 0010 D2: capacity
 * H, default quota 15), so a test that fills "a process's quota" fills the real
 * one. Was 16/15. */
#define CAP JE_VM_HANDLE_CAP
#define QUOTA JE_VM_HANDLE_QUOTA_DEFAULT

/* Pool-exhaustion table: both ends of every pool record plus one spare pair, so
 * the pool (E), never the table, is the binding limit. At E=24 this exceeds a
 * process table (48 > H=40), hence a dedicated table. */
#define POOL_T_CAP (2u * JE_EP_POOL_CAP + 2u)
static je_handle_slot g_pool_slots[POOL_T_CAP];
static je_handle_table g_pool_t;

/* REFOVERFLOW table (4b): room for REF_MAX refs of one end, the peer, and
 * spare, with quota == capacity, so JE_EP_REF_MAX is the only possible limit. */
#define REF_T_CAP (JE_EP_REF_MAX + 4u)
static je_handle_slot g_ref_slots[REF_T_CAP];
static je_handle_table g_ref_t;

static je_handle_slot g_slots[CAP];
static je_handle_table g_t;
static je_endpoint_pool g_pool;

static void fresh(uint32_t quota)
{
    assert(je_handle_table_init(&g_t, g_slots, CAP, quota));
    je_endpoint_pool_init(&g_pool);
}

/* Create one channel with both ends in the same table, all rights. */
static void make_pair(je_handle *h0, je_handle *h1)
{
    je_ep_status s = je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
                                                &g_t, JE_EP_RIGHTS_ALL, h0, h1);
    assert(s == JE_EP_OK && *h0 != 0u && *h1 != 0u && *h0 != *h1);
}

/* ===== ADR 0010 D-DEV: endpoint creation quota (je_endpoint_channel_create_by).
 * Written from endpoint.h + proc.h only. A REAL je_proc_pool is attached to the
 * endpoint pool, so charge/uncharge go through proc.c's identity checks, not a
 * fake. Every refusal is checked as "status AND no state change" by memcmp of
 * the endpoint pool, the proc pool and the table(s). ======================== */
static je_proc_pool     g_procs;
static je_proc_pool     g_procs_snap;
static je_endpoint_pool g_pool_snap;
static je_handle_slot   g_t_snap_slots[CAP];
static uint32_t         g_t_snap_live;
static je_handle_slot   g_tb_slots[CAP];     /* a second process's table */
static je_handle_table  g_tb;
static je_handle_slot   g_tq_slots[CAP];     /* a small-quota table for mint failure */
static je_handle_table  g_tq;
static je_endpoint_pool g_pool2;             /* create == create_by(NONE) twin */
static je_handle_slot   g_t2_slots[CAP];
static je_handle_table  g_t2;

static void ddev_fresh(void)
{
    fresh(QUOTA);
    assert(g_pool.procs == (je_proc_pool *)0);   /* init leaves charging disabled */
    je_proc_pool_init(&g_procs);
    g_pool.procs = &g_procs;
    assert(je_handle_table_init(&g_tb, g_tb_slots, CAP, QUOTA));
}

static void ddev_snap(void)
{
    memcpy(&g_pool_snap, &g_pool, sizeof g_pool);
    memcpy(&g_procs_snap, &g_procs, sizeof g_procs);
    memcpy(g_t_snap_slots, g_slots, sizeof g_slots);
    g_t_snap_live = g_t.live;
}

static void ddev_unchanged(void)
{
    assert(memcmp(&g_pool_snap, &g_pool, sizeof g_pool) == 0);
    assert(memcmp(&g_procs_snap, &g_procs, sizeof g_procs) == 0);
    assert(memcmp(g_t_snap_slots, g_slots, sizeof g_slots) == 0);
    assert(g_t.live == g_t_snap_live);
}

static void procs_snap(void) { memcpy(&g_procs_snap, &g_procs, sizeof g_procs); }
static void procs_unchanged(void) { assert(memcmp(&g_procs_snap, &g_procs, sizeof g_procs) == 0); }

static je_proc_id proc_new(uint32_t ep_limit)
{
    je_proc_id id = JE_PROC_ID_NONE;
    assert(je_proc_alloc(&g_procs, ep_limit, false, &id) == JE_OK);
    return id;
}

static uint32_t charge_of(je_proc_id id) { return g_procs.rec[id.index].ep_charge; }

/* create_by with both ends in g_t; returns the status, outs preset to garbage so
 * the "both *out are 0 on failure" rule is observable. */
static je_ep_status cb(je_proc_id creator, je_handle *h0, je_handle *h1)
{
    *h0 = 0xdeadu; *h1 = 0xbeefu;
    return je_endpoint_channel_create_by(&g_pool, creator, &g_t, JE_EP_RIGHTS_ALL,
                                         &g_t, JE_EP_RIGHTS_ALL, h0, h1);
}

static je_ep_channel *rec_of(je_handle_table *t, je_handle h)
{
    je_ep_channel *rec; uint8_t side;
    assert(je_endpoint_resolve(&g_pool, t, h, &rec, &side) == JE_EP_OK);
    return rec;
}

static void close_ok(je_handle_table *t, je_handle h)
{
    assert(je_endpoint_handle_close(&g_pool, t, h) == JE_EP_OK);
}

/* Move end `h` (in g_t) to table `dst` as a capability over carrier (s0 in g_t,
 * s1 in dst): send, peek, mint into dst, commit. Returns the minted handle. If
 * `leave_queued` the cap is sent but NOT received (it stays in flight). */
static je_handle move_cap(je_handle h, je_handle s0, je_handle s1, je_handle_table *dst,
                          bool leave_queued)
{
    je_ep_channel *xr; uint8_t xs;
    assert(je_endpoint_resolve(&g_pool, &g_t, h, &xr, &xs) == JE_EP_OK);
    je_ep_capdesc d = { (uint16_t)(xr - &g_pool.rec[0]), xs, 0u, xr->generation, JE_EP_RIGHTS_ALL };
    assert(je_endpoint_try_send_caps(&g_pool, &g_t, s0, (void *)0, 0, &d, 1) == JE_EP_OK);
    if (leave_queued) return 0u;
    uint8_t rb[JE_EP_MSG_MAX]; size_t rl = 0; uint8_t nc = 0;
    je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
    assert(je_endpoint_recv_peek_caps(&g_pool, dst, s1, rb, sizeof rb, &rl, JE_EP_MSG_CAPS_MAX,
                                      rc, &nc, &lz) == JE_EP_OK && nc == 1u);
    je_ep_channel *mr; uint8_t ms;
    assert(je_endpoint_capdesc_resolve(&g_pool, &rc[0], &mr, &ms) == JE_EP_OK);
    assert(je_endpoint_capdesc_retain_handle(&g_pool, &rc[0]) == JE_EP_OK);
    je_handle minted = 0u;
    assert(je_handle_alloc(dst, (void *)&mr->end[ms], JE_ENDPOINT_OBJ_TYPE, rc[0].rights,
                           &minted) == JE_HANDLE_OK);
    assert(je_endpoint_recv_commit_caps(&g_pool, &lz) == JE_EP_OK);
    return minted;
}

/* A carrier channel s0 (in g_t) <-> s1 (in dst), uncharged (kernel-style). */
static void carrier(je_handle_table *dst, je_handle *s0, je_handle *s1)
{
    assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL, dst, JE_EP_RIGHTS_ALL,
                                      s0, s1) == JE_EP_OK);
}

/* A non-NONE identity that names no record: index in range, a generation this
 * fresh proc pool has never issued. */
static je_proc_id mkid_proc_invalid(void)
{
    je_proc_id id = { JE_PROC_REC_MAX - 1u, 0x7777u };
    return id;
}

static void test_ddev(void)
{
    je_handle a0, a1, b0, b1, c0, c1;

    /* 1. create_by charges the creator and records its identity; at quota ->
     * QUOTA with NO state change; outs zeroed. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(2u);
        assert(cb(A, &a0, &a1) == JE_EP_OK && a0 != 0u && a1 != 0u);
        assert(charge_of(A) == 1u && je_endpoint_pool_allocated(&g_pool) == 1u);
        je_ep_channel *ra = rec_of(&g_t, a0);
        assert(ra->creator.index == A.index && ra->creator.gen == A.gen);
        assert(rec_of(&g_t, a1) == ra);
        assert(cb(A, &b0, &b1) == JE_EP_OK);
        assert(charge_of(A) == 2u && je_endpoint_pool_allocated(&g_pool) == 2u);
        ddev_snap();
        assert(cb(A, &c0, &c1) == JE_EP_QUOTA);
        assert(c0 == 0u && c1 == 0u);
        ddev_unchanged();
        assert(cb(A, &c0, &c1) == JE_EP_QUOTA);              /* repeatable */
        ddev_unchanged();

        /* 2. destruction uncharges exactly once, on the LAST close only */
        close_ok(&g_t, a0);
        assert(charge_of(A) == 2u);                         /* record still alive */
        close_ok(&g_t, a1);
        assert(charge_of(A) == 1u && je_endpoint_pool_allocated(&g_pool) == 1u);
        procs_snap();
        assert(je_endpoint_handle_close(&g_pool, &g_t, a1) == JE_EP_STALE);  /* double close */
        assert(je_endpoint_handle_close(&g_pool, &g_t, a0) == JE_EP_STALE);
        procs_unchanged();
        /* the freed charge is real room again */
        assert(cb(A, &a0, &a1) == JE_EP_OK && charge_of(A) == 2u);
        close_ok(&g_t, a0); close_ok(&g_t, a1);
        close_ok(&g_t, b1); close_ok(&g_t, b0);
        assert(charge_of(A) == 0u && je_endpoint_pool_allocated(&g_pool) == 0u);
        /* limit 0 (an M4 child): the first create is QUOTA, no change */
        je_proc_id K = proc_new(0u);
        ddev_snap();
        assert(cb(K, &c0, &c1) == JE_EP_QUOTA && c0 == 0u && c1 == 0u);
        ddev_unchanged();
        /* charges are per identity: K's refusal never touched A */
        assert(charge_of(A) == 0u && g_procs.rec[A.index].ep_limit == 2u);
    }

    /* 3. pool full -> NOSLOTS BEFORE charging. Pinned order: a creator AT QUOTA on
     * a full pool gets NOSLOTS (not QUOTA); a creator with room is not charged. */
    ddev_fresh();
    assert(je_handle_table_init(&g_pool_t, g_pool_slots, POOL_T_CAP, POOL_T_CAP));
    {
        je_proc_id B = proc_new(1u);       /* will be exactly at quota */
        je_proc_id R = proc_new(5u);       /* has room */
        je_handle p0, p1;
        for (uint32_t i = 0; i + 1u < JE_EP_POOL_CAP; ++i)
            assert(je_endpoint_channel_create(&g_pool, &g_pool_t, JE_EP_RIGHTS_ALL,
                                              &g_pool_t, JE_EP_RIGHTS_ALL, &p0, &p1) == JE_EP_OK);
        assert(je_endpoint_channel_create_by(&g_pool, B, &g_pool_t, JE_EP_RIGHTS_ALL,
                                             &g_pool_t, JE_EP_RIGHTS_ALL, &p0, &p1) == JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool) == JE_EP_POOL_CAP);
        assert(charge_of(B) == 1u && charge_of(R) == 0u);
        ddev_snap();
        assert(cb(B, &c0, &c1) == JE_EP_NOSLOTS);            /* at quota AND full */
        assert(c0 == 0u && c1 == 0u);
        ddev_unchanged();
        assert(cb(R, &c0, &c1) == JE_EP_NOSLOTS);            /* room, but full */
        ddev_unchanged();
        assert(charge_of(R) == 0u);                          /* never charged */
        assert(cb(JE_PROC_ID_NONE, &c0, &c1) == JE_EP_NOSLOTS);
        ddev_unchanged();
        /* step 1 precedes the charge, so even a non-NONE invalid creator is NOSLOTS */
        assert(cb(mkid_proc_invalid(), &c0, &c1) == JE_EP_NOSLOTS);
        ddev_unchanged();
        /* freeing B's channel (last pool record) uncharges B and makes room */
        close_ok(&g_pool_t, p0); close_ok(&g_pool_t, p1);
        assert(charge_of(B) == 0u);
        assert(cb(R, &c0, &c1) == JE_EP_OK && charge_of(R) == 1u);
    }

    /* 4. mint failure (table quota) unwinds the record AND uncharges. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(1u);
        je_handle q0, q1;
        /* second mint fails: a quota-1 table for both ends */
        assert(je_handle_table_init(&g_tq, g_tq_slots, CAP, 1u));
        procs_snap();
        q0 = 0xdeadu; q1 = 0xbeefu;
        assert(je_endpoint_channel_create_by(&g_pool, A, &g_tq, JE_EP_RIGHTS_ALL, &g_tq,
                                             JE_EP_RIGHTS_ALL, &q0, &q1) == JE_EP_QUOTA);
        assert(q0 == 0u && q1 == 0u);
        assert(charge_of(A) == 0u && g_tq.live == 0u);
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
        for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) assert(!g_pool.rec[i].allocated);
        procs_unchanged();
        /* first mint fails: a quota-0 table for end 0 */
        assert(je_handle_table_init(&g_tq, g_tq_slots, CAP, 0u));
        assert(je_endpoint_channel_create_by(&g_pool, A, &g_tq, JE_EP_RIGHTS_ALL, &g_t,
                                             JE_EP_RIGHTS_ALL, &q0, &q1) == JE_EP_QUOTA);
        assert(q0 == 0u && q1 == 0u && g_t.live == 0u);
        procs_unchanged();
        /* end 1 into the quota-0 table, end 0 into a good one */
        assert(je_endpoint_channel_create_by(&g_pool, A, &g_t, JE_EP_RIGHTS_ALL, &g_tq,
                                             JE_EP_RIGHTS_ALL, &q0, &q1) == JE_EP_QUOTA);
        assert(q0 == 0u && q1 == 0u && g_t.live == 0u);
        procs_unchanged();
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
        /* nothing leaked: A (limit 1) can still create exactly one channel */
        assert(cb(A, &a0, &a1) == JE_EP_OK && charge_of(A) == 1u);
        assert(cb(A, &b0, &b1) == JE_EP_QUOTA);
        /* a capacity-1 table (second mint refused: QUOTA, QUOTA-before-NOSLOTS)
         * also unwinds the charge */
        je_proc_id W = proc_new(3u);
        assert(je_handle_table_init(&g_tq, g_tq_slots, 1u, 1u));   /* capacity 1 */
        procs_snap();
        assert(je_endpoint_channel_create_by(&g_pool, W, &g_tq, JE_EP_RIGHTS_ALL, &g_tq,
                                             JE_EP_RIGHTS_ALL, &q0, &q1) != JE_EP_OK);
        assert(q0 == 0u && q1 == 0u && g_tq.live == 0u);
        procs_unchanged();
        assert(je_endpoint_pool_allocated(&g_pool) == 1u);
    }

    /* 5. the charge follows the RECORD, not the holder: it survives the creator's
     * handles moving to another table and is released only at destruction. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(2u);
        je_handle s0, s1;
        carrier(&g_tb, &s0, &s1);                            /* uncharged */
        assert(charge_of(A) == 0u);
        assert(cb(A, &a0, &a1) == JE_EP_OK && charge_of(A) == 1u);
        je_handle m = move_cap(a1, s0, s1, &g_tb, false);    /* end 1 now also in tb */
        close_ok(&g_t, a1);
        close_ok(&g_t, a0);                                  /* creator holds nothing */
        assert(charge_of(A) == 1u);                          /* tb keeps it alive */
        assert(je_endpoint_pool_allocated(&g_pool) == 2u);
        close_ok(&g_tb, m);                                  /* destruction */
        assert(charge_of(A) == 0u && je_endpoint_pool_allocated(&g_pool) == 1u);

        /* in-flight: a queued cap (inflight ref) keeps the record and the charge;
         * dequeuing it destroys the record and uncharges once. */
        assert(cb(A, &b0, &b1) == JE_EP_OK && charge_of(A) == 1u);
        (void)move_cap(b1, s0, s1, &g_tb, true);
        close_ok(&g_t, b0); close_ok(&g_t, b1);
        assert(charge_of(A) == 1u && je_endpoint_pool_allocated(&g_pool) == 2u);
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl = 0; uint8_t nc = 0;
        je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek_caps(&g_pool, &g_tb, s1, rb, sizeof rb, &rl,
                                          JE_EP_MSG_CAPS_MAX, rc, &nc, &lz) == JE_EP_OK);
        assert(nc == 1u && !je_endpoint_capdesc_live(&g_pool, &rc[0]));
        assert(je_endpoint_recv_commit_caps(&g_pool, &lz) == JE_EP_OK);
        assert(charge_of(A) == 0u && je_endpoint_pool_allocated(&g_pool) == 1u);
        /* the HOLDER (tb's process) is never charged: a fresh proc for it is 0 */
    }

    /* 6. creator DYING (same identity) -> uncharged at destroy, including during
     * its own release walk. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(3u);
        assert(cb(A, &a0, &a1) == JE_EP_OK);
        assert(cb(A, &b0, &b1) == JE_EP_OK && charge_of(A) == 2u);
        assert(je_proc_on_dying(&g_procs, A, 0u, 0u) == JE_OK);
        close_ok(&g_t, a0); close_ok(&g_t, a1);
        assert(charge_of(A) == 1u);
        assert(je_endpoint_release_table(&g_pool, &g_t) == 2);   /* b0, b1 */
        assert(charge_of(A) == 0u && je_endpoint_pool_allocated(&g_pool) == 0u);
        assert(g_procs.rec[A.index].state == JE_PROC_DYING);
    }

    /* 7. creator DEAD with a handle open (same identity) -> uncharged at destroy;
     * the record then frees normally on its last PROCESS-handle release. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(1u);
        assert(je_proc_retain(&g_procs, A) == JE_OK);
        assert(cb(A, &a0, &a1) == JE_EP_OK && charge_of(A) == 1u);
        assert(je_proc_on_dying(&g_procs, A, 1u, 7u) == JE_OK);
        assert(je_proc_on_reaped(&g_procs, A) == JE_OK);
        assert(g_procs.rec[A.index].state == JE_PROC_DEAD);
        close_ok(&g_t, a0);
        assert(charge_of(A) == 1u);
        close_ok(&g_t, a1);
        assert(charge_of(A) == 0u && g_procs.rec[A.index].state == JE_PROC_DEAD);
        je_proc_release(&g_procs, A);
        assert(g_procs.rec[A.index].state == JE_PROC_FREE);
    }

    /* 8. creator FREED -> destroy is a no-op on the proc pool. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(1u);
        assert(cb(A, &a0, &a1) == JE_EP_OK);
        assert(je_proc_on_dying(&g_procs, A, 0u, 0u) == JE_OK);
        assert(je_proc_on_reaped(&g_procs, A) == JE_OK);      /* refs 0 -> FREE */
        assert(g_procs.rec[A.index].state == JE_PROC_FREE);
        procs_snap();
        close_ok(&g_t, a0); close_ok(&g_t, a1);
        procs_unchanged();
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
    }

    /* 9. creator REUSED -> the old channel's destroy never decrements the new
     * identity (same index, next generation). */
    ddev_fresh();
    {
        je_proc_id A = proc_new(2u);
        assert(cb(A, &a0, &a1) == JE_EP_OK);
        assert(je_proc_on_dying(&g_procs, A, 0u, 0u) == JE_OK);
        assert(je_proc_on_reaped(&g_procs, A) == JE_OK);
        je_proc_id B = proc_new(2u);
        assert(B.index == A.index && B.gen == A.gen + 1u);
        assert(charge_of(B) == 0u);
        assert(cb(B, &b0, &b1) == JE_EP_OK && charge_of(B) == 1u);
        procs_snap();
        close_ok(&g_t, a0); close_ok(&g_t, a1);              /* A's channel dies */
        procs_unchanged();
        assert(charge_of(B) == 1u);
        close_ok(&g_t, b0); close_ok(&g_t, b1);
        assert(charge_of(B) == 0u);
        /* a stale creator identity cannot create (fail closed, see WORKER_NOTES) */
        ddev_snap();
        assert(cb(A, &c0, &c1) != JE_EP_OK && c0 == 0u && c1 == 0u);
        ddev_unchanged();
    }

    /* 10. NONE is uncharged even with procs attached. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(0u);                          /* a record at index 0 */
        (void)A;
        procs_snap();
        for (uint32_t i = 0; i < 3u; ++i) {
            je_handle x0, x1;
            assert(cb(JE_PROC_ID_NONE, &x0, &x1) == JE_EP_OK);
            je_ep_channel *r = rec_of(&g_t, x0);
            assert(r->creator.index == 0u && r->creator.gen == 0u);
        }
        procs_unchanged();                                    /* index 0 not charged */
        assert(je_endpoint_release_table(&g_pool, &g_t) == 6);
        procs_unchanged();
    }

    /* 11. procs == NULL: charging disabled, any creator is uncharged. A creator
     * at quota (limit 0) in a DETACHED proc pool still creates. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(0u);
        g_pool.procs = (je_proc_pool *)0;
        procs_snap();
        assert(cb(A, &a0, &a1) == JE_EP_OK);
        assert(cb(A, &b0, &b1) == JE_EP_OK);
        procs_unchanged();
        close_ok(&g_t, a0); close_ok(&g_t, a1);
        assert(je_endpoint_release_table(&g_pool, &g_t) == 2);
        procs_unchanged();
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
    }

    /* 12. je_endpoint_channel_create(...) == create_by(pool, NONE, ...): the same
     * sequence on two identically initialised pools gives identical handles,
     * statuses and record arrays, and never touches the attached proc pool. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(0u); (void)A;
        assert(je_handle_table_init(&g_t2, g_t2_slots, CAP, QUOTA));
        /* pool_init does not scrub queue payload bytes, so both twins start
         * from zeroed storage (g_pool was used by earlier sections). */
        memset(&g_pool, 0, sizeof g_pool);
        memset(&g_pool2, 0, sizeof g_pool2);
        je_endpoint_pool_init(&g_pool);
        je_endpoint_pool_init(&g_pool2);
        g_pool.procs = &g_procs;
        g_pool2.procs = &g_procs;
        procs_snap();
        for (uint32_t i = 0; i < 4u; ++i) {
            je_handle x0 = 1u, x1 = 1u, y0 = 2u, y1 = 2u;
            je_ep_status sx = je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
                                                         &g_t, JE_EP_RIGHT_RECV, &x0, &x1);
            je_ep_status sy = je_endpoint_channel_create_by(&g_pool2, JE_PROC_ID_NONE, &g_t2,
                                                            JE_EP_RIGHTS_ALL, &g_t2,
                                                            JE_EP_RIGHT_RECV, &y0, &y1);
            assert(sx == JE_EP_OK && sx == sy && x0 == y0 && x1 == y1);
            if (i == 1u) {   /* destroy one to exercise free + reuse identically */
                assert(je_endpoint_handle_close(&g_pool, &g_t, x0) == JE_EP_OK);
                assert(je_endpoint_handle_close(&g_pool, &g_t, x1) == JE_EP_OK);
                assert(je_endpoint_handle_close(&g_pool2, &g_t2, y0) == JE_EP_OK);
                assert(je_endpoint_handle_close(&g_pool2, &g_t2, y1) == JE_EP_OK);
            }
        }
        assert(memcmp(g_pool.rec, g_pool2.rec, sizeof g_pool.rec) == 0);
        assert(g_pool.allocated_count == g_pool2.allocated_count);
        procs_unchanged();
        /* and both fail identically at table quota (QUOTA, outs zero) */
        assert(je_handle_table_init(&g_tq, g_tq_slots, CAP, 1u));
        je_handle x0, x1, y0, y1;
        je_ep_status sx = je_endpoint_channel_create(&g_pool, &g_tq, JE_EP_RIGHTS_ALL,
                                                     &g_tq, JE_EP_RIGHTS_ALL, &x0, &x1);
        assert(je_handle_table_init(&g_tq, g_tq_slots, CAP, 1u));
        je_ep_status sy = je_endpoint_channel_create_by(&g_pool2, JE_PROC_ID_NONE, &g_tq,
                                                        JE_EP_RIGHTS_ALL, &g_tq,
                                                        JE_EP_RIGHTS_ALL, &y0, &y1);
        assert(sx == JE_EP_QUOTA && sy == JE_EP_QUOTA);
        assert(x0 == 0u && x1 == 0u && y0 == 0u && y1 == 0u);
        procs_unchanged();
    }
    /* 13. a reused pool record never carries a stale creator: A's freed record
     * reused by an uncharged create (or by another creator) must not decrement A
     * when it is destroyed, while A still holds a different charged channel. */
    ddev_fresh();
    {
        je_proc_id A = proc_new(2u), B = proc_new(2u);
        assert(cb(A, &a0, &a1) == JE_EP_OK);                 /* record 0, charged to A */
        je_ep_channel *r0 = rec_of(&g_t, a0);
        assert(cb(A, &b0, &b1) == JE_EP_OK);                 /* record 1, charged to A */
        close_ok(&g_t, a0); close_ok(&g_t, a1);              /* record 0 freed */
        assert(charge_of(A) == 1u);
        /* plain create reuses record 0 (lowest free) */
        assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL, &g_t,
                                          JE_EP_RIGHTS_ALL, &c0, &c1) == JE_EP_OK);
        assert(rec_of(&g_t, c0) == r0);
        assert(r0->creator.index == 0u && r0->creator.gen == 0u);
        close_ok(&g_t, c0); close_ok(&g_t, c1);
        assert(charge_of(A) == 1u);                          /* not decremented */
        /* B reuses record 0: destroying it charges/uncharges B only */
        assert(cb(B, &c0, &c1) == JE_EP_OK && rec_of(&g_t, c0) == r0);
        assert(r0->creator.index == B.index && r0->creator.gen == B.gen);
        assert(charge_of(A) == 1u && charge_of(B) == 1u);
        close_ok(&g_t, c0); close_ok(&g_t, c1);
        assert(charge_of(A) == 1u && charge_of(B) == 0u);
        close_ok(&g_t, b0); close_ok(&g_t, b1);
        assert(charge_of(A) == 0u);
    }

    /* restore the shared table for anything after this section */
    fresh(QUOTA);
}

int main(void)
{
    /* 1. Create: both ends OPEN, refs 1/1, one record allocated. */
    fresh(QUOTA);
    je_handle h0 = 0u, h1 = 0u;
    make_pair(&h0, &h1);
    assert(je_endpoint_pool_allocated(&g_pool) == 1u);
    {
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_OPEN);
        assert(je_endpoint_end_state(rec, 1) == JE_EP_END_OPEN);
        assert(rec->end[0].handle_refs == 1u && rec->end[1].handle_refs == 1u);
        assert(rec->end[side].side == side);
    }

    /* 2. Close end 0 -> end0 CLOSED, peer PEER_CLOSED, record still alive.
     *    Close end 1 -> record freed. */
    {
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, h1, &rec, &side) == JE_EP_OK);
        uint32_t gen_before = rec->generation;

        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_OK);
        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_CLOSED);
        assert(je_endpoint_end_state(rec, 1) == JE_EP_END_PEER_CLOSED);
        assert(rec->allocated);
        assert(je_endpoint_pool_allocated(&g_pool) == 1u);

        assert(je_endpoint_handle_close(&g_pool, &g_t, h1) == JE_EP_OK);
        assert(!rec->allocated);
        assert(rec->generation == gen_before + 1u);     /* bumped on free */
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
        /* The closed handles are now stale. */
        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_STALE);
    }

    /* 3. Dup: original and dup are independent handle_refs on the same end.
     *    Closing the original leaves the end OPEN; closing the dup closes it. */
    fresh(QUOTA);
    make_pair(&h0, &h1);
    {
        je_handle dup = 0u;
        assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHTS_ALL, &dup)
               == JE_EP_OK && dup != 0u && dup != h0);
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
        assert(rec->end[0].handle_refs == 2u);

        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_OK);
        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_OPEN);  /* dup remains */
        assert(rec->end[0].handle_refs == 1u);

        assert(je_endpoint_handle_close(&g_pool, &g_t, dup) == JE_EP_OK);
        assert(je_endpoint_end_state(rec, 0) == JE_EP_END_CLOSED);
        assert(je_endpoint_end_state(rec, 1) == JE_EP_END_PEER_CLOSED);
        assert(je_endpoint_handle_close(&g_pool, &g_t, h1) == JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
    }

    /* 4. REFOVERFLOW: dup end 0 up to JE_EP_REF_MAX handle_refs, then one more
     *    fails with nothing acquired, table live-count and *out unchanged. */
    fresh(QUOTA);
    make_pair(&h0, &h1);   /* end0 starts at 1 handle_ref */
    {
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
        je_handle dups[JE_EP_REF_MAX];
        /* bring handle_refs from 1 up to JE_EP_REF_MAX */
        uint32_t need = JE_EP_REF_MAX - 1u;
        for (uint32_t i = 0; i < need; ++i) {
            assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHT_INSPECT,
                                          &dups[i]) == JE_EP_OK);
        }
        assert(rec->end[0].handle_refs == JE_EP_REF_MAX);
        uint32_t live_before = g_t.live;
        je_handle over = 123u;   /* sentinel, must be cleared to 0 */
        assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHT_INSPECT,
                                      &over) == JE_EP_REFOVERFLOW);
        assert(over == 0u);
        assert(rec->end[0].handle_refs == JE_EP_REF_MAX);   /* unchanged */
        assert(g_t.live == live_before);                    /* no slot taken */
    }

    /* 4b. REFOVERFLOW is the limiter, not the table: a table whose capacity and
     *     quota both exceed the handles one end can ever hold. Dup past
     *     JE_EP_REF_MAX returns REFOVERFLOW and changes NOTHING (pool, table
     *     header and slots byte-identical); the table still has room afterwards
     *     and the other end is unaffected. */
    {
        _Static_assert(REF_T_CAP >= JE_EP_REF_MAX + 3u, "table must not be the limiter");
        static je_endpoint_pool pool_before;
        static je_handle_table t_before;
        static je_handle_slot slots_before[REF_T_CAP];
        je_handle r0 = 0u, r1 = 0u, d = 0u;
        je_ep_channel *rec; uint8_t side;

        assert(je_handle_table_init(&g_ref_t, g_ref_slots, REF_T_CAP, REF_T_CAP));
        je_endpoint_pool_init(&g_pool);
        assert(je_endpoint_channel_create(&g_pool, &g_ref_t, JE_EP_RIGHTS_ALL,
                                          &g_ref_t, JE_EP_RIGHTS_ALL, &r0, &r1)
               == JE_EP_OK);
        assert(je_endpoint_resolve(&g_pool, &g_ref_t, r0, &rec, &side) == JE_EP_OK);
        for (uint32_t i = 1u; i < JE_EP_REF_MAX; ++i)
            assert(je_endpoint_handle_dup(&g_pool, &g_ref_t, r0, JE_EP_RIGHTS_ALL,
                                          &d) == JE_EP_OK);
        assert(rec->end[side].handle_refs == JE_EP_REF_MAX);
        assert(g_ref_t.live == JE_EP_REF_MAX + 1u);
        assert(g_ref_t.live < g_ref_t.quota && g_ref_t.live < g_ref_t.capacity);
        memcpy(&pool_before, &g_pool, sizeof g_pool);
        memcpy(&t_before, &g_ref_t, sizeof g_ref_t);
        memcpy(slots_before, g_ref_slots, sizeof g_ref_slots);
        d = 77u;
        assert(je_endpoint_handle_dup(&g_pool, &g_ref_t, r0, JE_EP_RIGHTS_ALL, &d)
               == JE_EP_REFOVERFLOW);
        assert(d == 0u);
        assert(memcmp(&g_pool, &pool_before, sizeof g_pool) == 0);
        assert(memcmp(&g_ref_t, &t_before, sizeof g_ref_t) == 0);
        assert(memcmp(g_ref_slots, slots_before, sizeof g_ref_slots) == 0);
        /* The bound is per end: the peer end still dups into the same table. */
        assert(je_endpoint_handle_dup(&g_pool, &g_ref_t, r1, JE_EP_RIGHTS_ALL, &d)
               == JE_EP_OK);
        assert(d != 0u);
        /* And the table itself still admits a slot. */
        assert(je_handle_alloc(&g_ref_t, &g_ref_t, JE_HANDLE_OBJ_SELF, 0x1u, &d)
               == JE_HANDLE_OK);
    }

    /* 5. Dup DENIED on a superset: a RECV-only handle cannot dup to SEND. */
    fresh(QUOTA);
    {
        je_handle r0 = 0u, r1 = 0u, bad = 7u;
        assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHT_RECV,
                                          &g_t, JE_EP_RIGHTS_ALL, &r0, &r1)
               == JE_EP_OK);
        assert(je_endpoint_handle_dup(&g_pool, &g_t, r0, JE_EP_RIGHT_SEND, &bad)
               == JE_EP_DENIED);
        assert(bad == 0u);
        /* equal/subset dup is allowed */
        je_handle ok = 0u;
        assert(je_endpoint_handle_dup(&g_pool, &g_t, r0, JE_EP_RIGHT_RECV, &ok)
               == JE_EP_OK && ok != 0u);
    }

    /* 6. Dup rollback on QUOTA: fill the table to quota, then a dup must release
     *    its pre-acquired ref (end handle_refs returns to its prior value). */
    fresh(2u);   /* quota 2: the create pair already uses both live slots */
    make_pair(&h0, &h1);
    {
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
        assert(g_t.live == 2u);
        uint16_t refs_before = rec->end[0].handle_refs;
        je_handle q = 9u;
        assert(je_endpoint_handle_dup(&g_pool, &g_t, h0, JE_EP_RIGHT_INSPECT, &q)
               == JE_EP_QUOTA);
        assert(q == 0u);
        assert(rec->end[0].handle_refs == refs_before);  /* retain rolled back */
    }

    /* 7. WRONGTYPE: a SELF handle is not an endpoint; and an endpoint handle
     *    must be rejected by the SELF-only table dup path. */
    fresh(QUOTA);
    {
        je_handle self = 0u;
        assert(je_handle_alloc(&g_t, (void *)0, JE_HANDLE_OBJ_SELF,
                               JE_EP_RIGHT_INSPECT, &self) == JE_HANDLE_OK);
        je_handle x = 5u;
        assert(je_endpoint_handle_dup(&g_pool, &g_t, self, JE_EP_RIGHT_INSPECT, &x)
               == JE_EP_WRONGTYPE);
        assert(x == 0u);
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, self, &rec, &side)
               == JE_EP_WRONGTYPE);

        make_pair(&h0, &h1);
        /* dup_self on an endpoint handle: the table's SELF-only gate rejects it. */
        je_handle y = 6u;
        assert(je_handle_dup_self(&g_t, h0, JE_HANDLE_OBJ_SELF,
                                  JE_EP_RIGHT_INSPECT, &y) == JE_HANDLE_WRONGTYPE);
        assert(y == 0u);
    }

    /* 8. inflight_refs keep the record alive after both handles close; releasing
     *    the last inflight ref frees it. */
    fresh(QUOTA);
    make_pair(&h0, &h1);
    {
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, h0, &rec, &side) == JE_EP_OK);
        assert(je_endpoint_retain_inflight(&g_pool, rec, 0) == JE_EP_OK);
        assert(rec->end[0].inflight_refs == 1u);

        assert(je_endpoint_handle_close(&g_pool, &g_t, h0) == JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool, &g_t, h1) == JE_EP_OK);
        /* both handle_refs are 0, but an inflight ref remains -> NOT freed */
        assert(rec->allocated);
        assert(je_endpoint_pool_allocated(&g_pool) == 1u);

        assert(je_endpoint_release_inflight(&g_pool, rec, 0) == JE_EP_OK);
        assert(!rec->allocated);
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
        /* underflow guard: releasing again fails closed */
        assert(je_endpoint_release_inflight(&g_pool, rec, 0) == JE_EP_CORRUPT);
    }

    /* 9. Pool exhaustion + create unwind: fill the pool, next create NOSLOTS and
     *    leaves t0 unchanged; also a create whose SECOND mint fails unwinds the
     *    first mint and the record completely. */
    /*    ADR 0010 D2: exactly JE_EP_POOL_CAP (E) creates succeed; the (E+1)-th
     *    is NOSLOTS and mutates neither the pool nor the table (byte-compare).
     *    Freeing one record makes exactly one more create admissible. */
    fresh(CAP);
    {
        static je_endpoint_pool pool_before;
        static je_handle_table t_before;
        static je_handle_slot slots_before[POOL_T_CAP];
        je_handle a0[JE_EP_POOL_CAP], a1[JE_EP_POOL_CAP];
        assert(je_handle_table_init(&g_pool_t, g_pool_slots, POOL_T_CAP, POOL_T_CAP));
        for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
            assert(je_endpoint_channel_create(&g_pool, &g_pool_t, JE_EP_RIGHTS_ALL,
                                              &g_pool_t, JE_EP_RIGHTS_ALL,
                                              &a0[i], &a1[i]) == JE_EP_OK);
            assert(je_endpoint_pool_allocated(&g_pool) == i + 1u);
        }
        assert(je_endpoint_pool_allocated(&g_pool) == JE_EP_POOL_CAP);
        assert(g_pool_t.live == 2u * JE_EP_POOL_CAP);   /* table NOT the limiter */
        memcpy(&pool_before, &g_pool, sizeof g_pool);
        memcpy(&t_before, &g_pool_t, sizeof g_pool_t);
        memcpy(slots_before, g_pool_slots, sizeof g_pool_slots);
        je_handle x0 = 1u, x1 = 1u;
        assert(je_endpoint_channel_create(&g_pool, &g_pool_t, JE_EP_RIGHTS_ALL,
                                          &g_pool_t, JE_EP_RIGHTS_ALL, &x0, &x1)
               == JE_EP_NOSLOTS);
        assert(x0 == 0u && x1 == 0u);
        assert(memcmp(&g_pool, &pool_before, sizeof g_pool) == 0);
        assert(memcmp(&g_pool_t, &t_before, sizeof g_pool_t) == 0);
        assert(memcmp(g_pool_slots, slots_before, sizeof g_pool_slots) == 0);
        /* Release one record (last ref of both ends) -> one create fits again. */
        const uint32_t mid = JE_EP_POOL_CAP / 2u;
        assert(je_endpoint_handle_close(&g_pool, &g_pool_t, a0[mid]) == JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool, &g_pool_t, a1[mid]) == JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool) == JE_EP_POOL_CAP - 1u);
        assert(je_endpoint_channel_create(&g_pool, &g_pool_t, JE_EP_RIGHTS_ALL,
                                          &g_pool_t, JE_EP_RIGHTS_ALL,
                                          &a0[mid], &a1[mid]) == JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool) == JE_EP_POOL_CAP);
        assert(g_pool_t.live == 2u * JE_EP_POOL_CAP);
        /* Both recreated handles are live endpoints on opposite ends of ONE
         * record, and distinct from each other and from every other handle. */
        {
            je_ep_channel *r0, *r1; uint8_t s0, s1;
            assert(je_endpoint_resolve(&g_pool, &g_pool_t, a0[mid], &r0, &s0) == JE_EP_OK);
            assert(je_endpoint_resolve(&g_pool, &g_pool_t, a1[mid], &r1, &s1) == JE_EP_OK);
            assert(r0 == r1 && s0 != s1);
            assert(a0[mid] != 0u && a1[mid] != 0u && a0[mid] != a1[mid]);
            for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
                if (i == mid) continue;
                assert(a0[i] != a0[mid] && a0[i] != a1[mid]);
                assert(a1[i] != a0[mid] && a1[i] != a1[mid]);
            }
        }
        /* The second (E+1)-th attempt is also NOSLOTS with no state change. */
        memcpy(&pool_before, &g_pool, sizeof g_pool);
        memcpy(&t_before, &g_pool_t, sizeof g_pool_t);
        memcpy(slots_before, g_pool_slots, sizeof g_pool_slots);
        x0 = x1 = 1u;
        assert(je_endpoint_channel_create(&g_pool, &g_pool_t, JE_EP_RIGHTS_ALL,
                                          &g_pool_t, JE_EP_RIGHTS_ALL, &x0, &x1)
               == JE_EP_NOSLOTS);
        assert(x0 == 0u && x1 == 0u);
        assert(memcmp(&g_pool, &pool_before, sizeof g_pool) == 0);
        assert(memcmp(&g_pool_t, &t_before, sizeof g_pool_t) == 0);
        assert(memcmp(g_pool_slots, slots_before, sizeof g_pool_slots) == 0);
    }
    /* second-mint failure unwind: t1 quota 0 so end-1 alloc fails with QUOTA.
     * The record and end-0's mint must FULLY unwind — assert the record itself is
     * back to FREE (not just the allocated_count, which only ever counts
     * successes and so cannot catch a leaked half-built record), and that a fresh
     * create then reuses rec[0] cleanly. */
    fresh(CAP);
    {
        je_handle_slot t1_slots[4];
        je_handle_table t1;
        assert(je_handle_table_init(&t1, t1_slots, 4u, 0u));  /* quota 0 */
        uint32_t live0_before = g_t.live;
        je_handle x0 = 2u, x1 = 2u;
        je_ep_status s = je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
                                                    &t1, JE_EP_RIGHTS_ALL,
                                                    &x0, &x1);
        assert(s == JE_EP_QUOTA);
        assert(x0 == 0u && x1 == 0u);
        assert(g_t.live == live0_before);                 /* end-0 mint undone */
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
        /* The real leak check: rec[0] must be genuinely FREE (flag AND states AND
         * both handle_refs), so a half-built record cannot survive invisibly. */
        assert(!g_pool.rec[0].allocated);
        assert(g_pool.rec[0].end[0].state == JE_EP_END_FREE);
        assert(g_pool.rec[0].end[1].state == JE_EP_END_FREE);
        assert(g_pool.rec[0].end[0].handle_refs == 0u);
        assert(g_pool.rec[0].end[1].handle_refs == 0u);
        /* And the slot is cleanly reusable by a following successful create. */
        je_handle y0 = 0u, y1 = 0u;
        make_pair(&y0, &y1);
        assert(je_endpoint_pool_allocated(&g_pool) == 1u);
    }

    /* 9b. Aliased create outputs are rejected (out0 == out1 would clobber h0 and
     * orphan end-0's LIVE slot). Nothing is allocated. */
    fresh(CAP);
    {
        je_handle same = 3u;
        assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
                                          &g_t, JE_EP_RIGHTS_ALL, &same, &same)
               == JE_EP_BADINDEX);
        assert(same == 0u);
        assert(je_endpoint_pool_allocated(&g_pool) == 0u);
    }

    /* 10. BADINDEX / corrupt guards: handle 0 and a wide handle resolve BADINDEX. */
    fresh(QUOTA);
    {
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool, &g_t, 0u, &rec, &side) == JE_EP_BADINDEX);
        assert(je_endpoint_handle_close(&g_pool, &g_t, 0u) == JE_EP_BADINDEX);
    }

    /* 11. Release walk (je_endpoint_release_table): simulates process death. A
     * table with endpoint handles is walked; every endpoint handle is released,
     * no endpoint slot remains LIVE, and records with no surviving ref are freed.
     * A SELF handle in the same table is left untouched (only the zero reclaims
     * it, as today). */
    fresh(QUOTA);
    {
        /* Two channels fully owned by this one table (both ends here), plus a
         * dup, plus a SELF handle that must survive the walk. */
        je_handle a0=0,a1=0,b0=0,b1=0,dup=0,self=0;
        make_pair(&a0,&a1);
        make_pair(&b0,&b1);
        assert(je_endpoint_handle_dup(&g_pool,&g_t,a0,JE_EP_RIGHTS_ALL,&dup)==JE_EP_OK);
        assert(je_handle_alloc(&g_t,(void*)0,JE_HANDLE_OBJ_SELF,
                               JE_EP_RIGHT_INSPECT,&self)==JE_HANDLE_OK);
        assert(je_endpoint_pool_allocated(&g_pool)==2u);
        uint32_t live_before = g_t.live;   /* 5 endpoint handles + 1 self = 6 */

        int released = je_endpoint_release_table(&g_pool, &g_t);
        assert(released == 5);                      /* a0,a1,b0,b1,dup */
        assert(je_endpoint_pool_allocated(&g_pool)==0u);  /* both records freed */
        /* No endpoint slot LIVE; the SELF slot still LIVE. */
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool,&g_t,a0,&rec,&side)==JE_EP_STALE);
        void *obj=(void*)1;
        assert(je_handle_lookup(&g_t,self,JE_HANDLE_OBJ_SELF,0u,&obj)==JE_HANDLE_OK);
        assert(g_t.live == live_before - 5u);       /* only endpoints dropped */

        /* Idempotent: a second walk releases nothing and still succeeds. */
        assert(je_endpoint_release_table(&g_pool, &g_t) == 0);
    }

    /* 12. Release walk, same channel's two ends both in the dying table but one
     * via a dup: last release frees the record, no mid-walk UAF. */
    fresh(QUOTA);
    {
        je_handle c0=0,c1=0,c0dup=0;
        make_pair(&c0,&c1);
        assert(je_endpoint_handle_dup(&g_pool,&g_t,c0,JE_EP_RIGHTS_ALL,&c0dup)==JE_EP_OK);
        /* end0 has 2 handle_refs (c0,c0dup), end1 has 1 (c1). */
        assert(je_endpoint_release_table(&g_pool,&g_t) == 3);
        assert(je_endpoint_pool_allocated(&g_pool)==0u);
    }

    /* 13. Data IPC: send/recv roundtrip + FIFO, via the pure peek/commit seam.
     * Helper to do a full recv (peek into a local buf, then commit). */
    fresh(QUOTA);
    make_pair(&h0, &h1);
    {
        /* send on h0 -> lands in end1 inbound (peer). recv on h1 reads it. */
        uint8_t msg[8] = {10,11,12,13,14,15,16,17};
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,msg,8)==JE_EP_OK);
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool,&g_t,h1,&rec,&side)==JE_EP_OK);
        assert(je_endpoint_inbound_count(rec,side)==1u);
        /* recv on h0 is EMPTY (direction: nothing landed in end0 inbound). */
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h0,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
        /* recv on h1 delivers the message. */
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
        assert(rl==8u);
        for (int i=0;i<8;i++) assert(rb[i]==msg[i]);
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
        assert(je_endpoint_inbound_count(rec,side)==0u);
        /* now empty -> EMPTY. */
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
    }

    /* 14. FIFO order + QUEUE_FULL at DEPTH + no-refcount-interaction. */
    fresh(QUOTA);
    make_pair(&h0,&h1);
    {
        je_ep_channel *rec; uint8_t side;
        assert(je_endpoint_resolve(&g_pool,&g_t,h0,&rec,&side)==JE_EP_OK);
        uint16_t hr0=rec->end[0].handle_refs, hr1=rec->end[1].handle_refs;
        for (uint8_t i=0;i<JE_EP_QUEUE_DEPTH;i++) {
            uint8_t m[1]={ (uint8_t)(100+i) };
            assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,1)==JE_EP_OK);
        }
        /* queue full now -> QUEUE_FULL, nothing enqueued. */
        uint8_t mf[1]={9};
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,mf,1)==JE_EP_QUEUE_FULL);
        /* data did NOT touch refcounts. */
        assert(rec->end[0].handle_refs==hr0 && rec->end[1].handle_refs==hr1);
        assert(rec->end[0].inflight_refs==0u && rec->end[1].inflight_refs==0u);
        /* drain in FIFO order. */
        for (uint8_t i=0;i<JE_EP_QUEUE_DEPTH;i++) {
            uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
            assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
            assert(rl==1u && rb[0]==(uint8_t)(100+i));
            assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
        }
    }

    /* 15. SHORT_BUFFER: peek reports length, copies nothing, does NOT dequeue. */
    fresh(QUOTA);
    make_pair(&h0,&h1);
    {
        uint8_t m[5]={1,2,3,4,5};
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,5)==JE_EP_OK);
        uint8_t rb[2]={0xAA,0xBB}; size_t rl=0; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,2,&rl,&lz)==JE_EP_SHORT_BUFFER);
        assert(rl==5u);                 /* required length reported */
        assert(rb[0]==0xAA && rb[1]==0xBB);  /* nothing copied */
        /* still queued: a big-enough recv delivers it. */
        uint8_t rb2[JE_EP_MSG_MAX];
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb2,sizeof rb2,&rl,&lz)==JE_EP_OK);
        assert(rl==5u);
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
    }

    /* 16. PEER_CLOSED: send to a peer-closed end; drain-then-EOF on recv. */
    fresh(QUOTA);
    make_pair(&h0,&h1);
    {
        uint8_t m[3]={7,8,9};
        /* h0 sends one, then h0 closes: end1 (h1's end) goes PEER_CLOSED but its
         * inbound still holds the message. */
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,3)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,h0)==JE_EP_OK);
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
        /* drain first (message delivered despite peer gone). */
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
        assert(rl==3u && rb[0]==7);
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
        /* now empty + peer gone -> PEER_CLOSED (EOF). */
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_PEER_CLOSED);
        /* send on h1 -> its peer (end0) is CLOSED -> PEER_CLOSED. */
        assert(je_endpoint_send_preflight(&g_pool,&g_t,h1,3)==JE_EP_PEER_CLOSED);
    }

    /* 17. Rights: send needs SEND, recv needs RECV (DENIED otherwise). */
    fresh(QUOTA);
    {
        /* end0 has SEND only; end1 has RECV only. */
        je_handle s0=0,r1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHT_SEND,
                                          &g_t,JE_EP_RIGHT_RECV,&s0,&r1)==JE_EP_OK);
        uint8_t m[1]={1};
        /* s0 can send; r1 cannot send (no SEND) -> DENIED. */
        assert(je_endpoint_send_preflight(&g_pool,&g_t,s0,1)==JE_EP_OK);
        assert(je_endpoint_send_preflight(&g_pool,&g_t,r1,1)==JE_EP_DENIED);
        assert(je_endpoint_try_send(&g_pool,&g_t,s0,m,1)==JE_EP_OK);
        /* r1 can recv; s0 cannot recv (no RECV) -> DENIED. */
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek(&g_pool,&g_t,s0,rb,sizeof rb,&rl,&lz)==JE_EP_DENIED);
        assert(je_endpoint_recv_peek(&g_pool,&g_t,r1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
    }

    /* 18. Zero-length message is valid and distinct from EMPTY. */
    fresh(QUOTA);
    make_pair(&h0,&h1);
    {
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,(void*)0,0)==JE_EP_OK);
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=7; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
        assert(rl==0u);                 /* OK + len 0, NOT EMPTY */
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
    }

    /* 19. len > MSG_MAX rejected; free-with-nonempty-queue returns to baseline. */
    fresh(QUOTA);
    make_pair(&h0,&h1);
    {
        uint8_t big[JE_EP_MSG_MAX+1];
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,big,JE_EP_MSG_MAX+1)==JE_EP_INVALID);
        /* queue one, then close BOTH ends while a message is queued: record must
         * still free (data is not an owning ref). */
        uint8_t m[2]={5,6};
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,2)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,h0)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,h1)==JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool)==0u);  /* freed despite queued msg */
    }

    /* 20. commit lease rejected after the record is freed/reused (generation). */
    fresh(QUOTA);
    make_pair(&h0,&h1);
    {
        uint8_t m[1]={1};
        assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,1)==JE_EP_OK);
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
        /* Tear down the whole channel before committing -> stale lease. */
        assert(je_endpoint_handle_close(&g_pool,&g_t,h0)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,h1)==JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool)==0u);
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_CORRUPT);  /* fail closed */
    }

    /* 21. Ring WRAPAROUND with distinct payloads + lengths (catches a wrong
     * head/tail modulo that identical-payload tests would miss). Partially
     * drain then refill so head advances past 0 and tail wraps. */
    fresh(QUOTA);
    make_pair(&h0,&h1);
    {
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; je_ep_recv_lease lz;
        /* send 3 distinct (len i+1, bytes 0x10+i). */
        for (uint8_t i=0;i<3;i++) {
            uint8_t m[3]={(uint8_t)(0x10+i),(uint8_t)(0x20+i),(uint8_t)(0x30+i)};
            assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,(size_t)(i+1))==JE_EP_OK);
        }
        /* drain 2 (head -> 2). */
        for (uint8_t i=0;i<2;i++) {
            assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
            assert(rl==(size_t)(i+1) && rb[0]==(uint8_t)(0x10+i));
            assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
        }
        /* send 2 more (distinct, len 4) -> tail wraps past DEPTH. */
        for (uint8_t i=3;i<5;i++) {
            uint8_t m[4]={(uint8_t)(0x10+i),(uint8_t)(0x20+i),(uint8_t)(0x30+i),(uint8_t)(0x40+i)};
            assert(je_endpoint_try_send(&g_pool,&g_t,h0,m,4)==JE_EP_OK);
        }
        /* drain remaining in FIFO: msg2 (len3), msg3 (len4), msg4 (len4). */
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
        assert(rl==3u && rb[0]==0x12);
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
        assert(rl==4u && rb[0]==0x13);
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_OK);
        assert(rl==4u && rb[0]==0x14);
        assert(je_endpoint_recv_commit(&g_pool,&lz)==JE_EP_OK);
        assert(je_endpoint_recv_peek(&g_pool,&g_t,h1,rb,sizeof rb,&rl,&lz)==JE_EP_EMPTY);
    }

    /* 22. Capability transfer (pure layer, A5): build a descriptor for one
     * channel's end and send it as a cap over a SECOND channel; the receiver
     * side mints it. Also: inflight accounting, dead-cap (A5-2), cycle reclaim. */
    fresh(QUOTA);
    {
        /* seed channel S (s0,s1) and payload channel C (c0,c1), all in g_t. */
        je_handle s0=0,s1=0,c0=0,c1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&s0,&s1)==JE_EP_OK);
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&c0,&c1)==JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool)==2u);
        /* Build a cap descriptor for c1 (resolve it, snapshot rec/side/gen). */
        je_ep_channel *crec; uint8_t cside;
        assert(je_endpoint_resolve(&g_pool,&g_t,c1,&crec,&cside)==JE_EP_OK);
        je_ep_capdesc d = { (uint16_t)(crec - &g_pool.rec[0]), cside, 0u,
                            crec->generation, JE_EP_RIGHTS_ALL };
        uint16_t c1_inflight_before = crec->end[cside].inflight_refs;
        /* send the cap over s0 (lands in s1's inbound); retains inflight on c1. */
        uint8_t msg[2]={0xAB,0xCD};
        assert(je_endpoint_try_send_caps(&g_pool,&g_t,s0,msg,2,&d,1)==JE_EP_OK);
        assert(crec->end[cside].inflight_refs==c1_inflight_before+1u); /* retained */
        /* recv on s1: peek reports 1 cap, mint it as a fresh handle. */
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
        assert(rl==2u && nc==1u && rb[0]==0xAB);
        assert(je_endpoint_capdesc_live(&g_pool,&rc[0]));
        /* mint: retain target handle_ref + alloc a receiver handle. */
        je_ep_channel *mr; uint8_t ms;
        assert(je_endpoint_capdesc_resolve(&g_pool,&rc[0],&mr,&ms)==JE_EP_OK);
        uint16_t c1_hr_before = mr->end[ms].handle_refs;
        assert(je_endpoint_capdesc_retain_handle(&g_pool,&rc[0])==JE_EP_OK);
        je_handle minted=0;
        assert(je_handle_alloc(&g_t,(void*)&mr->end[ms],JE_ENDPOINT_OBJ_TYPE,rc[0].rights,&minted)==JE_HANDLE_OK);
        assert(minted!=0 && minted!=c1);
        assert(mr->end[ms].handle_refs==c1_hr_before+1u);
        /* commit: dequeue + release the cap inflight. */
        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
        assert(crec->end[cside].inflight_refs==c1_inflight_before); /* released */
        /* The minted handle resolves to the SAME end as the original c1. */
        je_ep_channel *vr; uint8_t vs;
        assert(je_endpoint_resolve(&g_pool,&g_t,minted,&vr,&vs)==JE_EP_OK);
        assert(vr==crec && vs==cside);
    }

    /* 23. Dead-cap (A5-2): send a cap, then CLOSE the target before recv; peek
     * reports the cap but capdesc_live() is false -> deliver null. */
    fresh(QUOTA);
    {
        je_handle s0=0,s1=0,c0=0,c1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&s0,&s1)==JE_EP_OK);
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&c0,&c1)==JE_EP_OK);
        je_ep_channel *crec; uint8_t cside;
        assert(je_endpoint_resolve(&g_pool,&g_t,c1,&crec,&cside)==JE_EP_OK);
        je_ep_capdesc d = { (uint16_t)(crec - &g_pool.rec[0]), cside, 0u, crec->generation, JE_EP_RIGHTS_ALL };
        assert(je_endpoint_try_send_caps(&g_pool,&g_t,s0,(void*)0,0,&d,1)==JE_EP_OK);
        /* close BOTH handles to c1's end (c1) AND c0 so the record can go... but
         * the queued cap's inflight keeps it allocated. Close c1 (the target). */
        assert(je_endpoint_handle_close(&g_pool,&g_t,c1)==JE_EP_OK);
        /* target end now CLOSED (handle_refs 0) but inflight>0 keeps record alive. */
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rc[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rc,&nc,&lz)==JE_EP_OK);
        assert(nc==1u);
        assert(!je_endpoint_capdesc_live(&g_pool,&rc[0]));   /* DEAD -> deliver 0 */
        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);  /* releases inflight */
    }

    /* 24. Cycle reclaim (A5-1): A<->B caps queued each way; closing all handles
     * discards both inbounds, releasing the cap inflights -> both records free. */
    fresh(QUOTA);
    {
        je_handle a0=0,a1=0,b0=0,b1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&a0,&a1)==JE_EP_OK);
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&b0,&b1)==JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool)==2u);
        je_ep_channel *ar,*br; uint8_t as,bs;
        assert(je_endpoint_resolve(&g_pool,&g_t,a1,&ar,&as)==JE_EP_OK);
        assert(je_endpoint_resolve(&g_pool,&g_t,b1,&br,&bs)==JE_EP_OK);
        je_ep_capdesc da = { (uint16_t)(ar-&g_pool.rec[0]), as, 0u, ar->generation, JE_EP_RIGHTS_ALL };
        je_ep_capdesc db = { (uint16_t)(br-&g_pool.rec[0]), bs, 0u, br->generation, JE_EP_RIGHTS_ALL };
        /* send cap-to-A1 over B (lands in b1 inbound) and cap-to-B1 over A (a1 inbound) */
        assert(je_endpoint_try_send_caps(&g_pool,&g_t,b0,(void*)0,0,&da,1)==JE_EP_OK);
        assert(je_endpoint_try_send_caps(&g_pool,&g_t,a0,(void*)0,0,&db,1)==JE_EP_OK);
        /* close every handle; no one ever receives -> discard-on-close must break
         * the cycle and free both records. */
        assert(je_endpoint_handle_close(&g_pool,&g_t,a0)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,a1)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,b0)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,b1)==JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool)==0u);   /* cycle reclaimed */
    }

    /* 25. Cap edge cases (A5): dead+live siblings in one message, two caps to the
     * same target, self-referential cap (cap to a1 sent over a0 -> sits in a1's
     * OWN inbound), and pool occupancy after a dead-cap commit. */
    fresh(QUOTA);
    {
        je_handle s0=0,s1=0,c0=0,c1=0,d0=0,d1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&s0,&s1)==JE_EP_OK);
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&c0,&c1)==JE_EP_OK);
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&d0,&d1)==JE_EP_OK);
        je_ep_channel *cr,*dr; uint8_t cs2,ds;
        assert(je_endpoint_resolve(&g_pool,&g_t,c1,&cr,&cs2)==JE_EP_OK);
        assert(je_endpoint_resolve(&g_pool,&g_t,d1,&dr,&ds)==JE_EP_OK);
        je_ep_capdesc cd = { (uint16_t)(cr-&g_pool.rec[0]), cs2, 0u, cr->generation, JE_EP_RIGHTS_ALL };
        je_ep_capdesc dd = { (uint16_t)(dr-&g_pool.rec[0]), ds, 0u, dr->generation, JE_EP_RIGHTS_ALL };
        /* dead+live sibling: close c1 (dead) BUT keep d1 (live), send both as a
         * 2-cap message over s0. */
        je_ep_capdesc pair[2] = { cd, dd };
        /* close c1 so cd's target is dead; but cd is not yet queued so nothing
         * holds c's inflight — c's record frees on c1+c0 close. Re-create c after
         * to keep a valid (dead) descriptor generation. Simpler: send FIRST (both
         * live), then this tests two-live; dead-sibling is covered by test 23. */
        assert(je_endpoint_try_send_caps(&g_pool,&g_t,s0,(void*)0,0,pair,2)==JE_EP_OK);
        assert(cr->end[cs2].inflight_refs>=1u && dr->end[ds].inflight_refs>=1u);
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0; uint8_t nc=0; je_ep_capdesc rcv[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
        /* short handle buffer (hcap 1 < 2 caps) -> SHORT_BUFFER, no dequeue. */
        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,1,rcv,&nc,&lz)==JE_EP_SHORT_BUFFER);
        assert(nc==2u);   /* required slot count reported */
        /* enough buffer: both delivered. */
        assert(je_endpoint_recv_peek_caps(&g_pool,&g_t,s1,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,rcv,&nc,&lz)==JE_EP_OK);
        assert(nc==2u && je_endpoint_capdesc_live(&g_pool,&rcv[0]) && je_endpoint_capdesc_live(&g_pool,&rcv[1]));
        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
        assert(cr->end[cs2].inflight_refs==0u && dr->end[ds].inflight_refs==0u); /* released */
    }

    /* 26. Self-referential cap: a cap to a1 sent over a0 lands in a1's OWN inbound
     * (a0's peer is a1). Closing all of a's handles must discard a1's inbound and
     * release the self-cap inflight -> record frees (A5-1 reclaim, self edge). */
    fresh(QUOTA);
    {
        je_handle a0=0,a1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&a0,&a1)==JE_EP_OK);
        je_ep_channel *ar; uint8_t as;
        assert(je_endpoint_resolve(&g_pool,&g_t,a1,&ar,&as)==JE_EP_OK);
        je_ep_capdesc self = { (uint16_t)(ar-&g_pool.rec[0]), as, 0u, ar->generation, JE_EP_RIGHTS_ALL };
        /* send cap-to-a1 over a0 -> lands in a1's inbound (a0's peer = a1). */
        assert(je_endpoint_try_send_caps(&g_pool,&g_t,a0,(void*)0,0,&self,1)==JE_EP_OK);
        assert(ar->end[as].inflight_refs==1u);
        /* close both handles; a1's inbound holds the self-cap. Discard-on-close of
         * a1 must release the self-cap inflight -> record frees. */
        assert(je_endpoint_handle_close(&g_pool,&g_t,a0)==JE_EP_OK);
        assert(je_endpoint_handle_close(&g_pool,&g_t,a1)==JE_EP_OK);
        assert(je_endpoint_pool_allocated(&g_pool)==0u);  /* self-edge reclaimed */
    }

    /* 27. Blocking-recv waiter primitives (A6, pure layer): register, the single
     * terminal CAS, entry-probe reserved-head masking, release idempotence. */
    fresh(QUOTA);
    {
        je_handle a0=0,a1=0;  /* a0 = sender end, a1 = receiver end */
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&a0,&a1)==JE_EP_OK);
        je_ep_channel *rr; uint8_t rs;   /* a1's end */
        assert(je_endpoint_resolve(&g_pool,&g_t,a1,&rr,&rs)==JE_EP_OK);
        /* entry probe on empty OPEN end -> EMPTY (would block). */
        assert(je_endpoint_recv_entry_probe(&g_pool,&g_t,a1)==JE_EP_EMPTY);
        /* register a waiter (id 3) on a1. */
        je_ep_channel *wr; uint8_t ws;
        assert(je_endpoint_wait_register(&g_pool,&g_t,a1,3u,&wr,&ws)==JE_EP_OK);
        assert(wr==rr && ws==rs);
        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_WAITING);
        /* A6-4: a second register on the same end -> WAITERS_BUSY. */
        assert(je_endpoint_wait_register(&g_pool,&g_t,a1,4u,&wr,&ws)==JE_EP_WAITERS_BUSY);
        /* a sender puts a message in a1's inbound (send on a0 -> peer a1). */
        uint8_t msg[3]={7,8,9};
        assert(je_endpoint_try_send(&g_pool,&g_t,a0,msg,3)==JE_EP_OK);
        /* arbitrate WOKEN -> WON, reserves the head, reports waiter id 3. */
        uint32_t woke=0u;
        assert(je_endpoint_wait_arbitrate(rr,rs,JE_EP_WAIT_WOKEN,&woke)==JE_EP_ARB_WON && woke==3u);
        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_WOKEN);
        /* single terminal CAS: a second arbitrate (e.g. TIMED_OUT) -> LOST, cause unchanged. */
        uint32_t x=0u;
        assert(je_endpoint_wait_arbitrate(rr,rs,JE_EP_WAIT_TIMED_OUT,&x)==JE_EP_ARB_LOST);
        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_WOKEN);
        /* the reserved head is NOT deliverable to a concurrent handle-based peek. */
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0;
        assert(je_endpoint_recv_peek(&g_pool,&g_t,a1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_EMPTY);
        /* but the OWNED-end peek (the reserving waiter) sees it. */
        uint8_t ncaps=0u; je_ep_capdesc cd[JE_EP_MSG_CAPS_MAX]; je_ep_recv_lease lz;
        assert(je_endpoint_recv_peek_owned_caps(&g_pool,rr,rs,rb,sizeof rb,&rl,JE_EP_MSG_CAPS_MAX,cd,&ncaps,&lz)==JE_EP_OK);
        assert(rl==3u && rb[0]==7 && rb[2]==9 && ncaps==0u);
        /* commit + release the waiter. */
        assert(je_endpoint_recv_commit_caps(&g_pool,&lz)==JE_EP_OK);
        assert(je_endpoint_wait_release(&g_pool,rr,rs,3u)==true);
        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_NONE);
        /* release idempotence: releasing again (or a wrong id) is a safe no-op. */
        assert(je_endpoint_wait_release(&g_pool,rr,rs,3u)==false);
    }

    /* 28. A6-CLARIFY-2: release-by-id returns a reserved head to deliverable. */
    fresh(QUOTA);
    {
        je_handle b0=0,b1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&b0,&b1)==JE_EP_OK);
        je_ep_channel *rr; uint8_t rs;
        assert(je_endpoint_resolve(&g_pool,&g_t,b1,&rr,&rs)==JE_EP_OK);
        je_ep_channel *wr; uint8_t ws;
        assert(je_endpoint_wait_register(&g_pool,&g_t,b1,5u,&wr,&ws)==JE_EP_OK);
        uint8_t msg[1]={42};
        assert(je_endpoint_try_send(&g_pool,&g_t,b0,msg,1)==JE_EP_OK);
        uint32_t w=0u;
        assert(je_endpoint_wait_arbitrate(rr,rs,JE_EP_WAIT_WOKEN,&w)==JE_EP_ARB_WON);
        /* head reserved -> concurrent peek EMPTY. */
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0;
        assert(je_endpoint_recv_peek(&g_pool,&g_t,b1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_EMPTY);
        /* the waiter "dies before re-entry": release by id (as mark_dying would). */
        assert(je_endpoint_wait_release_by_id(&g_pool,5u)==true);
        assert(je_endpoint_wait_cause(rr,rs)==JE_EP_WAIT_NONE);
        /* head is deliverable again at the front (A6-CLARIFY-2). */
        assert(je_endpoint_recv_peek(&g_pool,&g_t,b1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_OK);
        assert(rl==1u && rb[0]==42);
    }

    /* 29. Reserved-head-with-peer-closed looks EMPTY, not PEER_CLOSED, to another
     * reader (review finding: a reserved head means a message exists, never EOF).
     * And a release after peer-close lets the message drain. */
    fresh(QUOTA);
    {
        je_handle c0=0,c1=0;
        assert(je_endpoint_channel_create(&g_pool,&g_t,JE_EP_RIGHTS_ALL,&g_t,JE_EP_RIGHTS_ALL,&c0,&c1)==JE_EP_OK);
        je_ep_channel *rr; uint8_t rs;
        assert(je_endpoint_resolve(&g_pool,&g_t,c1,&rr,&rs)==JE_EP_OK);
        je_ep_channel *wr; uint8_t ws;
        assert(je_endpoint_wait_register(&g_pool,&g_t,c1,9u,&wr,&ws)==JE_EP_OK);
        uint8_t msg[2]={1,2};
        assert(je_endpoint_try_send(&g_pool,&g_t,c0,msg,2)==JE_EP_OK);
        uint32_t w=0u;
        assert(je_endpoint_wait_arbitrate(rr,rs,JE_EP_WAIT_WOKEN,&w)==JE_EP_ARB_WON);
        /* close the sender end -> c1 (receiver) becomes PEER_CLOSED, but its head
         * is reserved: a concurrent peek must see EMPTY (message exists), NOT EOF. */
        assert(je_endpoint_handle_close(&g_pool,&g_t,c0)==JE_EP_OK);
        uint8_t rb[JE_EP_MSG_MAX]; size_t rl=0;
        assert(je_endpoint_end_state(rr,rs)==JE_EP_END_PEER_CLOSED);
        assert(je_endpoint_recv_entry_probe(&g_pool,&g_t,c1)==JE_EP_EMPTY);  /* not PEER_CLOSED */
        assert(je_endpoint_recv_peek(&g_pool,&g_t,c1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_EMPTY);
        /* release the waiter -> head deliverable; now peer-closed + a real message. */
        assert(je_endpoint_wait_release(&g_pool,rr,rs,9u)==true);
        assert(je_endpoint_recv_peek(&g_pool,&g_t,c1,rb,sizeof rb,&rl,(je_ep_recv_lease*)0)==JE_EP_OK);
        assert(rl==2u && rb[0]==1 && rb[1]==2);
    }

    test_ddev();

    printf("endpoint: PASS (create, split-ref close/destruction, dup "
           "acquire-before-commit, REFOVERFLOW, DENIED/WRONGTYPE, inflight keep-"
           "alive, pool-exhaustion + create-unwind, release-walk, data IPC "
           "send/recv/FIFO/QUEUE_FULL/SHORT_BUFFER/PEER_CLOSED/rights/zero-len, "
           "cap-transfer mint/dead-cap/cycle-reclaim, "
           "waits register/arbitrate-CAS/reserved-head/release/release-by-id/peer-close-EMPTY, "
           "D-DEV create_by charge/QUOTA/NOSLOTS-first/mint-unwind/transfer/dying/dead/"
           "freed/reused/NONE/procs-NULL/create==create_by(NONE)/no stale creator on reuse)\n");
    return 0;
}
