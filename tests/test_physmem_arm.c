/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-1a (ADR 0011 D5): hosted test of the allocator's WINDOW-RELATIVE paths
 * at the aarch64 nonzero base. Built with -DJE_ARCH_AARCH64, so
 * JE_PHYSICAL_BASE = 0x40000000, JE_PHYSICAL_LIMIT = 0x50000000 and the page
 * index maps to BASE + i*PAGE_SIZE exactly as on the real ARM target.
 *
 * Why a SEPARATE file from test_physmem.c: that suite static-asserts base 0
 * (the host/x86 window) and exercises the predicate + init semantics. The
 * owner-approved change in this PR is the allocator WINDOW parameterization, and
 * the base-relative arithmetic + the out-of-window wrap rejection in
 * je_pages_free NEVER run under ASan unless the base is nonzero. Both code
 * reviews flagged exactly this gap: a regression in `(a-BASE) >= SPAN` or in
 * `BASE + i*PAGE` would otherwise pass every test. This file closes it under
 * ASan+UBSan.
 *
 * It drives je_pages directly over a fabricated free window (a predicate that
 * frees a small low slice of the window) — no QEMU, no FDT — so every edge is
 * cheap and deterministic.
 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <jeios/alloc.h>

_Static_assert(JE_PHYSICAL_BASE == UINT64_C(0x40000000),
               "this test must be built with -DJE_ARCH_AARCH64");
_Static_assert(JE_PHYSICAL_LIMIT == UINT64_C(0x50000000), "unexpected ARM limit");

static je_pages pages;
static je_pages snap;

static bool same_pages(const je_pages *a, const je_pages *b)
{
    if (a->ready != b->ready || a->free_count != b->free_count) return false;
    for (size_t i = 0; i < JE_PAGE_COUNT; ++i)
        if (a->state[i] != b->state[i]) return false;
    return true;
}

/* Free exactly the first FREE_PAGES pages of the window; reserve the rest. This
 * makes the free region start AT the window base (0x40000000), so an allocation
 * must return BASE for the first page — catching a mapping that forgets to add
 * BASE (it would return 0 or i*PAGE_SIZE). */
#define FREE_PAGES 32u
static bool low_slice_free(const void *ctx, uint64_t phys)
{
    (void)ctx;
    return phys < JE_PHYSICAL_BASE + (uint64_t)FREE_PAGES * JE_PAGE_SIZE;
}

int main(void)
{
    /* Init over the ARM window via the predicate path. */
    assert(je_pages_init_pred(&pages, low_slice_free, NULL) == JE_OK);
    assert(pages.ready);
    assert(pages.free_count == FREE_PAGES);

    /* (1) base-relative mapping: the first allocation returns the window BASE,
     * not 0 and not a window-relative offset. */
    uint64_t a = 0;
    assert(je_pages_alloc(&pages, 1u, &a) == JE_OK);
    assert(a == JE_PHYSICAL_BASE);            /* NOT 0, NOT 0x0 offset */
    assert(je_pages_free(&pages, a, 1u) == JE_OK);
    assert(pages.free_count == FREE_PAGES);

    /* (2) a multi-page run is laid down base-relative and contiguous. */
    uint64_t r = 0;
    assert(je_pages_alloc(&pages, 4u, &r) == JE_OK);
    assert(r == JE_PHYSICAL_BASE);
    assert(je_pages_free(&pages, r, 4u) == JE_OK);
    assert(pages.free_count == FREE_PAGES);

    /* Snapshot the untouched state for the no-mutation checks below. */
    snap = pages;

    /* (3) OUT-OF-WINDOW frees must be rejected BEFORE any index is derived and
     * must not touch state. These are the inputs the `(a-BASE) >= SPAN` unsigned
     * test must catch; several would wrap to a huge index and corrupt state[] if
     * the check were wrong or ordered after the divide. */
    const uint64_t bad[] = {
        0u,                                  /* below base: wraps negative */
        JE_PHYSICAL_BASE - JE_PAGE_SIZE,     /* one page below base */
        JE_PHYSICAL_BASE - 1u,               /* just below base, misaligned */
        JE_PHYSICAL_LIMIT,                   /* at the exclusive top */
        JE_PHYSICAL_LIMIT + JE_PAGE_SIZE,    /* past the top */
        (uint64_t)0xfffffffffffff000u,       /* near 2^64, page-aligned */
        JE_PHYSICAL_BASE + 0x800u,           /* in-window but misaligned */
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        assert(je_pages_free(&pages, bad[i], 1u) == JE_ERR_INVALID);
        assert(same_pages(&pages, &snap));   /* no state change */
    }

    /* (4) a free whose START is in-window but whose RANGE crosses the exclusive
     * window end must be rejected (never index past state[]). Here the last two
     * free pages start at BASE and the test asks to free 2 pages starting at the
     * LAST managed page, which would run one page past the window. */
    uint64_t last = JE_PHYSICAL_LIMIT - JE_PAGE_SIZE;
    assert(je_pages_free(&pages, last, 2u) == JE_ERR_INVALID);
    assert(same_pages(&pages, &snap));

    /* (5) allocate the whole free slice to exhaustion; every returned page is
     * inside [BASE, BASE+FREE_PAGES*P) and the (FREE_PAGES+1)th 1-page request
     * is NOMEM, state unchanged across the failed request. */
    uint64_t got[FREE_PAGES];
    for (unsigned i = 0; i < FREE_PAGES; ++i) {
        assert(je_pages_alloc(&pages, 1u, &got[i]) == JE_OK);
        assert(got[i] >= JE_PHYSICAL_BASE);
        assert(got[i] <  JE_PHYSICAL_BASE + (uint64_t)FREE_PAGES * JE_PAGE_SIZE);
    }
    assert(pages.free_count == 0u);
    je_pages after_exhaust = pages;
    uint64_t none = 123u;
    assert(je_pages_alloc(&pages, 1u, &none) == JE_ERR_NOMEM);
    assert(none == 0u);                       /* failure zeroes *out */
    assert(same_pages(&pages, &after_exhaust)); /* failed alloc frees nothing */

    printf("physmem_arm: PASS (ARM window base=0x%llx: base-relative alloc/free, "
           "out-of-window + window-crossing rejection, exhaustion, NOMEM leaves "
           "state untouched)\n", (unsigned long long)JE_PHYSICAL_BASE);
    return 0;
}
