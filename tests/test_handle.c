/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hosted unit tests for the M3b pure handle table (ADR 0008).
 * Built twice by the Makefile:
 *   - production width (16/16): boundary-vector + semantics coverage;
 *   - reduced width (JE_HANDLE_TEST_WIDTH, tiny generation): exhaustive
 *     saturation / retirement / capacity-loss coverage unreachable at 16 bits.
 * The width is selected by -D flags on the compile line, before this include.
 */
#include <jeios/handle.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CAP 8u

static je_handle_slot g_slots[CAP];

/* A fresh table with capacity CAP and the given quota. */
static je_handle_table mk(uint32_t quota)
{
    je_handle_table t;
    assert(je_handle_table_init(&t, g_slots, CAP, quota));
    return t;
}

static void test_encode_decode(void)
{
    /* round-trip + zero-is-invalid + generation-starts-at-1 */
    je_handle h = je_handle_encode(3u, 1u);
    uint32_t idx, gen;
    je_handle_decode(h, &idx, &gen);
    assert(idx == 3u && gen == 1u);
    assert(h != 0u && je_handle_is_wellformed(h));
    assert(!je_handle_is_wellformed(0u));             /* the null handle */
    /* index 0, generation 1 must be nonzero (the slot-0 first handle) */
    assert(je_handle_encode(0u, 1u) != 0u);
    /* fields do not bleed into each other */
    je_handle_decode(je_handle_encode(JE_HANDLE_INDEX_MASK, JE_HANDLE_GEN_MAX),
                     &idx, &gen);
    assert(idx == JE_HANDLE_INDEX_MASK && gen == JE_HANDLE_GEN_MAX);
}

static void test_alloc_lookup_close(void)
{
    je_handle_table t = mk(CAP);
    int obj_a = 0, obj_b = 0;
    je_handle ha, hb;
    void *out = (void *)0;

    /* deterministic lowest-free-index: first alloc -> index 0, second -> 1 */
    assert(je_handle_alloc(&t, &obj_a, 7u, 0x3u, &ha) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t, &obj_b, 7u, 0x1u, &hb) == JE_HANDLE_OK);
    { uint32_t i, g; je_handle_decode(ha, &i, &g); assert(i == 0u && g == 1u); }
    { uint32_t i, g; je_handle_decode(hb, &i, &g); assert(i == 1u && g == 1u); }

    /* lookup: correct type + sufficient rights -> OK, returns the object */
    assert(je_handle_lookup(&t, ha, 7u, 0x1u, &out) == JE_HANDLE_OK && out == &obj_a);
    assert(je_handle_lookup(&t, ha, 7u, 0x3u, &out) == JE_HANDLE_OK);
    /* wrong type -> WRONGTYPE */
    assert(je_handle_lookup(&t, ha, 9u, 0x1u, &out) == JE_HANDLE_WRONGTYPE);
    /* missing a required right -> DENIED (hb only has 0x1) */
    assert(je_handle_lookup(&t, hb, 7u, 0x2u, &out) == JE_HANDLE_DENIED);

    /* forged / malformed handles */
    assert(je_handle_lookup(&t, 0u, 7u, 0u, &out) == JE_HANDLE_BADINDEX);
    assert(je_handle_lookup(&t, je_handle_encode(CAP, 1u), 7u, 0u, &out)
           == JE_HANDLE_BADINDEX);                 /* index == capacity */
    assert(je_handle_lookup(&t, je_handle_encode(0u, 999u), 7u, 0u, &out)
           == JE_HANDLE_STALE);                    /* wrong generation */

    /* close ha; its handle is now stale; slot 0 goes FREE (gen bumped to 2) */
    assert(je_handle_close(&t, ha) == JE_HANDLE_OK);
    assert(je_handle_lookup(&t, ha, 7u, 0u, &out) == JE_HANDLE_STALE);
    assert(je_handle_close(&t, ha) == JE_HANDLE_STALE);  /* double close */

    /* realloc takes the lowest FREE index (0 again) with the bumped generation */
    je_handle ha2;
    assert(je_handle_alloc(&t, &obj_a, 7u, 0u, &ha2) == JE_HANDLE_OK);
    { uint32_t i, g; je_handle_decode(ha2, &i, &g); assert(i == 0u && g == 2u); }
    assert(ha2 != ha);                               /* old handle != new */
    assert(je_handle_lookup(&t, ha, 7u, 0u, &out) == JE_HANDLE_STALE);
}

static void test_quota(void)
{
    /* quota < capacity: exhausting the quota reports QUOTA, not NOSLOTS. */
    je_handle_table t = mk(2u);
    je_handle h; int obj = 0;
    assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_OK);
    h = 0xdeadu;
    assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_QUOTA);
    assert(h == 0u);                                  /* *out cleared on fail */

    /* quota == capacity: QUOTA is the binding limit (it takes precedence over
     * NOSLOTS when both could apply, ADR 0008 D8). NOSLOTS-via-retirement is
     * only reachable at the reduced width and is covered there. */
    je_handle_table t2 = mk(CAP);
    for (uint32_t i = 0; i < CAP; ++i)
        assert(je_handle_alloc(&t2, &obj, 1u, 0u, &h) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t2, &obj, 1u, 0u, &h) == JE_HANDLE_QUOTA);
}

#ifdef JE_HANDLE_TEST_WIDTH
/* Reduced generation width: close a single slot until it saturates and RETIRES,
 * proving non-wrapping retirement and permanent capacity loss. At the tiny test
 * width GEN_MAX is small, so this terminates quickly. */
static void test_saturation_and_retirement(void)
{
    je_handle_table t = mk(CAP);
    je_handle h; int obj = 0;
    uint32_t closes = 0;
    uint32_t i, g;

    /* Slot 0 starts at generation 1. Each alloc+close bumps it by one until a
     * close at GEN_MAX retires the slot instead of wrapping. */
    for (;;) {
        je_handle_status s = je_handle_alloc(&t, &obj, 1u, 0u, &h);
        assert(s == JE_HANDLE_OK);
        je_handle_decode(h, &i, &g);
        assert(i == 0u);                     /* always the lowest free = 0 */
        assert(g >= 1u && g <= JE_HANDLE_GEN_MAX);
        assert(je_handle_close(&t, h) == JE_HANDLE_OK);
        closes++;
        if (t.slots[0].state == JE_HSLOT_RETIRED) {
            /* capacity shifted off slot 0: the next alloc moves to slot 1 */
            assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_OK);
            je_handle_decode(h, &i, &g);
            assert(i == 1u);
            break;
        }
    }
    /* Retirement happened after exactly GEN_MAX closes of slot 0 (gens 1..MAX). */
    assert(closes == JE_HANDLE_GEN_MAX);
    /* A handle naming the retired slot 0 at any generation is STALE, never OK. */
    void *out;
    assert(je_handle_lookup(&t, je_handle_encode(0u, 1u), 1u, 0u, &out) == JE_HANDLE_STALE);
    assert(je_handle_lookup(&t, je_handle_encode(0u, JE_HANDLE_GEN_MAX), 1u, 0u, &out) == JE_HANDLE_STALE);
}

/* NOSLOTS (distinct from QUOTA) is reachable only when live < quota yet no FREE
 * slot remains — i.e. retirement has consumed the gap. Retire slot 0, fill the
 * rest LIVE, and the next alloc must report NOSLOTS, not QUOTA. */
static void test_noslots_via_retirement(void)
{
    je_handle_table t = mk(CAP);           /* quota == capacity */
    je_handle h; int obj = 0;
    /* Retire slot 0: close it GEN_MAX times. */
    for (uint32_t k = 0; k < JE_HANDLE_GEN_MAX; ++k) {
        assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_OK);
        assert(je_handle_close(&t, h) == JE_HANDLE_OK);
    }
    assert(t.slots[0].state == JE_HSLOT_RETIRED);
    /* Fill the remaining CAP-1 slots. live is now CAP-1 (< quota == CAP). */
    for (uint32_t k = 0; k < CAP - 1u; ++k)
        assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_OK);
    assert(t.live == CAP - 1u);
    /* No FREE slot remains, but live < quota -> NOSLOTS, not QUOTA. */
    assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_NOSLOTS);
}
#else
/* Production width: a boundary-vector sanity pass (16/16). Saturation is not
 * reachable here (65534 closes); the reduced-width build covers it. */
static void test_production_boundaries(void)
{
    assert(JE_HANDLE_INDEX_BITS == 16u && JE_HANDLE_GEN_BITS == 16u);
    assert(JE_HANDLE_GEN_MAX == 0xffffu);
    /* top index and top generation round-trip without collision */
    uint32_t i, g;
    je_handle_decode(je_handle_encode(0xffffu, 0xffffu), &i, &g);
    assert(i == 0xffffu && g == 0xffffu);
    /* a generation-1 handle on slot 0 is nonzero (the zero-invalid guarantee) */
    assert(je_handle_encode(0u, 1u) == 0x00010000u);
}
#endif

static void test_init_rejections(void)
{
    je_handle_table t;
    /* NULL table / NULL slots rejected. */
    assert(!je_handle_table_init((je_handle_table *)0, g_slots, CAP, CAP));
    assert(!je_handle_table_init(&t, (je_handle_slot *)0, CAP, CAP));
    /* capacity 0 rejected; quota > capacity rejected. */
    assert(!je_handle_table_init(&t, g_slots, 0u, 0u));
    assert(!je_handle_table_init(&t, g_slots, CAP, CAP + 1u));
    /* quota 0 is accepted (a disabled table): every alloc fails QUOTA. */
    assert(je_handle_table_init(&t, g_slots, CAP, 0u));
    je_handle h; int obj = 0;
    assert(je_handle_alloc(&t, &obj, 1u, 0u, &h) == JE_HANDLE_QUOTA && h == 0u);
    /* alloc requires a non-NULL out: NULL out allocates nothing. */
    assert(je_handle_table_init(&t, g_slots, CAP, CAP));
    assert(je_handle_alloc(&t, &obj, 1u, 0u, (je_handle *)0) == JE_HANDLE_BADINDEX);
    assert(t.live == 0u);                 /* nothing was consumed */
}

static void test_rights_of(void)
{
    je_handle_table t = mk(CAP);
    int obj = 0;
    je_handle h_ins, h_none;
    uint32_t r;
    /* a handle WITH the inspect right (0x1) and one WITHOUT any rights (0) */
    assert(je_handle_alloc(&t, &obj, 7u, 0x1u, &h_ins) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t, &obj, 7u, 0x0u, &h_none) == JE_HANDLE_OK);

    /* rights_of requiring 0x1: OK on the granted handle, returns the full mask */
    assert(je_handle_rights_of(&t, h_ins, 7u, 0x1u, &r) == JE_HANDLE_OK && r == 0x1u);
    /* requiring 0x1 on a rights-0 handle: DENIED (the branch ring 3 can't reach
     * in M3b until attenuation exists — proven here in the hosted suite). Also
     * confirm *out_rights is left untouched on a failure. */
    r = 0xdeadu;
    assert(je_handle_rights_of(&t, h_none, 7u, 0x1u, &r) == JE_HANDLE_DENIED);
    assert(r == 0xdeadu);                 /* untouched on failure */
    /* required=0 on the rights-0 handle: OK, returns the (empty) mask */
    assert(je_handle_rights_of(&t, h_none, 7u, 0x0u, &r) == JE_HANDLE_OK && r == 0x0u);
    /* wrong type -> WRONGTYPE (also hosted-only in M3b) */
    assert(je_handle_rights_of(&t, h_ins, 9u, 0x1u, &r) == JE_HANDLE_WRONGTYPE);
    /* a well-formed handle naming an out-of-range index -> BADINDEX (distinct
     * from the malformed zero handle). encode(CAP,1) is index==capacity. */
    assert(je_handle_rights_of(&t, je_handle_encode(CAP, 1u), 7u, 0u, &r) == JE_HANDLE_BADINDEX);
    /* stale after close -> STALE; forged (zero) -> BADINDEX */
    assert(je_handle_close(&t, h_ins) == JE_HANDLE_OK);
    assert(je_handle_rights_of(&t, h_ins, 7u, 0x1u, &r) == JE_HANDLE_STALE);
    assert(je_handle_rights_of(&t, 0u, 7u, 0u, &r) == JE_HANDLE_BADINDEX);
}

static void test_dup(void)
{
    const uint32_t SELF = JE_HANDLE_OBJ_SELF;   /* the only duplicable type */
    je_handle_table t = mk(CAP);
    int obj = 0;
    je_handle src, d_full, d_weak, nh;
    uint32_t r;
    void *o;

    /* source with INSPECT (0x1) at slot 0 */
    assert(je_handle_alloc(&t, &obj, SELF, 0x1u, &src) == JE_HANDLE_OK);

    /* dup with the full right -> OK, a DIFFERENT handle at a DIFFERENT slot
     * (fresh-slot), same object, same rights. */
    assert(je_handle_dup_self(&t, src, SELF, 0x1u, &d_full) == JE_HANDLE_OK);
    assert(d_full != src);
    { uint32_t i,g; je_handle_decode(d_full,&i,&g); assert(i == 1u); }  /* lowest free */
    assert(je_handle_rights_of(&t, d_full, SELF, 0x1u, &r) == JE_HANDLE_OK && r == 0x1u);
    assert(je_handle_lookup(&t, d_full, SELF, 0u, &o) == JE_HANDLE_OK && o == &obj);

    /* dup dropping the right -> OK, a rights-0 handle. rights_of requiring INSPECT
     * on it is DENIED — the genuine attenuation->enforcement proof. */
    assert(je_handle_dup_self(&t, src, SELF, 0x0u, &d_weak) == JE_HANDLE_OK);
    assert(je_handle_rights_of(&t, d_weak, SELF, 0x1u, &r) == JE_HANDLE_DENIED);
    assert(je_handle_rights_of(&t, d_weak, SELF, 0x0u, &r) == JE_HANDLE_OK && r == 0x0u);

    /* superset request (a right the source lacks) -> DENIED, table unchanged. */
    uint32_t live_before = t.live;
    assert(je_handle_dup_self(&t, src, SELF, 0x3u, &nh) == JE_HANDLE_DENIED);
    assert(nh == 0u && t.live == live_before);        /* no effect on failure */

    /* a non-SELF type can NEVER be duplicated via this path, even if the named
     * slot's type matches the requested expected_type — the gate is structural,
     * not caller-trusted. (This is the latent-UAF guard for M3c.) */
    je_handle other;
    assert(je_handle_alloc(&t, &obj, 9u /* not SELF */, 0x1u, &other) == JE_HANDLE_OK);
    live_before = t.live;
    assert(je_handle_dup_self(&t, other, 9u, 0x0u, &nh) == JE_HANDLE_WRONGTYPE);
    assert(nh == 0u && t.live == live_before);

    /* cannot dup a stale source; derived handles survive the source's close */
    assert(je_handle_close(&t, src) == JE_HANDLE_OK);
    live_before = t.live;
    assert(je_handle_dup_self(&t, src, SELF, 0x1u, &nh) == JE_HANDLE_STALE);
    assert(nh == 0u && t.live == live_before);        /* no effect on failure */
    assert(je_handle_lookup(&t, d_full, SELF, 0u, &o) == JE_HANDLE_OK);  /* alive */
    assert(je_handle_lookup(&t, d_weak, SELF, 0u, &o) == JE_HANDLE_OK);
    /* each derived handle closes independently */
    assert(je_handle_close(&t, d_weak) == JE_HANDLE_OK);
    assert(je_handle_lookup(&t, d_full, SELF, 0u, &o) == JE_HANDLE_OK);  /* still alive */
    assert(je_handle_close(&t, d_full) == JE_HANDLE_OK);

    /* dup at QUOTA -> QUOTA, *out 0, table unchanged (quota<capacity so it's
     * QUOTA not NOSLOTS). Fill to the quota from one SELF source, then dup. */
    je_handle_table t2;
    je_handle_slot slots2[CAP];
    assert(je_handle_table_init(&t2, slots2, CAP, 2u));   /* quota 2 */
    je_handle a, b;
    assert(je_handle_alloc(&t2, &obj, SELF, 0x1u, &a) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t2, &obj, SELF, 0x1u, &b) == JE_HANDLE_OK);  /* live==quota */
    live_before = t2.live;
    assert(je_handle_dup_self(&t2, a, SELF, 0x1u, &nh) == JE_HANDLE_QUOTA);
    assert(nh == 0u && t2.live == live_before);
    (void) b;
}

/* ADR 0010 D3: je_handle_resolve_any. Written from handle.h only. */

/* Output sentinels: resolve_any must leave every output untouched on failure. */
#define RA_TYPE_SENT   0xA5A5A5A5u
#define RA_RIGHTS_SENT 0x5A5A5A5Au
static int g_ra_sentinel_obj;

/* Call resolve_any with sentinel-filled outputs; on any non-OK status assert the
 * outputs were not written and the table is byte-identical to before. */
static je_handle_status ra(const je_handle_table *t, je_handle h,
                           uint32_t *type, uint32_t *rights, void **obj)
{
    je_handle_slot before[CAP];
    uint32_t live_before = t->live;
    memcpy(before, t->slots, sizeof before);
    *type = RA_TYPE_SENT; *rights = RA_RIGHTS_SENT; *obj = &g_ra_sentinel_obj;
    je_handle_status s = je_handle_resolve_any(t, h, type, rights, obj);
    assert(memcmp(before, t->slots, sizeof before) == 0);   /* pure, read-only */
    assert(t->live == live_before);
    if (s != JE_HANDLE_OK)
        assert(*type == RA_TYPE_SENT && *rights == RA_RIGHTS_SENT
               && *obj == &g_ra_sentinel_obj);
    return s;
}

/* For a status lookup and resolve_any share (BADINDEX/STALE), lookup with the
 * slot's own type and no required rights must agree with resolve_any. */
static void ra_same_as_lookup(const je_handle_table *t, je_handle h, uint32_t type_hint)
{
    uint32_t ty, r; void *o;
    void *lo = &g_ra_sentinel_obj;
    je_handle_status sa = ra(t, h, &ty, &r, &o);
    je_handle_status sl = je_handle_lookup(t, h, type_hint, 0u, &lo);
    assert(sa == sl);
}

static void test_resolve_any(void)
{
    je_handle_table t = mk(CAP);
    int obj_self = 0, obj_ep = 0, obj_proc = 0;
    je_handle h_self, h_ep, h_proc, h_null, h_odd;
    uint32_t ty, r; void *o;

    /* One live slot per object type (SELF 1, ENDPOINT 2, PROCESS 3), plus a NULL
     * object and an arbitrary type: the pure table is type-agnostic. */
    assert(je_handle_alloc(&t, &obj_self, JE_HANDLE_OBJ_SELF, 0x1u, &h_self) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t, &obj_ep, 2u, 0xFu, &h_ep) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t, &obj_proc, 3u, 0x11u, &h_proc) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t, (void *)0, 3u, 0x0u, &h_null) == JE_HANDLE_OK);
    assert(je_handle_alloc(&t, &obj_self, 0x7fu, 0xffffffffu, &h_odd) == JE_HANDLE_OK);

    /* OK: writes the slot's type, FULL rights and object for each type. */
    assert(ra(&t, h_self, &ty, &r, &o) == JE_HANDLE_OK);
    assert(ty == JE_HANDLE_OBJ_SELF && r == 0x1u && o == &obj_self);
    assert(ra(&t, h_ep, &ty, &r, &o) == JE_HANDLE_OK);
    assert(ty == 2u && r == 0xFu && o == &obj_ep);
    assert(ra(&t, h_proc, &ty, &r, &o) == JE_HANDLE_OK);
    assert(ty == 3u && r == 0x11u && o == &obj_proc);
    assert(ra(&t, h_null, &ty, &r, &o) == JE_HANDLE_OK);     /* rights 0 still resolve */
    assert(ty == 3u && r == 0x0u && o == (void *)0);           /* NULL object written */
    assert(ra(&t, h_odd, &ty, &r, &o) == JE_HANDLE_OK);       /* rights not masked */
    assert(ty == 0x7fu && r == 0xffffffffu && o == &obj_self);

    /* No type and no rights check: where lookup says WRONGTYPE or DENIED,
     * resolve_any says OK (that is the point of the any-type resolve). */
    { void *lo;
      assert(je_handle_lookup(&t, h_proc, 2u, 0u, &lo) == JE_HANDLE_WRONGTYPE);
      assert(je_handle_lookup(&t, h_null, 3u, 0x1u, &lo) == JE_HANDLE_DENIED); }
    assert(ra(&t, h_proc, &ty, &r, &o) == JE_HANDLE_OK);
    assert(ra(&t, h_null, &ty, &r, &o) == JE_HANDLE_OK);

    /* BADINDEX: handle 0; index == capacity; index far beyond capacity (the high
     * index bits set; with a 32-bit total width there are no bits above the
     * index+generation fields to set, the wide-RDI case is the decoder's). */
    assert(ra(&t, 0u, &ty, &r, &o) == JE_HANDLE_BADINDEX);
    assert(ra(&t, je_handle_encode(CAP, 1u), &ty, &r, &o) == JE_HANDLE_BADINDEX);
    assert(ra(&t, je_handle_encode(JE_HANDLE_INDEX_MASK, 1u), &ty, &r, &o) == JE_HANDLE_BADINDEX);
    assert(ra(&t, je_handle_encode(1u << (JE_HANDLE_INDEX_BITS - 1u), 1u), &ty, &r, &o)
           == JE_HANDLE_BADINDEX);
    /* index out of range wins over any generation, including gen 0 */
    assert(ra(&t, je_handle_encode(CAP, 0u), &ty, &r, &o) == JE_HANDLE_BADINDEX);
    assert(ra(&t, je_handle_encode(CAP, JE_HANDLE_GEN_MAX), &ty, &r, &o) == JE_HANDLE_BADINDEX);

    /* STALE: a never-allocated FREE slot; wrong generation (+1 and 0) on a LIVE
     * slot; a closed handle. */
    assert(ra(&t, je_handle_encode(CAP - 1u, 1u), &ty, &r, &o) == JE_HANDLE_STALE);
    { uint32_t i, g; je_handle_decode(h_ep, &i, &g);
      if (g < JE_HANDLE_GEN_MAX)
          assert(ra(&t, je_handle_encode(i, g + 1u), &ty, &r, &o) == JE_HANDLE_STALE);
      assert(ra(&t, je_handle_encode(i, 0u), &ty, &r, &o) == JE_HANDLE_STALE); }
    assert(je_handle_close(&t, h_ep) == JE_HANDLE_OK);
    assert(ra(&t, h_ep, &ty, &r, &o) == JE_HANDLE_STALE);
    /* reuse: the old handle stays STALE, the new one resolves to the new binding */
    { je_handle h2;
      assert(je_handle_alloc(&t, &obj_proc, 3u, 0x10u, &h2) == JE_HANDLE_OK);
      assert(h2 != h_ep);
      assert(ra(&t, h_ep, &ty, &r, &o) == JE_HANDLE_STALE);
      assert(ra(&t, h2, &ty, &r, &o) == JE_HANDLE_OK && ty == 3u && r == 0x10u && o == &obj_proc);
      assert(je_handle_close(&t, h2) == JE_HANDLE_OK); }

    /* NULL table / NULL out pointers -> BADINDEX; checked before the handle, so
     * even a valid live handle reports BADINDEX, and nothing is written. */
    { uint32_t ty2 = RA_TYPE_SENT, r2 = RA_RIGHTS_SENT; void *o2 = &g_ra_sentinel_obj;
      assert(je_handle_resolve_any((const je_handle_table *)0, h_self, &ty2, &r2, &o2) == JE_HANDLE_BADINDEX);
      assert(ty2 == RA_TYPE_SENT && r2 == RA_RIGHTS_SENT && o2 == &g_ra_sentinel_obj);
      assert(je_handle_resolve_any(&t, h_self, (uint32_t *)0, &r2, &o2) == JE_HANDLE_BADINDEX);
      assert(r2 == RA_RIGHTS_SENT && o2 == &g_ra_sentinel_obj);
      assert(je_handle_resolve_any(&t, h_self, &ty2, (uint32_t *)0, &o2) == JE_HANDLE_BADINDEX);
      assert(ty2 == RA_TYPE_SENT && o2 == &g_ra_sentinel_obj);
      assert(je_handle_resolve_any(&t, h_self, &ty2, &r2, (void **)0) == JE_HANDLE_BADINDEX);
      assert(ty2 == RA_TYPE_SENT && r2 == RA_RIGHTS_SENT);
      /* NULL out together with a stale handle: still BADINDEX (NULL first) */
      assert(je_handle_resolve_any(&t, h_ep, &ty2, &r2, (void **)0) == JE_HANDLE_BADINDEX); }

    /* Same precedence as lookup on every shared case. */
    ra_same_as_lookup(&t, 0u, 1u);
    ra_same_as_lookup(&t, je_handle_encode(CAP, 1u), 1u);
    ra_same_as_lookup(&t, je_handle_encode(JE_HANDLE_INDEX_MASK, JE_HANDLE_GEN_MAX), 1u);
    ra_same_as_lookup(&t, je_handle_encode(CAP - 1u, 1u), 1u);
    ra_same_as_lookup(&t, h_ep, 2u);
    ra_same_as_lookup(&t, h_self, JE_HANDLE_OBJ_SELF);   /* both OK */
    ra_same_as_lookup(&t, h_proc, 3u);

    /* RETIRED slot: close one slot until its generation saturates, then every
     * generation naming it is STALE, never OK. GEN_MAX closes (65535 at the
     * production width, 3 at the reduced width) are cheap. */
    {
        je_handle_table t3 = mk(CAP);
        je_handle h3 = 0u, last = 0u;
        int obj3 = 0;
        for (uint32_t k = 0; k < JE_HANDLE_GEN_MAX; ++k) {
            assert(je_handle_alloc(&t3, &obj3, 3u, 0x1u, &h3) == JE_HANDLE_OK);
            { uint32_t i, g; je_handle_decode(h3, &i, &g); assert(i == 0u); }
            assert(ra(&t3, h3, &ty, &r, &o) == JE_HANDLE_OK);
            assert(je_handle_close(&t3, h3) == JE_HANDLE_OK);
            last = h3;
        }
        assert(t3.slots[0].state == JE_HSLOT_RETIRED);
        assert(ra(&t3, last, &ty, &r, &o) == JE_HANDLE_STALE);
        assert(ra(&t3, je_handle_encode(0u, 1u), &ty, &r, &o) == JE_HANDLE_STALE);
        assert(ra(&t3, je_handle_encode(0u, JE_HANDLE_GEN_MAX), &ty, &r, &o) == JE_HANDLE_STALE);
        ra_same_as_lookup(&t3, last, 3u);
    }
    (void) h_odd;
}

/* ADR 0010 D5 step 0: je_handle_can_alloc. Written from handle.h only (the
 * implementation was not read). Contract: pure and read-only, and returns exactly
 * the status je_handle_alloc would return on the same table right now. */
static je_handle_slot g_ca_scratch[CAP];
static int g_ca_obj;

/* Assert can_alloc(t) == want, that it changed nothing (slot bytes and every
 * accounting field), that a second call agrees, and that a real alloc on a
 * byte-identical scratch copy of the table returns the same status. */
static void ca_check(const je_handle_table *t, je_handle_status want)
{
    je_handle_slot before[CAP];
    je_handle_slot *slots_before = t->slots;
    uint32_t cap_before = t->capacity, quota_before = t->quota, live_before = t->live;
    assert(t->capacity <= CAP);
    memcpy(before, t->slots, t->capacity * sizeof before[0]);

    je_handle_status s = je_handle_can_alloc(t);
    assert(s == want);
    assert(memcmp(before, t->slots, t->capacity * sizeof before[0]) == 0);
    assert(t->slots == slots_before && t->capacity == cap_before
           && t->quota == quota_before && t->live == live_before);
    assert(je_handle_can_alloc(t) == s);                 /* idempotent */

    /* Oracle: what alloc returns on an identical copy. */
    je_handle_table c = *t;
    memcpy(g_ca_scratch, t->slots, t->capacity * sizeof g_ca_scratch[0]);
    c.slots = g_ca_scratch;
    je_handle h = 0xdeadu;
    je_handle_status a = je_handle_alloc(&c, &g_ca_obj, 1u, 0u, &h);
    assert(a == s);
    assert((a == JE_HANDLE_OK) == (h != 0u));
}

/* Retire the first r non-retired slots of t (lowest-free allocation always picks
 * the first FREE slot, so r * GEN_MAX alloc+close cycles retire slots in order).
 * Requires every slot to be FREE or RETIRED on entry. */
static void ca_retire(je_handle_table *t, uint32_t r)
{
    je_handle h;
    for (uint32_t k = 0; k < r * JE_HANDLE_GEN_MAX; ++k) {
        assert(je_handle_alloc(t, &g_ca_obj, 1u, 0u, &h) == JE_HANDLE_OK);
        assert(je_handle_close(t, h) == JE_HANDLE_OK);
    }
}

static void test_can_alloc(void)
{
    je_handle h, hs[CAP];

    /* NULL table -> BADINDEX. */
    assert(je_handle_can_alloc((const je_handle_table *)0) == JE_HANDLE_BADINDEX);

    /* Empty table -> OK. Then a real alloc agrees and lands. */
    {
        je_handle_table t = mk(CAP);
        ca_check(&t, JE_HANDLE_OK);
        assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &h) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_OK);
    }

    /* Quota 0 (disabled table) -> QUOTA on an empty table. */
    {
        je_handle_table t = mk(0u);
        ca_check(&t, JE_HANDLE_QUOTA);
    }

    /* Quota 3 < capacity: OK below, QUOTA at the quota (FREE slots remain, so
     * this is QUOTA, not NOSLOTS), OK again after one close, QUOTA after refill. */
    {
        je_handle_table t = mk(3u);
        for (uint32_t i = 0; i < 3u; ++i) {
            ca_check(&t, JE_HANDLE_OK);
            assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &hs[i]) == JE_HANDLE_OK);
        }
        ca_check(&t, JE_HANDLE_QUOTA);
        assert(je_handle_close(&t, hs[1]) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_OK);
        assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &hs[1]) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_QUOTA);
    }

    /* Quota == capacity, every slot LIVE: both QUOTA and NOSLOTS apply; QUOTA wins
     * (ADR 0008 D8 precedence, which alloc applies). After a close -> OK. */
    {
        je_handle_table t = mk(CAP);
        for (uint32_t i = 0; i < CAP; ++i)
            assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &hs[i]) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_QUOTA);
        assert(je_handle_close(&t, hs[CAP - 1u]) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_OK);
    }

    /* Every slot RETIRED, live 0 < quota -> NOSLOTS. CAP * GEN_MAX cycles (about
     * 524k at the production width) are cheap enough to run in both builds. */
    {
        je_handle_table t = mk(CAP);
        ca_retire(&t, CAP);
        for (uint32_t i = 0; i < CAP; ++i)
            assert(t.slots[i].state == JE_HSLOT_RETIRED);
        assert(t.live == 0u);
        ca_check(&t, JE_HANDLE_NOSLOTS);
    }

    /* Mixed RETIRED + LIVE, live < quota, no FREE slot -> NOSLOTS. */
    {
        je_handle_table t = mk(CAP);
        ca_retire(&t, 2u);
        ca_check(&t, JE_HANDLE_OK);                       /* FREE slots remain */
        for (uint32_t i = 0; i < CAP - 2u; ++i)
            assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &hs[i]) == JE_HANDLE_OK);
        assert(t.live == CAP - 2u);
        ca_check(&t, JE_HANDLE_NOSLOTS);
        /* a close frees a slot -> OK */
        assert(je_handle_close(&t, hs[0]) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_OK);
    }

    /* Mixed RETIRED + LIVE with live == quota and no FREE slot: QUOTA and NOSLOTS
     * both apply; QUOTA before NOSLOTS. */
    {
        je_handle_table t = mk(CAP - 3u);
        ca_retire(&t, 3u);
        for (uint32_t i = 0; i < CAP - 3u; ++i)
            assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &hs[i]) == JE_HANDLE_OK);
        assert(t.live == t.quota);
        ca_check(&t, JE_HANDLE_QUOTA);
    }

    /* can_alloc followed by alloc on the REAL table: the preflight answer holds
     * (the spawn usage: preflight, then the publishing alloc). */
    {
        je_handle_table t = mk(2u);
        assert(je_handle_can_alloc(&t) == JE_HANDLE_OK);
        assert(je_handle_alloc(&t, &g_ca_obj, 3u, 0x11u, &h) == JE_HANDLE_OK);
        assert(je_handle_can_alloc(&t) == JE_HANDLE_OK);
        assert(je_handle_alloc(&t, &g_ca_obj, 3u, 0x11u, &h) == JE_HANDLE_OK);
        assert(je_handle_can_alloc(&t) == JE_HANDLE_QUOTA);
        h = 0xdeadu;
        assert(je_handle_alloc(&t, &g_ca_obj, 3u, 0x11u, &h) == JE_HANDLE_QUOTA && h == 0u);
    }

    /* Capacity-1 table: OK, then (quota 1) QUOTA, then retired -> NOSLOTS. */
    {
        je_handle_slot one[1];
        je_handle_table t;
        assert(je_handle_table_init(&t, one, 1u, 1u));
        ca_check(&t, JE_HANDLE_OK);
        assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &h) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_QUOTA);
        assert(je_handle_close(&t, h) == JE_HANDLE_OK);
        ca_check(&t, JE_HANDLE_OK);
        while (t.slots[0].state != JE_HSLOT_RETIRED) {
            assert(je_handle_alloc(&t, &g_ca_obj, 1u, 0u, &h) == JE_HANDLE_OK);
            assert(je_handle_close(&t, h) == JE_HANDLE_OK);
        }
        ca_check(&t, JE_HANDLE_NOSLOTS);
    }
}

int main(void)
{
    test_encode_decode();
    test_alloc_lookup_close();
    test_quota();
    test_init_rejections();
    test_rights_of();
    test_dup();
    test_resolve_any();
    test_can_alloc();
#ifdef JE_HANDLE_TEST_WIDTH
    test_saturation_and_retirement();
    test_noslots_via_retirement();
    printf("handle: PASS (encode/decode, alloc/lookup/close, quota, "
           "saturation->retire, retired-capacity loss, noslots-via-retirement, "
           "resolve_any, can_alloc) "
           "[reduced width gen_bits=%u]\n", (unsigned)JE_HANDLE_GEN_BITS);
#else
    test_production_boundaries();
    printf("handle: PASS (encode/decode, alloc/lookup/close, forged/stale/"
           "wrongtype/denied, quota precedence, resolve_any, can_alloc) [production 16/16]\n");
#endif
    return 0;
}
