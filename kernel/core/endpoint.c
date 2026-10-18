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

/* ---- pool / record helpers ------------------------------------------------ */

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
        /* Inbound queue discard happens here in the data-IPC PR (empty now). */
        maybe_free_record(pool, rec);
    }
}

/* ---- inflight ref API (tested directly; used by queue/wait PRs) ----------- */

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
    if (rec < &pool->rec[0] || rec > &pool->rec[JE_EP_POOL_CAP - 1u]
        || !rec->allocated) return JE_EP_CORRUPT;
    je_ep_end *e = &rec->end[side];
    if (e->inflight_refs >= JE_EP_REF_MAX) return JE_EP_REFOVERFLOW;
    e->inflight_refs++;
    return JE_EP_OK;
}

je_ep_status je_endpoint_release_inflight(je_endpoint_pool *pool,
                                          je_ep_channel *rec, uint8_t side)
{
    if (!pool || !rec || side > 1u) return JE_EP_CORRUPT;
    if (rec < &pool->rec[0] || rec > &pool->rec[JE_EP_POOL_CAP - 1u]
        || !rec->allocated) return JE_EP_CORRUPT;
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
