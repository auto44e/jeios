/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-1a (ADR 0011 D5): hosted adversarial test of the aarch64 physical
 * allocator adapter (kernel/arch/aarch64/physmem.c, contract physmem.h) and of
 * the window-relative allocator paths it feeds (kernel/core/alloc.c).
 *
 * Authorship: written by a different agent than physmem.c (AGENTS.md multi-agent
 * model). Every assertion is aimed at a plausible wrong implementation; each
 * group names the defect it is meant to catch. The suite was mutation-checked
 * (see docs/HANDOFF.md / the PR): each listed mutant of physmem.c makes it FAIL.
 *
 * Two layers:
 *  (A) je_arm_page_free driven DIRECTLY with hand-built je_boot_ctx/view and
 *      explicit phys values. The predicate is window-agnostic, so this does not
 *      depend on the host window.
 *  (B) je_arm_physmem_init over the HOST window (JE_ARCH_AARCH64 undefined:
 *      base 0, span 1 GiB). Fixtures place RAM low in that window; results are
 *      compared page-by-page with an independent BYTE-granular reference (no
 *      interval arithmetic shared with physmem.c), then exhaustion and the
 *      fragmented-NOMEM no-mutation property are checked. The kernel smoke
 *      cannot prove the latter on its 65k-page live window, so THIS test is the
 *      only place that property is proven.
 */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <jeios/alloc.h>
#include "physmem.h"

#define P JE_PAGE_SIZE
/* Parking spot for kernel/dtb when they are not under test: inside the host
 * window, outside every RAM fixture, above the byte reference region. */
#define PARK UINT64_C(0x30000000)

_Static_assert(JE_PHYSICAL_BASE == 0u, "hosted test expects the host window (base 0)");

static je_pages pages;
static je_pages snap;
static je_pages fresh;

/* Field-wise equality (struct padding after `ready` is not state). */
static bool same_pages(const je_pages *a, const je_pages *b)
{
    return a->ready == b->ready && a->free_count == b->free_count &&
           memcmp(a->state, b->state, sizeof a->state) == 0;
}

/* ------------------------------------------------------------------------- */
/* (A) predicate                                                              */
/* ------------------------------------------------------------------------- */

/* Predicate RAM fixture: [0x100000, 0x140000), 64 pages. */
#define R0 UINT64_C(0x100000)
#define R0SZ (64u * P)

static je_boot_ctx one_ram(void)
{
    je_boot_ctx c;
    memset(&c, 0, sizeof c);
    c.ram_count = 1u;
    c.ram[0] = (je_range){R0, R0SZ};
    return c;
}

static je_arm_physmem_view view_of(const je_boot_ctx *c)
{
    return (je_arm_physmem_view){
        .ctx = c,
        .kernel = {PARK, P},
        .dtb = {PARK + 0x10000u, P},
    };
}

static bool pf(const je_boot_ctx *c, uint64_t phys)
{
    je_arm_physmem_view v = view_of(c);
    return je_arm_page_free(&v, phys);
}

enum kind { K_KERNEL, K_DTB, K_INITRD, K_INITRD_ABSENT, K_RSV, K_RSV_NOMAP };

/* Evaluate the predicate with exactly one region of kind k set to r (others
 * parked outside RAM). Kept rsv go in the LAST of three used slots, so a
 * `i + 1 < rsv_count` loop bound is caught. */
static bool pk(enum kind k, je_range r, uint64_t phys)
{
    je_boot_ctx c = one_ram();
    je_arm_physmem_view v = view_of(&c);
    switch (k) {
    case K_KERNEL: v.kernel = r; break;
    case K_DTB: v.dtb = r; break;
    case K_INITRD: c.has_initrd = true; c.initrd = r; break;
    case K_INITRD_ABSENT: c.has_initrd = false; c.initrd = r; break;
    case K_RSV:
    case K_RSV_NOMAP:
        c.rsv_count = 3u;
        c.rsv[0] = (je_range){PARK + 0x20000u, P};
        c.rsv[1] = (je_range){PARK + 0x30000u, P};
        c.rsv[2] = r;
        c.rsv_nomap[2] = (k == K_RSV_NOMAP) ? 1u : 0u;
        break;
    }
    return je_arm_page_free(&v, phys);
}

/* Half-open edge discipline for one reserved-region kind. `res` = whether the
 * region is expected to reserve (false only for an initrd with has_initrd=0). */
static void edges(enum kind k, bool res)
{
    /* Aligned 2-page region [0x110000, 0x112000). */
    je_range a = {0x110000u, 2u * P};
    assert(pk(k, a, 0x10f000u));            /* page ends exactly at base: free */
    assert(pk(k, a, 0x110000u) == !res);    /* fully inside */
    assert(pk(k, a, 0x111000u) == !res);    /* last page inside */
    assert(pk(k, a, 0x112000u));            /* starts at exclusive end: free */

    /* Unaligned [0x110fff, 0x112001): touches ONLY the last byte of page
     * 0x110000 and ONLY the first byte of page 0x112000. */
    je_range u = {0x110fffu, 0x1002u};
    assert(pk(k, u, 0x10f000u));
    assert(pk(k, u, 0x110000u) == !res);    /* last byte only */
    assert(pk(k, u, 0x111000u) == !res);
    assert(pk(k, u, 0x112000u) == !res);    /* first byte only */
    assert(pk(k, u, 0x113000u));

    /* One-byte regions: last byte of a page, first byte of a page. */
    je_range last = {0x120fffu, 1u};
    assert(pk(k, last, 0x11f000u));
    assert(pk(k, last, 0x120000u) == !res);
    assert(pk(k, last, 0x121000u));
    je_range first = {0x121000u, 1u};
    assert(pk(k, first, 0x120000u));
    assert(pk(k, first, 0x121000u) == !res);
    assert(pk(k, first, 0x122000u));

    /* Region that starts BEFORE the page and ends AFTER it (catches an
     * intersection test that only asks "does the region base fall in the
     * page"). */
    je_range wide = {0x12f800u, 0x2000u};    /* [0x12f800, 0x131800) */
    assert(pk(k, wide, 0x12e000u));
    assert(pk(k, wide, 0x12f000u) == !res);
    assert(pk(k, wide, 0x130000u) == !res); /* covered, region base not in it */
    assert(pk(k, wide, 0x131000u) == !res);
    assert(pk(k, wide, 0x132000u));
}

static void test_predicate(void)
{
    /* Invalid inputs fail closed. */
    assert(!je_arm_page_free(NULL, R0));
    je_arm_physmem_view nv = {.ctx = NULL, .kernel = {PARK, P}, .dtb = {PARK, P}};
    assert(!je_arm_page_free(&nv, R0));

    je_boot_ctx c = one_ram();
    /* Fully inside the single RAM range, touching nothing: free. */
    assert(pf(&c, R0));
    assert(pf(&c, R0 + 31u * P));
    assert(pf(&c, R0 + R0SZ - P));          /* last page of RAM */
    /* Entirely outside all RAM, nothing reserved there: NOT free (catches
     * dropping the membership half of the predicate). */
    assert(!pf(&c, 0u));
    assert(!pf(&c, R0 - P));                /* page just below RAM */
    assert(!pf(&c, R0 + R0SZ));             /* page at RAM exclusive end */
    assert(!pf(&c, 0x200000u));
    assert(!pf(&c, 0x3ffff000u));

    /* No RAM at all: nothing is free. */
    je_boot_ctx empty = c;
    empty.ram_count = 0u;
    assert(!pf(&empty, R0));

    /* Straddling a RAM boundary (partly in, partly out): NOT free. */
    je_boot_ctx s = c;
    s.ram[0] = (je_range){R0 + 0x800u, R0SZ - 0x1000u}; /* [0x100800,0x13f800) */
    assert(!pf(&s, R0));                    /* first 0x800 bytes outside */
    assert(pf(&s, R0 + P));
    assert(pf(&s, R0 + R0SZ - 2u * P));
    assert(!pf(&s, R0 + R0SZ - P));         /* last 0x800 bytes outside */
    /* Off-by-one at a page-aligned end: RAM ends one byte short. */
    je_boot_ctx s1 = c;
    s1.ram[0].size = R0SZ - 1u;
    assert(!pf(&s1, R0 + R0SZ - P));
    assert(pf(&s1, R0 + R0SZ - 2u * P));

    /* Three RAM ranges with gaps; probe pages in the LAST range (catches a
     * short RAM loop bound). */
    je_boot_ctx m;
    memset(&m, 0, sizeof m);
    m.ram_count = 3u;
    m.ram[0] = (je_range){0x100000u, 0x40000u};
    m.ram[1] = (je_range){0x200000u, 0x10000u};
    m.ram[2] = (je_range){0x300000u, 0x8000u};
    assert(pf(&m, 0x100000u) && pf(&m, 0x13f000u));
    assert(!pf(&m, 0x140000u) && !pf(&m, 0x180000u) && !pf(&m, 0x1ff000u));
    assert(pf(&m, 0x200000u) && pf(&m, 0x20f000u));
    assert(!pf(&m, 0x210000u) && !pf(&m, 0x2ff000u));
    assert(pf(&m, 0x300000u) && pf(&m, 0x307000u));
    assert(!pf(&m, 0x308000u));

    /* Two touching RAM ranges split mid-page: the page is covered by the UNION
     * but by no single range -> NOT free (contract: "inside exactly one"). */
    je_boot_ctx adj;
    memset(&adj, 0, sizeof adj);
    adj.ram_count = 2u;
    adj.ram[0] = (je_range){0x100000u, 0x20800u};  /* [0x100000,0x120800) */
    adj.ram[1] = (je_range){0x120800u, 0x1f800u};  /* [0x120800,0x140000) */
    assert(pf(&adj, 0x11f000u));
    assert(!pf(&adj, 0x120000u));
    assert(pf(&adj, 0x121000u));
    /* Duplicate RAM range: inside TWO ranges, not exactly one -> NOT free
     * (pins the frozen header wording; a validated ctx never has this). */
    je_boot_ctx dup = c;
    dup.ram_count = 2u;
    dup.ram[1] = dup.ram[0];
    assert(!pf(&dup, R0));

    /* Counts beyond the fixed capacity: fail closed even if ram[0] matches. */
    je_boot_ctx big = c;
    big.ram_count = JE_FDT_MAX_RAM + 1u;
    assert(!pf(&big, R0));
    big = c;
    big.rsv_count = JE_FDT_MAX_RSV + 1u;
    assert(!pf(&big, R0));
    /* Exactly at capacity is accepted; the page lives in the LAST slot, and a
     * reservation in the LAST rsv slot is honored. */
    je_boot_ctx cap;
    memset(&cap, 0, sizeof cap);
    cap.ram_count = JE_FDT_MAX_RAM;
    for (uint32_t i = 0u; i < JE_FDT_MAX_RAM; ++i)
        cap.ram[i] = (je_range){0x100000u * (i + 1u), 0x10000u};
    uint64_t lastram = 0x100000u * JE_FDT_MAX_RAM;
    assert(pf(&cap, lastram) && pf(&cap, lastram + P));
    cap.rsv_count = JE_FDT_MAX_RSV;
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; ++i)
        cap.rsv[i] = (je_range){PARK + 0x40000u + i * P, P};
    cap.rsv[JE_FDT_MAX_RSV - 1u] = (je_range){lastram, P};
    assert(!pf(&cap, lastram) && pf(&cap, lastram + P));

    /* Half-open edges for every reserved-region kind. */
    edges(K_KERNEL, true);
    edges(K_DTB, true);
    edges(K_INITRD, true);
    edges(K_INITRD_ABSENT, false);   /* has_initrd=false: initrd is ignored */
    edges(K_RSV, true);
    edges(K_RSV_NOMAP, true);        /* no-map is still reserved */

    /* A reservation entirely OUTSIDE RAM must not make anything free. */
    je_boot_ctx o = c;
    o.rsv_count = 1u;
    o.rsv[0] = (je_range){0x200000u, P};
    assert(!pf(&o, 0x200000u) && pf(&o, R0));
}

/* ------------------------------------------------------------------------- */
/* (B) init over the host window, byte-granular reference                     */
/* ------------------------------------------------------------------------- */

/* The reference covers bytes [0, REF_LIMIT); every fixture's RAM lies inside
 * it, so every page at or above REF_LIMIT must be reserved. */
#define REF_LIMIT UINT64_C(0x80000)
#define REF_PAGES ((size_t)(REF_LIMIT / P))
static unsigned char ref_ram[REF_LIMIT];
static unsigned char ref_rsv[REF_LIMIT];
static bool ref_free[REF_PAGES];

static void ref_mark(unsigned char *map, je_range r)
{
    for (uint64_t b = r.base; b < r.base + r.size; ++b)
        if (b < REF_LIMIT) map[b] = 1u;
}

/* Independent reference: a page is free iff EVERY byte is RAM and NO byte is
 * reserved. Returns the number of free pages. Shares no interval arithmetic
 * with physmem.c (fixtures never use touching/duplicate RAM ranges, where
 * byte-union and "exactly one range" would differ). */
static size_t ref_build(const je_arm_physmem_view *v)
{
    memset(ref_ram, 0, sizeof ref_ram);
    memset(ref_rsv, 0, sizeof ref_rsv);
    for (uint32_t i = 0u; i < v->ctx->ram_count; ++i) ref_mark(ref_ram, v->ctx->ram[i]);
    ref_mark(ref_rsv, v->kernel);
    ref_mark(ref_rsv, v->dtb);
    if (v->ctx->has_initrd) ref_mark(ref_rsv, v->ctx->initrd);
    for (uint32_t i = 0u; i < v->ctx->rsv_count; ++i) ref_mark(ref_rsv, v->ctx->rsv[i]);
    size_t n = 0u;
    for (size_t p = 0u; p < REF_PAGES; ++p) {
        bool f = true;
        for (uint64_t b = p * P; b < (p + 1u) * P; ++b)
            if (!ref_ram[b] || ref_rsv[b]) { f = false; break; }
        ref_free[p] = f;
        if (f) ++n;
    }
    return n;
}

/* Init `pages` from v and compare EVERY state[] entry to the reference (not
 * just free_count, so compensating errors cannot hide). Returns free_count. */
static size_t init_and_check(const je_arm_physmem_view *v)
{
    size_t want = ref_build(v);
    assert(je_arm_physmem_init(&pages, v) == JE_OK);
    assert(pages.ready);
    for (size_t i = 0u; i < JE_PAGE_COUNT; ++i) {
        bool f = i < REF_PAGES && ref_free[i];
        assert(pages.state[i] == (f ? 0u : UINT16_MAX));
    }
    assert(pages.free_count == want);
    return pages.free_count;
}

#define IR0 UINT64_C(0x1000)
#define IR0N 64u
#define IR1 UINT64_C(0x60000)
#define IR1N 8u

static je_boot_ctx init_ctx(void)
{
    je_boot_ctx c;
    memset(&c, 0, sizeof c);
    c.ram_count = 1u;
    c.ram[0] = (je_range){IR0, IR0N * P};     /* [0x1000, 0x41000) */
    return c;
}

static void test_init_counts(void)
{
    /* Invalid arguments. */
    je_boot_ctx c = init_ctx();
    je_arm_physmem_view v = view_of(&c);
    assert(je_arm_physmem_init(NULL, &v) == JE_ERR_INVALID);
    assert(je_arm_physmem_init(&pages, NULL) == JE_ERR_INVALID);
    je_arm_physmem_view nv = v;
    nv.ctx = NULL;
    assert(je_arm_physmem_init(&pages, &nv) == JE_ERR_INVALID);

    /* Baseline: kernel/dtb parked outside RAM; exactly the RAM pages are free
     * (the other 262080 window pages are not RAM and must not be counted). */
    size_t base = init_and_check(&v);
    assert(base == IR0N);

    /* Each reservation inside RAM lowers free_count by exactly its page span. */
    je_boot_ctx k = c; je_arm_physmem_view vk = view_of(&k);
    vk.kernel = (je_range){0x3000u, 2u * P};               /* 2 pages */
    assert(init_and_check(&vk) == base - 2u);
    je_boot_ctx d = c; je_arm_physmem_view vd = view_of(&d);
    vd.dtb = (je_range){0x8800u, P};                        /* straddles 2 pages */
    assert(init_and_check(&vd) == base - 2u);
    je_boot_ctx ini = c; ini.has_initrd = true;
    ini.initrd = (je_range){0x10000u, 1u};                  /* 1 byte -> 1 page */
    je_arm_physmem_view vi = view_of(&ini);
    assert(init_and_check(&vi) == base - 1u);
    ini.has_initrd = false;                                 /* absent: no effect */
    assert(init_and_check(&vi) == base);
    je_boot_ctx r = c; r.rsv_count = 1u;
    r.rsv[0] = (je_range){0x20000u, 4u * P}; r.rsv_nomap[0] = 1u;
    je_arm_physmem_view vr = view_of(&r);
    assert(init_and_check(&vr) == base - 4u);

    /* Second RAM range across a gap adds exactly its pages. */
    je_boot_ctx two = c; two.ram_count = 2u;
    two.ram[1] = (je_range){IR1, IR1N * P};
    je_arm_physmem_view v2 = view_of(&two);
    assert(init_and_check(&v2) == base + IR1N);
}

/* Combined fixture: two RAM ranges, every reservation kind inside RAM, one
 * overlapping another (no double count), one rsv in the second range. */
static je_boot_ctx combo_ctx;
static je_arm_physmem_view combo(void)
{
    je_boot_ctx *c = &combo_ctx;
    *c = init_ctx();
    c->ram_count = 2u;
    c->ram[1] = (je_range){IR1, IR1N * P};
    c->has_initrd = true;
    c->initrd = (je_range){0x10000u, 1u};
    c->rsv_count = 3u;
    c->rsv[0] = (je_range){0x20000u, 4u * P}; c->rsv_nomap[0] = 1u;
    c->rsv[1] = (je_range){0x3800u, 0x1000u};  /* overlaps kernel page 0x3000 + 0x4000 */
    c->rsv[2] = (je_range){IR1 + 3u * P, P};
    je_arm_physmem_view v = view_of(c);
    v.kernel = (je_range){0x3000u, 2u * P};
    v.dtb = (je_range){0x8800u, P};
    return v;
}
/* 64 + 8 RAM pages - kernel 2 - dtb 2 - initrd 1 - rsv0 4 - rsv1 0 (inside
 * kernel) - rsv2 1. */
#define COMBO_FREE (IR0N + IR1N - 2u - 2u - 1u - 4u - 0u - 1u)

static void test_exhaustion(void)
{
    je_arm_physmem_view v = combo();
    assert(init_and_check(&v) == COMBO_FREE);
    memcpy(&fresh, &pages, sizeof pages);

    /* Re-init over a dirty object yields the identical result (init must
     * overwrite every entry, not only the free ones). */
    memset(&pages, 0x5a, sizeof pages);
    assert(je_arm_physmem_init(&pages, &v) == JE_OK);
    assert(same_pages(&pages, &fresh));

    /* Allocate to exhaustion: every page is reference-free, none repeats, and
     * EVERY reference-free page is handed out. */
    static bool seen[REF_PAGES];
    memset(seen, 0, sizeof seen);
    size_t got = 0u;
    uint64_t pa;
    for (;;) {
        je_status s = je_pages_alloc(&pages, 1u, &pa);
        if (s == JE_ERR_NOMEM) { assert(pa == 0u); break; }
        assert(s == JE_OK);
        assert((pa & (P - 1u)) == 0u && pa < REF_LIMIT);
        size_t i = (size_t)(pa / P);
        assert(ref_free[i] && !seen[i]);
        seen[i] = true;
        ++got;
        assert(got <= COMBO_FREE);
    }
    assert(got == COMBO_FREE && pages.free_count == 0u);
    for (size_t i = 0u; i < REF_PAGES; ++i) assert(seen[i] == ref_free[i]);
    /* Freeing a reserved page / out-of-window address is refused. */
    assert(je_pages_free(&pages, 0x3000u, 1u) == JE_ERR_INVALID);
    assert(je_pages_free(&pages, JE_PHYSICAL_LIMIT, 1u) == JE_ERR_INVALID);
    for (size_t i = 0u; i < REF_PAGES; ++i)
        if (seen[i]) assert(je_pages_free(&pages, (uint64_t)i * P, 1u) == JE_OK);
    assert(same_pages(&pages, &fresh));
}

/* Fragmented window: RAM [0x1000, +30 pages), every third page reserved
 * (relative pages 2,5,...,29) -> ten runs of exactly 2 free pages, 20 free. */
static void test_fragmented_rollback(void)
{
    je_boot_ctx c = init_ctx();
    c.ram[0].size = 30u * P;
    c.rsv_count = 10u;
    for (uint32_t k = 0u; k < 10u; ++k)
        c.rsv[k] = (je_range){IR0 + (3u * k + 2u) * P, P};
    je_arm_physmem_view v = view_of(&c);
    assert(init_and_check(&v) == 20u);
    memcpy(&fresh, &pages, sizeof pages);

    /* free_count (20) >= 3 but no run of 3: NOMEM, *out zeroed, and state[]
     * plus free_count byte-identical (no partial commit leaked). */
    uint64_t pa = 0xdeadbeefu;
    memcpy(&snap, &pages, sizeof pages);
    assert(je_pages_alloc(&pages, 3u, &pa) == JE_ERR_NOMEM && pa == 0u);
    assert(same_pages(&pages, &snap));
    assert(je_pages_alloc(&pages, JE_ALLOC_MAX_PAGES, &pa) == JE_ERR_NOMEM);
    assert(same_pages(&pages, &snap));
    assert(je_pages_alloc(&pages, JE_ALLOC_MAX_PAGES + 1u, &pa) == JE_ERR_INVALID);
    assert(same_pages(&pages, &snap));

    /* A run of 2 does exist: it must be two reference-free adjacent pages. */
    assert(je_pages_alloc(&pages, 2u, &pa) == JE_OK);
    assert(ref_free[pa / P] && ref_free[pa / P + 1u] && pages.free_count == 18u);
    assert(je_pages_free(&pages, pa, 2u) == JE_OK);
    assert(same_pages(&pages, &fresh));

    /* Break every run into singletons: allocate all 20 pages, then free the
     * second page of each run. 10 free remain, each isolated between an
     * allocated head and a reserved page; a 2-page request must NOMEM without
     * mutation even though free_count (10) >= 2. */
    uint64_t all[20];
    for (uint32_t k = 0u; k < 20u; ++k) {
        assert(je_pages_alloc(&pages, 1u, &all[k]) == JE_OK);
        assert(ref_free[all[k] / P]);
    }
    assert(pages.free_count == 0u);
    for (uint32_t k = 0u; k < 10u; ++k)
        assert(je_pages_free(&pages, IR0 + (3u * k + 1u) * P, 1u) == JE_OK);
    assert(pages.free_count == 10u);
    memcpy(&snap, &pages, sizeof pages);
    pa = 0xdeadbeefu;
    assert(je_pages_alloc(&pages, 2u, &pa) == JE_ERR_NOMEM && pa == 0u);
    assert(same_pages(&pages, &snap));
    for (uint32_t k = 0u; k < 10u; ++k)
        assert(je_pages_free(&pages, IR0 + 3u * k * P, 1u) == JE_OK);
    assert(same_pages(&pages, &fresh));
}

int main(void)
{
    test_predicate();
    test_init_counts();
    test_exhaustion();
    test_fragmented_rollback();
    puts("physmem: PASS (default-deny predicate: RAM membership, straddle, gaps, "
         "exactly-one-range, capacity; half-open edges for kernel/dtb/initrd/rsv/no-map; "
         "init state[] vs byte-level reference; exhaustion; fragmented NOMEM leaves "
         "state byte-identical)");
    return 0;
}
