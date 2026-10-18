/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3c sub-PR 2 (ADR 0009 D-SEQ step 2): endpoint/channel lifecycle core.
 * Hostable, no kernel state, no ABI include — see endpoint.h for the full
 * contract and the load-bearing table-purity precondition. Sound only under the
 * ADR 0008 D6 uniprocessor / IF=0 precondition (no SMP claim).
 */
#include <jeios/endpoint.h>

/* Policy-bound pins (ADR 0009 D4): REF_MAX must admit every non-handle owning
 * ref (queue depth + waiters) and still be small enough to reach in a test, and
 * it must fit the uint16_t counters. */
_Static_assert(JE_EP_REF_MAX >= JE_EP_QUEUE_DEPTH + JE_EP_MAX_WAITERS,
               "JE_EP_REF_MAX must cover queue depth + waiters");
_Static_assert(JE_EP_REF_MAX <= 0xffffu, "ref counts are uint16_t");
_Static_assert(JE_EP_POOL_CAP >= 1u, "pool must have at least one record");
_Static_assert(JE_EP_MSG_MAX <= 255u, "per-slot length fits uint8_t");
_Static_assert(JE_EP_QUEUE_DEPTH <= 255u, "head/count fit uint8_t");
/* Keep the static kernel pool bounded: 2 inbound queues per record dominate. */
_Static_assert(sizeof(je_ep_channel) <= 640u, "channel record size budget");

/* ---- pool / record helpers ------------------------------------------------ */

/* Reset (empty + zero) one inbound queue. Zeroing the payload bytes prevents a
 * stale message surviving into a reused slot and never lets uninitialized bytes
 * reach a reader (defense in depth; recv copies only len bytes regardless). */
static void queue_reset(je_ep_queue *q)
{
    q->head = 0u;
    q->count = 0u;
    for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i) {
        q->len[i] = 0u;
        for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[i][b] = 0u;
    }
}

void je_endpoint_pool_init(je_endpoint_pool *pool)
{
    if (!pool) return;
    pool->allocated_count = 0u;
    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        je_ep_channel *r = &pool->rec[i];
        r->allocated = false;
        r->generation = 1u;            /* never 0 */
        for (uint8_t s = 0; s < 2u; ++s) {
            r->end[s].state = JE_EP_END_FREE;
            r->end[s].handle_refs = 0u;
            r->end[s].inflight_refs = 0u;
            r->end[s].side = s;
            queue_reset(&r->end[s].inbound);
        }
    }
}

uint32_t je_endpoint_pool_allocated(const je_endpoint_pool *pool)
{
    return pool ? pool->allocated_count : 0u;
}

je_ep_end_state je_endpoint_end_state(const je_ep_channel *rec, uint8_t side)
{
    if (!rec || side > 1u) return JE_EP_END_FREE;
    return rec->end[side].state;
}

/* The opaque object pointer a handle slot binds for an endpoint is &rec->end[s].
 * Map such a pointer back to (rec, side), validating it lies on a real end of a
 * LIVE record in THIS pool — never dereference an unvalidated pointer. */
static je_ep_status object_to_end(const je_endpoint_pool *pool, const void *obj,
                                  je_ep_channel **out_rec, uint8_t *out_side)
{
    if (!pool || !obj) return JE_EP_CORRUPT;
    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        const je_ep_channel *r = &pool->rec[i];
        for (uint8_t s = 0; s < 2u; ++s) {
            if ((const void *)&r->end[s] == obj) {
                if (!r->allocated) return JE_EP_CORRUPT;   /* stale end pointer */
                *out_rec = (je_ep_channel *)r;
                *out_side = s;
                return JE_EP_OK;
            }
        }
    }
    return JE_EP_CORRUPT;
}

/* Free a record once BOTH ends hold no references of either class. */
static void maybe_free_record(je_endpoint_pool *pool, je_ep_channel *rec)
{
    for (uint8_t s = 0; s < 2u; ++s) {
        if (rec->end[s].handle_refs != 0u || rec->end[s].inflight_refs != 0u)
            return;
    }
    /* Both ends fully unreferenced: reclaim. Bump generation (never 0). */
    rec->allocated = false;
    rec->generation = (rec->generation == 0xffffffffu) ? 1u
                                                       : rec->generation + 1u;
    for (uint8_t s = 0; s < 2u; ++s) {
        rec->end[s].state = JE_EP_END_FREE;
        rec->end[s].side = s;
        queue_reset(&rec->end[s].inbound);   /* no stale payload into reuse */
    }
    if (pool->allocated_count) pool->allocated_count--;
}

/* Map je_handle_status -> je_ep_status for the resolve/alloc paths. */
static je_ep_status hstat(je_handle_status s)
{
    switch (s) {
    case JE_HANDLE_OK:        return JE_EP_OK;
    case JE_HANDLE_BADINDEX:  return JE_EP_BADINDEX;
    case JE_HANDLE_STALE:     return JE_EP_STALE;
    case JE_HANDLE_WRONGTYPE: return JE_EP_WRONGTYPE;
    case JE_HANDLE_DENIED:    return JE_EP_DENIED;
    case JE_HANDLE_QUOTA:     return JE_EP_QUOTA;
    case JE_HANDLE_NOSLOTS:   return JE_EP_NOSLOTS;
    }
    return JE_EP_CORRUPT;   /* fail closed if the enum ever grows */
}

/* ---- resolve a handle to its end ----------------------------------------- */

je_ep_status je_endpoint_resolve(const je_endpoint_pool *pool,
                                 const je_handle_table *t, je_handle h,
                                 je_ep_channel **out_rec, uint8_t *out_side)
{
    void *obj = (void *)0;
    je_handle_status s;
    if (!pool || !t || !out_rec || !out_side) return JE_EP_BADINDEX;
    /* Resolve as an ENDPOINT with no required rights; keeps the table's
     * BADINDEX -> STALE -> WRONGTYPE precedence. */
    s = je_handle_lookup(t, h, JE_ENDPOINT_OBJ_TYPE, 0u, &obj);
    if (s != JE_HANDLE_OK) return hstat(s);
    return object_to_end(pool, obj, out_rec, out_side);
}

/* ---- internal handle_ref acquire / release ------------------------------- */

/* Acquire one handle_ref on an end; fail closed past REF_MAX with nothing taken. */
static je_ep_status retain_handle(je_ep_end *e)
{
    if (e->handle_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
    e->handle_refs++;
    return JE_EP_OK;
}

/* Drop one handle_ref on an end that definitely holds at least one (infallible
 * for a valid live ref). On reaching 0: this end CLOSED, a live peer -> PEER_CLOSED,
 * then try to reclaim the record. */
static void release_handle(je_endpoint_pool *pool, je_ep_channel *rec, uint8_t side)
{
    je_ep_end *e = &rec->end[side];
    /* handle_refs == 0 here would be an invariant violation by the caller; guard
     * defensively (never underflow) but do not mask it as success elsewhere. */
    if (e->handle_refs == 0u) return;
    e->handle_refs--;
    if (e->handle_refs == 0u) {
        e->state = JE_EP_END_CLOSED;
        je_ep_end *peer = &rec->end[side ^ 1u];
        if (peer->state == JE_EP_END_OPEN) peer->state = JE_EP_END_PEER_CLOSED;
        /* Discard THIS (closing) end's inbound queue: no one can ever read it
         * again. The peer's inbound is left intact so a survivor can drain it
         * (D6). Data messages own no refs, so this never affects the free rule. */
        queue_reset(&e->inbound);
        maybe_free_record(pool, rec);
    }
}

/* ---- inflight ref API (tested directly; used by queue/wait PRs) ----------- */

/* Is `rec` one of this pool's channel records? Uses uintptr_t comparison (not
 * relational pointer comparison, which is UB for a pointer outside the array)
 * and requires exact slot alignment, so a forged/other-pool pointer fails. */
static bool rec_in_pool(const je_endpoint_pool *pool, const je_ep_channel *rec)
{
    uintptr_t base = (uintptr_t)&pool->rec[0];
    uintptr_t top  = (uintptr_t)&pool->rec[JE_EP_POOL_CAP];   /* one-past-end */
    uintptr_t p    = (uintptr_t)rec;
    if (p < base || p >= top) return false;
    return ((p - base) % sizeof(pool->rec[0])) == 0u;         /* slot-aligned */
}

je_ep_status je_endpoint_retain_inflight(je_endpoint_pool *pool,
                                         je_ep_channel *rec, uint8_t side)
{
    if (!pool || !rec || side > 1u) return JE_EP_CORRUPT;
    /* Provenance: rec MUST be one of this pool's records (reject a wrong-pool or
     * stray pointer, which would otherwise corrupt a different channel's count).
     * NOTE: this does NOT defend against a stale pointer to a since-freed-and-
     * reused record in the SAME pool — the ADR 0009 borrowed-pointer rule makes
     * that a trusted-caller precondition, and the queue/wait PR that first holds
     * such a pair MUST carry the record generation to detour reuse. */
    if (!rec_in_pool(pool, rec) || !rec->allocated) return JE_EP_CORRUPT;
    je_ep_end *e = &rec->end[side];
    if (e->inflight_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
    e->inflight_refs++;
    return JE_EP_OK;
}

je_ep_status je_endpoint_release_inflight(je_endpoint_pool *pool,
                                          je_ep_channel *rec, uint8_t side)
{
    if (!pool || !rec || side > 1u) return JE_EP_CORRUPT;
    if (!rec_in_pool(pool, rec) || !rec->allocated) return JE_EP_CORRUPT;
    je_ep_end *e = &rec->end[side];
    if (e->inflight_refs == 0u) return JE_EP_CORRUPT;   /* underflow guard */
    e->inflight_refs--;
    if (e->inflight_refs == 0u) maybe_free_record(pool, rec);
    return JE_EP_OK;
}

/* ---- create (kernel-only) ------------------------------------------------- */

/* Return a tentatively-allocated record to the FREE state on a create unwind,
 * restoring the FREE <=> !allocated invariant (states, not just the flag). The
 * record was never observed by any caller, so no generation bump is needed. */
static void unwind_record(je_ep_channel *rec)
{
    rec->allocated = false;
    for (uint8_t s = 0; s < 2u; ++s) {
        rec->end[s].state = JE_EP_END_FREE;
        rec->end[s].handle_refs = 0u;
        rec->end[s].inflight_refs = 0u;
        rec->end[s].side = s;
    }
}

je_ep_status je_endpoint_channel_create(je_endpoint_pool *pool,
                                        je_handle_table *t0, uint32_t rights0,
                                        je_handle_table *t1, uint32_t rights1,
                                        je_handle *out0, je_handle *out1)
{
    if (out0) *out0 = 0u;
    if (out1) *out1 = 0u;
    if (!pool || !t0 || !t1 || !out0 || !out1) return JE_EP_BADINDEX;
    /* Aliased outputs would make the second write clobber the first handle,
     * orphaning end 0's LIVE slot (a permanently unreclaimable ref). Reject. */
    if (out0 == out1) return JE_EP_BADINDEX;
    /* Reject rights with any unknown endpoint bit (defense in depth; the glue
     * also checks). */
    if ((rights0 & ~(uint32_t)JE_EP_RIGHTS_ALL)
        || (rights1 & ~(uint32_t)JE_EP_RIGHTS_ALL)) return JE_EP_BADINDEX;

    /* Find a FREE record (lowest index). */
    je_ep_channel *rec = (je_ep_channel *)0;
    for (uint32_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        if (!pool->rec[i].allocated) { rec = &pool->rec[i]; break; }
    }
    if (!rec) return JE_EP_NOSLOTS;   /* pool exhausted */

    /* Tentatively allocate the record and open both ends. */
    rec->allocated = true;
    for (uint8_t s = 0; s < 2u; ++s) {
        rec->end[s].state = JE_EP_END_OPEN;
        rec->end[s].handle_refs = 0u;
        rec->end[s].inflight_refs = 0u;
        rec->end[s].side = s;
    }

    /* Mint end 0: retain then alloc. retain of a fresh end cannot overflow. */
    je_handle h0 = 0u, h1 = 0u;
    je_ep_status rs = retain_handle(&rec->end[0]);
    if (rs != JE_EP_OK) { unwind_record(rec); return rs; }   /* unreachable */
    je_handle_status a0 = je_handle_alloc(t0, (void *)&rec->end[0],
                                          JE_ENDPOINT_OBJ_TYPE, rights0, &h0);
    if (a0 != JE_HANDLE_OK) {
        unwind_record(rec);                /* record never observed */
        return hstat(a0);
    }

    /* Mint end 1 similarly. */
    rs = retain_handle(&rec->end[1]);
    if (rs != JE_EP_OK) {                   /* unreachable; unwind end 0 fully */
        (void)je_handle_close(t0, h0);
        unwind_record(rec);
        return rs;
    }
    je_handle_status a1 = je_handle_alloc(t1, (void *)&rec->end[1],
                                          JE_ENDPOINT_OBJ_TYPE, rights1, &h1);
    if (a1 != JE_HANDLE_OK) {
        /* Roll back the whole transaction: close h0, drop both retains, free. */
        (void)je_handle_close(t0, h0);
        unwind_record(rec);
        return hstat(a1);
    }

    pool->allocated_count++;
    *out0 = h0;
    *out1 = h1;
    return JE_EP_OK;
}

/* ---- close seam ----------------------------------------------------------- */

je_ep_status je_endpoint_handle_close(je_endpoint_pool *pool,
                                      je_handle_table *t, je_handle h)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    je_ep_status s = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (s != JE_EP_OK) return s;
    /* Invalidate the table slot FIRST (so no LIVE slot ever names a freed end),
     * then drop the handle_ref. Under IF=0 a resolved handle always closes. */
    je_handle_status c = je_handle_close(t, h);
    if (c != JE_HANDLE_OK) return hstat(c);   /* defensive; not expected */
    release_handle(pool, rec, side);
    return JE_EP_OK;
}

/* ---- dup seam (retain-before-commit; endpoints never use dup_self) -------- */

je_ep_status je_endpoint_handle_dup(je_endpoint_pool *pool, je_handle_table *t,
                                    je_handle src, uint32_t new_rights,
                                    je_handle *out)
{
    if (out) *out = 0u;
    if (!pool || !t || !out) return JE_EP_BADINDEX;

    /* Resolve src as an endpoint and read its rights, preserving the table's
     * BADINDEX -> STALE -> WRONGTYPE precedence. */
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    je_ep_status rs = je_endpoint_resolve(pool, t, src, &rec, &side);
    if (rs != JE_EP_OK) return rs;

    uint32_t src_rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, src, JE_ENDPOINT_OBJ_TYPE, 0u,
                                              &src_rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);

    /* Subset check: every requested bit must be held (equality allowed). */
    if (new_rights & ~src_rights) return JE_EP_DENIED;

    /* Acquire BEFORE commit (ADR 0009 D8). Overflow acquires nothing. */
    je_ep_status ac = retain_handle(&rec->end[side]);
    if (ac != JE_EP_OK) return ac;   /* JE_EP_REFOVERFLOW */

    je_handle nh = 0u;
    je_handle_status a = je_handle_alloc(t, (void *)&rec->end[side],
                                         JE_ENDPOINT_OBJ_TYPE, new_rights, &nh);
    if (a != JE_HANDLE_OK) {
        /* Roll back the retain. src still holds a handle_ref, so this can never
         * drop the end to 0 / trigger destruction. */
        rec->end[side].handle_refs--;
        return hstat(a);
    }
    *out = nh;
    return JE_EP_OK;
}

/* ---- process-death release walk (A3-1) ----------------------------------- */

int je_endpoint_release_table(je_endpoint_pool *pool, je_handle_table *t)
{
    if (!pool || !t) return -1;
    int released = 0;
    /* Reading t->capacity is table METADATA (public struct field), not slot
     * internals; slot contents are reached only via the read-only iterator. */
    for (uint32_t i = 0; i < t->capacity; ++i) {
        je_handle h = 0u;
        uint32_t type = 0u;
        if (!je_handle_live_at(t, i, &h, &type)) continue;
        if (type != JE_ENDPOINT_OBJ_TYPE) continue;   /* leave SELF etc. to the zero */
        je_ep_status s = je_endpoint_handle_close(pool, t, h);
        if (s != JE_EP_OK) return -1;   /* invariant break: caller must fail-stop */
        released++;
    }
    /* Post-condition: no endpoint slot may remain LIVE. */
    for (uint32_t i = 0; i < t->capacity; ++i) {
        je_handle h = 0u; uint32_t type = 0u;
        if (je_handle_live_at(t, i, &h, &type) && type == JE_ENDPOINT_OBJ_TYPE)
            return -1;   /* a LIVE endpoint survived the walk -> fatal */
    }
    return released;
}

/* ---- non-blocking data IPC (M3c step 4, D6 / D9) ------------------------- */

uint8_t je_endpoint_inbound_count(const je_ep_channel *rec, uint8_t side)
{
    if (!rec || side > 1u) return 0u;
    return rec->end[side].inbound.count;
}

je_ep_status je_endpoint_send_preflight(je_endpoint_pool *pool, je_handle_table *t,
                                        je_handle h, size_t len)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (len > JE_EP_MSG_MAX) return JE_EP_INVALID;

    /* Resolve + SEND right (BADINDEX -> STALE -> WRONGTYPE -> DENIED). */
    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_SEND, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);   /* DENIED if SEND not held */

    /* A message delivered on end `side` lands in the PEER end's inbound. If THIS
     * end is peer-closed, nobody will ever read it -> PEER_CLOSED. */
    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
    if (rec->end[side ^ 1u].inbound.count >= JE_EP_QUEUE_DEPTH)
        return JE_EP_QUEUE_FULL;
    return JE_EP_OK;
}

je_ep_status je_endpoint_try_send(je_endpoint_pool *pool, je_handle_table *t,
                                  je_handle h, const void *kbuf, size_t len)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (len != 0u && !kbuf) return JE_EP_INVALID;
    /* Re-run the full preflight (the glue ran it once before the copy; this is
     * the same pure validator, so the check+enqueue are atomic under IF=0 with
     * no new logic). */
    je_ep_status pf = je_endpoint_send_preflight(pool, t, h, len);
    if (pf != JE_EP_OK) return pf;
    (void)je_endpoint_resolve(pool, t, h, &rec, &side);   /* cannot fail now */

    je_ep_queue *q = &rec->end[side ^ 1u].inbound;   /* peer's inbound */
    uint8_t tail = (uint8_t)((q->head + q->count) % JE_EP_QUEUE_DEPTH);
    for (uint8_t b = 0; b < (uint8_t)len; ++b)
        q->buf[tail][b] = ((const uint8_t *)kbuf)[b];
    q->len[tail] = (uint8_t)len;
    q->count++;
    return JE_EP_OK;
}

je_ep_status je_endpoint_recv_peek(je_endpoint_pool *pool, je_handle_table *t,
                                   je_handle h, void *out_kbuf, size_t cap,
                                   size_t *out_len, je_ep_recv_lease *lease)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (!out_len) return JE_EP_INVALID;
    *out_len = 0u;

    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_RECV, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);   /* DENIED if RECV not held */

    je_ep_queue *q = &rec->end[side].inbound;    /* this end's own inbound */
    if (q->count == 0u) {
        /* Empty: EOF if the peer is gone, else would-block EMPTY. */
        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;
    }
    uint8_t mlen = q->len[q->head];
    *out_len = mlen;                 /* report length even on SHORT_BUFFER */
    if (cap < mlen) return JE_EP_SHORT_BUFFER;   /* no copy, no dequeue */
    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;   /* NULL dst guard */
    for (uint8_t b = 0; b < mlen; ++b)
        ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
    if (lease) {
        lease->rec = rec;
        lease->side = side;
        lease->generation = rec->generation;
        lease->head = q->head;
        lease->len = mlen;
    }
    return JE_EP_OK;
}

je_ep_status je_endpoint_recv_commit(je_endpoint_pool *pool,
                                     const je_ep_recv_lease *lease)
{
    if (!pool || !lease || !lease->rec) return JE_EP_CORRUPT;
    je_ep_channel *rec = lease->rec;
    if (!rec_in_pool(pool, rec)) return JE_EP_CORRUPT;
    /* The lease must still name the exact head it observed: record allocated,
     * same generation, same side, nonempty, same head index, same length. Under
     * IF=0 single-CPU this always holds between peek and commit (nothing can run
     * in between); a mismatch is a kernel invariant violation, never ordinary. */
    if (!rec->allocated || rec->generation != lease->generation || lease->side > 1u)
        return JE_EP_CORRUPT;
    je_ep_queue *q = &rec->end[lease->side].inbound;
    if (q->count == 0u || q->head != lease->head || q->len[q->head] != lease->len)
        return JE_EP_CORRUPT;
    /* Dequeue the head: zero the vacated slot, advance. */
    q->len[q->head] = 0u;
    for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[q->head][b] = 0u;
    q->head = (uint8_t)((q->head + 1u) % JE_EP_QUEUE_DEPTH);
    q->count--;
    return JE_EP_OK;
}
