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

int main(void)
{
    test_encode_decode();
    test_alloc_lookup_close();
    test_quota();
    test_init_rejections();
#ifdef JE_HANDLE_TEST_WIDTH
    test_saturation_and_retirement();
    test_noslots_via_retirement();
    printf("handle: PASS (encode/decode, alloc/lookup/close, quota, "
           "saturation->retire, retired-capacity loss, noslots-via-retirement) "
           "[reduced width gen_bits=%u]\n", (unsigned)JE_HANDLE_GEN_BITS);
#else
    test_production_boundaries();
    printf("handle: PASS (encode/decode, alloc/lookup/close, forged/stale/"
           "wrongtype/denied, quota precedence) [production 16/16]\n");
#endif
    return 0;
}
