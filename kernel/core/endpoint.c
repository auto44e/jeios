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
_Static_assert(JE_EP_QUEUE_DEPTH * JE_EP_MSG_CAPS_MAX <= JE_EP_REF_MAX,
               "a full queue of cap-carrying msgs must not overflow inflight");
/* Keep the static kernel pool bounded: the per-slot byte buffers + cap
 * descriptors dominate. Grew for step-5 caps; re-pinned here. */
_Static_assert(sizeof(je_ep_channel) <= 1280u, "channel record size budget");

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
        q->cap_count[i] = 0u;
        for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[i][b] = 0u;
        for (uint8_t c = 0; c < JE_EP_MSG_CAPS_MAX; ++c) {
            q->cap[i][c].rec_index = 0u; q->cap[i][c].side = 0u;
            q->cap[i][c].cap_count_pad = 0u;
            q->cap[i][c].generation = 0u; q->cap[i][c].rights = 0u;
        }
    }
}

/* Count the capability descriptors currently queued anywhere in one inbound
 * queue (used by the A5-3 free-time assertion: a record only frees when BOTH
 * inbound queues are cap-free). */
static uint32_t queue_cap_total(const je_ep_queue *q)
{
    uint32_t n = 0u;
    for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i) n += q->cap_count[i];
    return n;
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
    /* A5-3 idempotence guard: the cap-release discard path can reach this twice
     * for one record; a second call on an already-free record must be a no-op
     * (never double-decrement allocated_count / double-bump generation). */
    if (!rec->allocated) return;
    for (uint8_t s = 0; s < 2u; ++s) {
        if (rec->end[s].handle_refs != 0u || rec->end[s].inflight_refs != 0u)
            return;
    }
    /* A5-3: a record only frees when BOTH ends have zero refs. With zero inflight
     * refs there can be no queued caps pointing at this record, and this record's
     * own inbound queues must themselves be cap-free. If a residual cap is somehow
     * present it means an inflight ref was leaked — an invariant violation; guard
     * by NOT freeing (fail-safe: leak the record rather than free it with a live
     * cap), which is unreachable under the detach-before-release discard order. */
    for (uint8_t s = 0; s < 2u; ++s) {
        if (queue_cap_total(&rec->end[s].inbound) != 0u) return; /* unreachable; fail-safe */
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
/* Release one inflight ref on the end a queued cap descriptor points at (A5-3
 * discard path). The target is in THIS pool; a guarded maybe_free_record may free
 * it. Defence-in-depth: validate the descriptor resolves to an allocated,
 * generation-matching end (it must, while the inflight ref kept it alive). */
static void release_capdesc_inflight(je_endpoint_pool *pool,
                                     const je_ep_capdesc *d)
{
    if (d->rec_index >= JE_EP_POOL_CAP || d->side > 1u) return; /* unreachable */
    je_ep_channel *tr = &pool->rec[d->rec_index];
    if (!tr->allocated || tr->generation != d->generation) return; /* unreachable */
    je_ep_end *te = &tr->end[d->side];
    if (te->inflight_refs == 0u) return;   /* underflow guard (unreachable) */
    te->inflight_refs--;
    if (te->inflight_refs == 0u) maybe_free_record(pool, tr);
}

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
        /* A5-3 discard ordering: (1) DETACH every cap descriptor still queued in
         * this closing end's inbound into a bounded local array; (2) reset the
         * queue (bytes + cap storage) so this record is fully settled and holds
         * no caps; (3) maybe_free THIS record; (4) ONLY THEN release each detached
         * inflight ref on its (other-record) target. Doing releases last means
         * freeing a target never re-enters a queue still being discarded, and the
         * guarded maybe_free_record is idempotent if a target is this same record
         * via the peer end (its inbound was/will be discarded at its own close). */
        je_ep_capdesc detached[JE_EP_QUEUE_DEPTH * JE_EP_MSG_CAPS_MAX];
        uint32_t nd = 0u;
        je_ep_queue *q = &e->inbound;
        for (uint8_t i = 0; i < JE_EP_QUEUE_DEPTH; ++i)
            for (uint8_t c = 0; c < q->cap_count[i]; ++c)
                detached[nd++] = q->cap[i][c];
        queue_reset(q);                  /* this end holds no caps now */
        maybe_free_record(pool, rec);    /* settle THIS record first */
        for (uint32_t k = 0; k < nd; ++k)
            release_capdesc_inflight(pool, &detached[k]);  /* then other records */
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

/* Send preflight WITHOUT the QUEUE_FULL check: main-handle resolve + SEND right +
 * self-end PEER_CLOSED only. The glue runs this first, then resolves/validates the
 * caps (so per-cap DENIED/STALE outrank QUEUE_FULL per A5-4's frozen precedence),
 * and the final pure try_send_caps enforces QUEUE_FULL atomically at enqueue. */
je_ep_status je_endpoint_send_preflight_main(je_endpoint_pool *pool,
                                             je_handle_table *t, je_handle h,
                                             size_t len)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (len > JE_EP_MSG_MAX) return JE_EP_INVALID;
    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_SEND, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);
    if (rec->end[side].state == JE_EP_END_PEER_CLOSED
        || rec->end[side].state == JE_EP_END_CLOSED) return JE_EP_PEER_CLOSED;
    return JE_EP_OK;   /* NO queue-full check here */
}

je_ep_status je_endpoint_try_send(je_endpoint_pool *pool, je_handle_table *t,
                                  je_handle h, const void *kbuf, size_t len)
{
    /* Data-only send is exactly the zero-cap case. Delegate so the two paths
     * cannot drift (and the enqueue always writes cap_count[tail]=0). */
    return je_endpoint_try_send_caps(pool, t, h, kbuf, len, (const je_ep_capdesc *)0, 0u);
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
    /* Delegate to the cap-aware commit (which also releases any queued-cap
     * inflight refs and zeroes cap storage). A zero-cap message releases nothing;
     * same behaviour as before, but the two paths cannot drift. */
    return je_endpoint_recv_commit_caps(pool, lease);
}

/* ---- capability transfer (M3c step 5, ADR 0009 A5) ----------------------- */

bool je_endpoint_capdesc_live(const je_endpoint_pool *pool, const je_ep_capdesc *d)
{
    if (!pool || !d || d->rec_index >= JE_EP_POOL_CAP || d->side > 1u) return false;
    const je_ep_channel *tr = &pool->rec[d->rec_index];
    if (!tr->allocated || tr->generation != d->generation) return false; /* stale */
    je_ep_end_state st = tr->end[d->side].state;
    /* OPEN and PEER_CLOSED are LIVE (mintable); only CLOSED/FREE are dead (A5-2). */
    return st == JE_EP_END_OPEN || st == JE_EP_END_PEER_CLOSED;
}

je_ep_status je_endpoint_capdesc_resolve(je_endpoint_pool *pool,
                                         const je_ep_capdesc *d,
                                         je_ep_channel **out_rec, uint8_t *out_side)
{
    if (!pool || !d || !out_rec || !out_side) return JE_EP_CORRUPT;
    if (!je_endpoint_capdesc_live(pool, d)) return JE_EP_STALE;
    *out_rec = &pool->rec[d->rec_index];
    *out_side = d->side;
    return JE_EP_OK;
}

je_ep_status je_endpoint_capdesc_retain_handle(je_endpoint_pool *pool,
                                               const je_ep_capdesc *d)
{
    je_ep_channel *tr; uint8_t ts;
    je_ep_status s = je_endpoint_capdesc_resolve(pool, d, &tr, &ts);
    if (s != JE_EP_OK) return s;
    return retain_handle(&tr->end[ts]);   /* REFOVERFLOW if at the bound */
}

void je_endpoint_capdesc_release_handle(je_endpoint_pool *pool,
                                        const je_ep_capdesc *d)
{
    je_ep_channel *tr; uint8_t ts;
    if (je_endpoint_capdesc_resolve(pool, d, &tr, &ts) != JE_EP_OK) return;
    /* Undo a mint-path retain: drop one handle_ref (may free if it was the last,
     * but a mint rollback happens while the descriptor's inflight ref still holds
     * the target, so this cannot reach 0). */
    release_handle(pool, tr, ts);
}

/* Validate the batch of descriptors the glue resolved: count bound, each target
 * live, each descriptor's rights a subset of the target's CURRENT rights. Pure. */
static je_ep_status caps_validate(je_endpoint_pool *pool, const je_ep_capdesc *caps,
                                  uint8_t ncaps)
{
    if (ncaps > JE_EP_MSG_CAPS_MAX) return JE_EP_INVALID;
    for (uint8_t c = 0; c < ncaps; ++c) {
        je_ep_channel *tr; uint8_t ts;
        je_ep_status s = je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts);
        if (s != JE_EP_OK) return s;               /* dead target at send -> STALE */
        /* Descriptor rights must contain only known endpoint bits (defence in
         * depth; the GLUE attenuates against the sender's HANDLE rights before
         * building the descriptor — ends themselves carry no rights to check). */
        if (caps[c].rights & ~(uint32_t)JE_EP_RIGHTS_ALL) return JE_EP_INVALID;
        (void)tr; (void)ts;
    }
    return JE_EP_OK;
}

je_ep_status je_endpoint_send_preflight_caps(je_endpoint_pool *pool,
                                             je_handle_table *t, je_handle h,
                                             size_t len, const je_ep_capdesc *caps,
                                             uint8_t ncaps)
{
    je_ep_status base = je_endpoint_send_preflight(pool, t, h, len);
    if (base != JE_EP_OK) return base;
    if (ncaps == 0u) return JE_EP_OK;
    if (!caps) return JE_EP_INVALID;
    return caps_validate(pool, caps, ncaps);
}

je_ep_status je_endpoint_try_send_caps(je_endpoint_pool *pool, je_handle_table *t,
                                       je_handle h, const void *kbuf, size_t len,
                                       const je_ep_capdesc *caps, uint8_t ncaps)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (len != 0u && !kbuf) return JE_EP_INVALID;
    /* Full preflight (byte + caps) — same validator, re-run so check+enqueue is
     * atomic under IF=0. */
    je_ep_status pf = je_endpoint_send_preflight_caps(pool, t, h, len, caps, ncaps);
    if (pf != JE_EP_OK) return pf;
    (void)je_endpoint_resolve(pool, t, h, &rec, &side);   /* cannot fail now */

    /* RETAIN one inflight ref per cap on each target, rolling back all on any
     * REFOVERFLOW (all-or-nothing). */
    uint8_t retained = 0u;
    for (uint8_t c = 0; c < ncaps; ++c) {
        je_ep_channel *tr; uint8_t ts;
        if (je_endpoint_capdesc_resolve(pool, &caps[c], &tr, &ts) != JE_EP_OK) {
            /* target died between preflight and here — impossible under IF=0;
             * roll back and fail closed. */
            for (uint8_t r = 0; r < retained; ++r) {
                je_ep_channel *rr; uint8_t rs;
                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
                if (rr->end[rs].inflight_refs) rr->end[rs].inflight_refs--;
            }
            return JE_EP_STALE;
        }
        if (tr->end[ts].inflight_refs >= JE_EP_REF_MAX) {
            for (uint8_t r = 0; r < retained; ++r) {
                je_ep_channel *rr; uint8_t rs;
                (void)je_endpoint_capdesc_resolve(pool, &caps[r], &rr, &rs);
                if (rr->end[rs].inflight_refs) rr->end[rs].inflight_refs--;
            }
            return JE_EP_REFOVERFLOW;
        }
        tr->end[ts].inflight_refs++;
        retained++;
    }

    /* Enqueue the message (bytes + cap descriptors) at the peer inbound tail. */
    je_ep_queue *q = &rec->end[side ^ 1u].inbound;
    uint8_t tail = (uint8_t)((q->head + q->count) % JE_EP_QUEUE_DEPTH);
    for (uint8_t b = 0; b < (uint8_t)len; ++b)
        q->buf[tail][b] = ((const uint8_t *)kbuf)[b];
    q->len[tail] = (uint8_t)len;
    for (uint8_t c = 0; c < ncaps; ++c) q->cap[tail][c] = caps[c];
    q->cap_count[tail] = ncaps;
    q->count++;
    return JE_EP_OK;
}

je_ep_status je_endpoint_recv_peek_caps(je_endpoint_pool *pool, je_handle_table *t,
                                        je_handle h, void *out_kbuf, size_t cap,
                                        size_t *out_len, uint8_t hcap,
                                        je_ep_capdesc *out_caps, uint8_t *out_ncaps,
                                        je_ep_recv_lease *lease)
{
    je_ep_channel *rec = (je_ep_channel *)0;
    uint8_t side = 0u;
    if (!out_len || !out_ncaps) return JE_EP_INVALID;
    *out_len = 0u; *out_ncaps = 0u;

    je_ep_status rs = je_endpoint_resolve(pool, t, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(t, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_RECV, &rights);
    if (gr != JE_HANDLE_OK) return hstat(gr);

    je_ep_queue *q = &rec->end[side].inbound;
    if (q->count == 0u)
        return (rec->end[side].state == JE_EP_END_PEER_CLOSED)
               ? JE_EP_PEER_CLOSED : JE_EP_EMPTY;

    uint8_t mlen = q->len[q->head];
    uint8_t mcaps = q->cap_count[q->head];
    *out_len = mlen;
    *out_ncaps = mcaps;
    /* Short EITHER buffer -> SHORT_BUFFER, copy nothing, no dequeue (report both
     * required sizes via out_len / out_ncaps). */
    if (cap < mlen || hcap < mcaps) return JE_EP_SHORT_BUFFER;
    if (mlen != 0u && !out_kbuf) return JE_EP_INVALID;
    if (mcaps != 0u && !out_caps) return JE_EP_INVALID;
    for (uint8_t b = 0; b < mlen; ++b) ((uint8_t *)out_kbuf)[b] = q->buf[q->head][b];
    for (uint8_t c = 0; c < mcaps; ++c) out_caps[c] = q->cap[q->head][c];
    if (lease) {
        lease->rec = rec; lease->side = side;
        lease->generation = rec->generation; lease->head = q->head; lease->len = mlen;
    }
    return JE_EP_OK;
}

je_ep_status je_endpoint_recv_commit_caps(je_endpoint_pool *pool,
                                          const je_ep_recv_lease *lease)
{
    if (!pool || !lease || !lease->rec) return JE_EP_CORRUPT;
    je_ep_channel *rec = lease->rec;
    if (!rec_in_pool(pool, rec)) return JE_EP_CORRUPT;
    if (!rec->allocated || rec->generation != lease->generation || lease->side > 1u)
        return JE_EP_CORRUPT;
    je_ep_queue *q = &rec->end[lease->side].inbound;
    if (q->count == 0u || q->head != lease->head || q->len[q->head] != lease->len)
        return JE_EP_CORRUPT;
    /* Snapshot this head's caps BEFORE zeroing, then release each one's inflight
     * ref (the cap has been delivered — minted or consumed-as-dead by the glue). */
    uint8_t mcaps = q->cap_count[q->head];
    je_ep_capdesc snap[JE_EP_MSG_CAPS_MAX];
    for (uint8_t c = 0; c < mcaps; ++c) snap[c] = q->cap[q->head][c];
    /* Dequeue: zero the vacated slot (bytes + caps), advance. */
    q->len[q->head] = 0u;
    q->cap_count[q->head] = 0u;
    for (uint16_t b = 0; b < JE_EP_MSG_MAX; ++b) q->buf[q->head][b] = 0u;
    for (uint8_t c = 0; c < JE_EP_MSG_CAPS_MAX; ++c) {
        q->cap[q->head][c].rec_index = 0u; q->cap[q->head][c].side = 0u;
        q->cap[q->head][c].cap_count_pad = 0u;
        q->cap[q->head][c].generation = 0u; q->cap[q->head][c].rights = 0u;
    }
    q->head = (uint8_t)((q->head + 1u) % JE_EP_QUEUE_DEPTH);
    q->count--;
    /* Now release each delivered cap's inflight ref (may free the target). Done
     * AFTER dequeue so the queue slot no longer references the target. */
    for (uint8_t c = 0; c < mcaps; ++c) release_capdesc_inflight(pool, &snap[c]);
    return JE_EP_OK;
}
