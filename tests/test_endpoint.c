/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hostable unit tests for the M3c endpoint lifecycle core (ADR 0009 D-SEQ step 2).
 * No kernel state: a local handle table over local slots + a local endpoint pool.
 * Proves destruction-on-last-ref, the split refcount, REFOVERFLOW, and dup
 * acquire-before-commit rollback — all without any ring-3 path (create is
 * kernel-only, exercised directly here).
 */
#include <jeios/endpoint.h>
#include <assert.h>
#include <stdio.h>

#define CAP 16u
#define QUOTA 15u

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
    fresh(CAP);   /* generous table quota so the pool is the limiter */
    {
        je_handle a0[JE_EP_POOL_CAP], a1[JE_EP_POOL_CAP];
        for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) make_pair(&a0[i], &a1[i]);
        assert(je_endpoint_pool_allocated(&g_pool) == JE_EP_POOL_CAP);
        je_handle x0 = 1u, x1 = 1u;
        assert(je_endpoint_channel_create(&g_pool, &g_t, JE_EP_RIGHTS_ALL,
                                          &g_t, JE_EP_RIGHTS_ALL, &x0, &x1)
               == JE_EP_NOSLOTS);
        assert(x0 == 0u && x1 == 0u);
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

    printf("endpoint: PASS (create, split-ref close/destruction, dup "
           "acquire-before-commit, REFOVERFLOW, DENIED/WRONGTYPE, inflight keep-"
           "alive, pool-exhaustion + create-unwind, release-walk, data IPC "
           "send/recv/FIFO/QUEUE_FULL/SHORT_BUFFER/PEER_CLOSED/rights/zero-len)\n");
    return 0;
}
