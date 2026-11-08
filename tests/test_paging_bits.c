/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-1b (ADR 0011 D3/D4/D7): hosted test of the aarch64 stage-1 page-table
 * builder, its pinned register constants, the leaf encoder and the W^X audit.
 *
 * Author separation (AGENTS.md multi-agent model): this file was written by a
 * different agent from paging.c. It deliberately re-derives every expected bit
 * pattern from DDI0487 (VMSAv8-64 stage-1 descriptor + MAIR/TCR/SCTLR layouts)
 * with its OWN macros; it never reuses paging.c's encoders as the oracle. That
 * matters because je_arm_mmu_audit() compares installed leaves against
 * paging.c's own leaf_descriptor(), so an ENCODER bug (e.g. rodata PXN=0, or
 * the UART with AttrIdx 0) passes the audit unchanged — observed by mutation
 * while writing this test. Only an independent oracle catches it.
 *
 * Hosting: built with -DJE_ARCH_AARCH64 (allocator window [0x40000000,
 * 0x50000000), as on the target) and -DJE_ARM_PAGING_HOSTED_TEST (exports the
 * pure leaf encoder; compiles out the AT/PAR asm). The builder dereferences
 * table PHYSICAL addresses as identity pointers (MMU-off contract), so the test
 * maps the whole 256 MiB window at its physical address with
 * MAP_FIXED_NOREPLACE. On x86-64 Linux that range is ordinary low user memory
 * (also under ASan). If the kernel refuses the mapping the test FAILS — it
 * never reports success without having run.
 *
 * Order matters: the single permanent context (je_arm_mmu_context) cannot be
 * torn down, so negative builds run first (they must leave it invalid and the
 * allocator unchanged), then one good build, then audit tampering (leaf, then
 * structural), then enable.
 *
 * Structural tampers (fix-gate F04): the audit's ledger/root/count live in the
 * OPAQUE struct je_arm_mmu (paging.h exposes no layout). The test pokes them
 * through `struct mmu_mirror`, a field-for-field copy of paging.c's struct whose
 * layout is VERIFIED at runtime against known post-build values (every field the
 * test writes, plus the fields after frame_flags[], are checked) before any
 * poke. If paging.c's layout drifts, check_mirror() fails loudly rather than
 * poking the wrong bytes. Foreign table targets are mmap(PROT_NONE) pages, so a
 * stray dereference by the audit faults DETERMINISTICALLY (SIGSEGV), with or
 * without ASan.
 */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <jeios/alloc.h>
#include "paging.h"

_Static_assert(JE_PHYSICAL_BASE == UINT64_C(0x40000000),
               "build with -DJE_ARCH_AARCH64");

/* ---- Independent DDI0487 bit layouts (NOT paging.c's macros). ---- */
#define B(n) (UINT64_C(1) << (n))
#define FIELD(v, lo, w) (((v) >> (lo)) & ((UINT64_C(1) << (w)) - 1u))
/* Stage-1 L3 page descriptor (D8.3): [1:0]=0b11, AttrIndx[4:2], NS[5],
 * AP[7:6], SH[9:8], AF[10], nG[11], OA[47:12], DBM[51], Contig[52], PXN[53],
 * UXN[54], SW[58:55], [63:59] ignored/PBHA. AP[2:1] sit in bits [7:6]:
 * bit 7 = AP[2] (1 = read-only), bit 6 = AP[1] (1 = EL0 accessible). */
#define D_VALID_PAGE UINT64_C(3)
#define D_ATTR(i) ((uint64_t)(i) << 2)
#define D_AP(a) ((uint64_t)(a) << 6)
#define D_SH(s) ((uint64_t)(s) << 8)
#define D_AF B(10)
#define D_NG B(11)
#define D_PXN B(53)
#define D_UXN B(54)
#define D_OA_MASK UINT64_C(0x0000fffffffff000)
#define AP_EL1_RW 0u
#define AP_EL1_RO 2u
#define SH_INNER 3u

/* ADR 0011 D4 permission table, written out literally. */
static uint64_t spec_leaf(uint64_t pa, je_map_class cls)
{
    uint64_t oa = pa & D_OA_MASK;
    switch (cls) {
    case JE_MAP_TEXT:
        return oa | D_VALID_PAGE | D_ATTR(0) | D_AP(AP_EL1_RO) | D_SH(SH_INNER) |
               D_AF | D_UXN;
    case JE_MAP_RODATA:
        return oa | D_VALID_PAGE | D_ATTR(0) | D_AP(AP_EL1_RO) | D_SH(SH_INNER) |
               D_AF | D_PXN | D_UXN;
    case JE_MAP_RW:
        return oa | D_VALID_PAGE | D_ATTR(0) | D_AP(AP_EL1_RW) | D_SH(SH_INNER) |
               D_AF | D_PXN | D_UXN;
    case JE_MAP_DEVICE:
        return oa | D_VALID_PAGE | D_ATTR(1) | D_AP(AP_EL1_RW) | D_SH(0) |
               D_AF | D_PXN | D_UXN;
    default:
        assert(0 && "bad class");
        return 0;
    }
}

uint64_t je_arm_leaf_desc_hosted(uint64_t physical, je_map_class cls);
extern je_arm_mmu je_arm_mmu_context;
extern uint64_t je_arm_mmu_last_audit_leaves;

/* Image-extent symbols paging.c reads (&_text < &_end, in one object). */
__asm__(".data\n.balign 4096\n.globl _text\n_text:\n.zero 4096\n"
        ".globl _end\n_end:\n.zero 16\n.text");
extern uint8_t _text[], _end[];

/* Stub for the asm enable leaf: records its arguments; never touches regs. */
static struct {
    int calls;
    uint64_t a[8];
} leaf_rec;
void je_arm_mmu_enable_leaf(uint64_t root_pa, uint64_t mair, uint64_t tcr,
                            uint64_t r0s, uint64_t r0e, uint64_t r1s,
                            uint64_t r1e, uint64_t sctlr_set);
void je_arm_mmu_enable_leaf(uint64_t root_pa, uint64_t mair, uint64_t tcr,
                            uint64_t r0s, uint64_t r0e, uint64_t r1s,
                            uint64_t r1e, uint64_t sctlr_set)
{
    ++leaf_rec.calls;
    uint64_t v[8] = { root_pa, mair, tcr, r0s, r0e, r1s, r1e, sctlr_set };
    memcpy(leaf_rec.a, v, sizeof(v));
}

/* ---- Layout mirroring the real image (linker.ld, QEMU virt -m 256M). ---- */
#define TEXT_LO   UINT64_C(0x40080000)
#define TEXT_HI   UINT64_C(0x40086000)
#define RODATA_HI UINT64_C(0x40087000)
#define DATA_HI   UINT64_C(0x400c8000)
#define GUARD     DATA_HI
#define STACK_LO  UINT64_C(0x400c9000)
#define IMAGE_END UINT64_C(0x400cd000)
#define UART      UINT64_C(0x09000000)
#define PG        JE_PAGE_SIZE

static const je_map_region good[] = {
    { TEXT_LO, TEXT_HI - TEXT_LO, JE_MAP_TEXT },
    { TEXT_HI, RODATA_HI - TEXT_HI, JE_MAP_RODATA },
    { RODATA_HI, DATA_HI - RODATA_HI, JE_MAP_RW },
    { STACK_LO, IMAGE_END - STACK_LO, JE_MAP_RW },
    { UART, PG, JE_MAP_DEVICE },
};
#define NGOOD (sizeof(good) / sizeof(good[0]))

static je_pages pages;
static unsigned free_slice;
static bool low_slice_free(const void *ctx, uint64_t phys)
{
    (void)ctx;
    return phys < JE_PHYSICAL_BASE + (uint64_t)free_slice * PG;
}
static void pages_init(unsigned n)
{
    free_slice = n;
    assert(je_pages_init_pred(&pages, low_slice_free, NULL) == JE_OK);
    assert(pages.free_count == n);
}

/* Walk from the root; returns a pointer to the L3 slot for va, or NULL if a
 * table level is missing. */
static uint64_t *l3_slot(uint64_t root, uint64_t va)
{
    uint64_t t = root;
    for (unsigned lvl = 0; lvl < 3; ++lvl) {
        uint64_t d = ((uint64_t *)(uintptr_t)t)[FIELD(va, 39u - 9u * lvl, 9)];
        if ((d & 3u) != 3u) return NULL;
        t = d & D_OA_MASK;
    }
    return &((uint64_t *)(uintptr_t)t)[FIELD(va, 12, 9)];
}

static int failures_seen;
#define AUDIT_REJECTS(what) do { \
        assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_ERR_INVALID); \
        ++failures_seen; (void)(what); } while (0)
#define AUDIT_OK() assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_OK)

/* Tamper one descriptor, require the audit to reject it, restore it, require
 * the audit to accept again (proves the tamper — not something else — was the
 * cause). */
static void tamper(uint64_t *slot, uint64_t set, uint64_t clear, const char *what)
{
    uint64_t saved = *slot;
    *slot = (saved & ~clear) | set;
    assert(*slot != saved);
    AUDIT_REJECTS(what);
    *slot = saved;
    AUDIT_OK();
}

static void test_constants(void)
{
    /* MAIR_EL1: Attr0 = 0xff Normal WB RW-alloc, Attr1 = 0x00 Device-nGnRnE
     * (0x04 would be nGnRE — the classic mistake), Attr2..7 = 0x00. */
    assert(JE_ARM_MAIR_EL1 == UINT64_C(0x00000000000000ff));
    assert(FIELD(JE_ARM_MAIR_EL1, 0, 8) == 0xffu);
    assert(FIELD(JE_ARM_MAIR_EL1, 8, 8) == 0x00u);
    assert(FIELD(JE_ARM_MAIR_EL1, 8, 8) != 0x04u);
    for (unsigned i = 2; i < 8; ++i) assert(FIELD(JE_ARM_MAIR_EL1, 8u * i, 8) == 0u);

    /* TCR_EL1, field by field (D19 TCR_EL1). */
    const uint64_t t = JE_ARM_TCR_EL1;
    assert(FIELD(t, 0, 6) == 16u);   /* T0SZ: 48-bit VA */
    assert(FIELD(t, 6, 1) == 0u);    /* RES0 */
    assert(FIELD(t, 7, 1) == 0u);    /* EPD0: TTBR0 walks enabled */
    assert(FIELD(t, 8, 2) == 1u);    /* IRGN0 WBWA */
    assert(FIELD(t, 10, 2) == 1u);   /* ORGN0 WBWA */
    assert(FIELD(t, 12, 2) == 3u);   /* SH0 Inner Shareable */
    assert(FIELD(t, 14, 2) == 0u);   /* TG0 = 4 KiB (0b00) */
    assert(FIELD(t, 16, 6) == 16u);  /* T1SZ */
    assert(FIELD(t, 22, 1) == 0u);   /* A1: ASID from TTBR0 */
    assert(FIELD(t, 23, 1) == 1u);   /* EPD1: TTBR1 walks DISABLED */
    assert(FIELD(t, 30, 2) == 2u);   /* TG1 = 4 KiB is 0b10 (NOT 0b00) */
    assert(FIELD(t, 32, 3) == 4u);   /* IPS = 44-bit PA */
    assert(FIELD(t, 36, 1) == 0u);   /* AS: 8-bit ASID */
    assert(FIELD(t, 37, 1) == 0u);   /* TBI0 */
    assert(FIELD(t, 38, 1) == 0u);   /* TBI1 */
    assert(FIELD(t, 39, 2) == 0u);   /* HA/HD: no HW AF/dirty management */
    assert(t == UINT64_C(0x0000000480903510));  /* composed independently */

    /* SCTLR bits OR'd at enable: exactly M|C|I|WXN; never EE (bit 25). */
    assert(JE_ARM_SCTLR_SET == (B(0) | B(2) | B(12) | B(19)));
    assert(__builtin_popcountll(JE_ARM_SCTLR_SET) == 4);
    assert((JE_ARM_SCTLR_SET & B(25)) == 0u);
    assert((JE_ARM_SCTLR_SET & B(24)) == 0u);   /* E0E */
    assert((UINT64_C(0x30D00808) | JE_ARM_SCTLR_SET) == UINT64_C(0x30d8180d));
}

static void test_encoder(void)
{
    const uint64_t pas[] = { UINT64_C(0x40080000), UART, UINT64_C(0x1000),
                             UINT64_C(0x00000ffffffff000) /* 44-bit max */ };
    const uint64_t known = D_OA_MASK | D_VALID_PAGE | D_ATTR(7) | B(5) | D_AP(3) |
                           D_SH(3) | D_AF | D_NG | D_PXN | D_UXN;
    for (unsigned c = 0; c < JE_MAP_CLASS_COUNT; ++c) {
        for (size_t i = 0; i < sizeof(pas) / sizeof(pas[0]); ++i) {
            const je_map_class cls = (je_map_class)c;
            const uint64_t d = je_arm_leaf_desc_hosted(pas[i], cls);
            assert(d == spec_leaf(pas[i], cls));
            assert((d & ~known) == 0u);                 /* no stray bits */
            assert(FIELD(d, 0, 2) == 3u);               /* valid L3 page */
            assert((d & D_OA_MASK) == pas[i]);          /* OA preserved */
            assert(FIELD(d, 5, 1) == 0u);               /* NS */
            assert(FIELD(d, 6, 1) == 0u);               /* AP[1] (bit 6): no EL0 */
            assert((d & D_AF) != 0u);                   /* AF always 1 */
            assert((d & D_NG) == 0u);                   /* global kernel */
            assert((d & D_UXN) != 0u);                  /* never EL0-exec */
            const bool w = FIELD(d, 7, 1) == 0u;         /* AP[2] (bit 7) = 0: RW */
            const bool x = (d & D_PXN) == 0u;
            assert(!(w && x));                          /* W^X per leaf */
            assert(x == (cls == JE_MAP_TEXT));          /* only text executes */
            assert(w == (cls == JE_MAP_RW || cls == JE_MAP_DEVICE));
            assert(FIELD(d, 2, 3) == (cls == JE_MAP_DEVICE ? 1u : 0u));
            assert(FIELD(d, 8, 2) == (cls == JE_MAP_DEVICE ? 0u : 3u));
            /* Sub-page / >48-bit garbage in the PA never leaks into attrs. */
            assert(je_arm_leaf_desc_hosted(pas[i] | UINT64_C(0xfff) |
                                           UINT64_C(0xffff000000000000), cls) == d);
        }
    }
}

static void test_bad_builds(void)
{
    uint64_t root = 123;
    pages_init(64);
    const size_t before = pages.free_count;
    /* Arguments. */
    assert(je_arm_mmu_build(NULL, &pages, good, NGOOD, &root) == JE_ERR_INVALID);
    assert(je_arm_mmu_build(&je_arm_mmu_context, NULL, good, NGOOD, &root) == JE_ERR_INVALID);
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, NULL, NGOOD, &root) == JE_ERR_INVALID);
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, good, 0, &root) == JE_ERR_INVALID);
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, good, NGOOD, NULL) == JE_ERR_INVALID);
    /* Region shape. */
    const je_map_region bad[][2] = {
        { { TEXT_LO + 8, PG, JE_MAP_TEXT }, { UART, PG, JE_MAP_DEVICE } },  /* misaligned */
        { { TEXT_LO, 0, JE_MAP_TEXT }, { UART, PG, JE_MAP_DEVICE } },       /* empty */
        { { TEXT_LO, PG + 1, JE_MAP_TEXT }, { UART, PG, JE_MAP_DEVICE } },  /* partial page */
        { { TEXT_LO, PG, JE_MAP_CLASS_COUNT }, { UART, PG, JE_MAP_DEVICE } },/* class */
        { { TEXT_LO, 2 * PG, JE_MAP_TEXT }, { TEXT_LO + PG, PG, JE_MAP_RW } }, /* W+X overlap */
        { { (UINT64_C(1) << 48) - PG, 2 * PG, JE_MAP_RW }, { UART, PG, JE_MAP_DEVICE } }, /* > VA */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        root = 123;
        assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, bad[i], 2, &root) == JE_ERR_INVALID);
        assert(root == 0u);
        assert(pages.free_count == before);
    }
    /* Executable region covering the allocator's own frames: the first table
     * frame would become BOTH executable (text) and RW (its table identity
     * leaf) — the install-time ledger must refuse, and roll back every frame. */
    const je_map_region alias[] = {
        { JE_PHYSICAL_BASE, 64 * PG, JE_MAP_TEXT },
        { UART, PG, JE_MAP_DEVICE },
    };
    root = 123;
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, alias, 2, &root) == JE_ERR_INVALID);
    assert(root == 0u && pages.free_count == before);
    assert(je_arm_mmu_root(&je_arm_mmu_context) == 0u);
    /* Allocator exhaustion mid-build: NOMEM, every table frame returned. */
    pages_init(3);
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, good, NGOOD, &root) == JE_ERR_NOMEM);
    assert(root == 0u && pages.free_count == 3u);
    /* No enable/audit on a context that never built. */
    assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_ERR_INVALID);
    assert(je_arm_mmu_enable(&je_arm_mmu_context) == JE_ERR_INVALID);
    assert(leaf_rec.calls == 0);
}

static void test_good_build_and_audit(void)
{
    uint64_t root = 0;
    pages_init(64);
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, good, NGOOD, &root) == JE_OK);
    assert(root == JE_PHYSICAL_BASE);                  /* first-fit, as on QEMU */
    assert(je_arm_mmu_root(&je_arm_mmu_context) == root);
    /* Tables: root, L1, L2(1 GiB @0x40000000), L3(kernel 2 MiB),
     * L2(1 GiB @0), L3(UART 2 MiB) = 6; all inside the kernel L3's 2 MiB. */
    const size_t tables = 6;
    assert(pages.free_count == 64u - tables);
    /* A second build of a live context is refused. */
    uint64_t r2 = 7;
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, good, NGOOD, &r2) == JE_ERR_INVALID);

    /* Every requested page carries exactly the spec descriptor. */
    size_t region_pages = 0;
    for (size_t r = 0; r < NGOOD; ++r) {
        for (uint64_t va = good[r].base; va < good[r].base + good[r].size; va += PG) {
            uint64_t *s = l3_slot(root, va);
            assert(s && *s == spec_leaf(va, good[r].cls));
            ++region_pages;
        }
    }
    assert(region_pages == 77u);
    /* Each table frame is identity-mapped RW+XN, nothing else. */
    for (size_t i = 0; i < tables; ++i) {
        uint64_t *s = l3_slot(root, JE_PHYSICAL_BASE + i * PG);
        assert(s && *s == spec_leaf(JE_PHYSICAL_BASE + i * PG, JE_MAP_RW));
    }
    /* Guard page and the frame after the last table: NOT mapped. */
    assert(*l3_slot(root, GUARD) == 0u);
    assert(*l3_slot(root, JE_PHYSICAL_BASE + tables * PG) == 0u);
    assert(*l3_slot(root, UART + PG) == 0u);
    /* Table descriptors carry no APTable/PXNTable/UXNTable/NSTable bits. */
    const uint64_t *l0 = (const uint64_t *)(uintptr_t)root;
    assert((l0[0] & ~D_OA_MASK) == 3u);
    const uint64_t *l1 = (const uint64_t *)(uintptr_t)(l0[0] & D_OA_MASK);
    assert((l1[0] & ~D_OA_MASK) == 3u && (l1[1] & ~D_OA_MASK) == 3u);

    AUDIT_OK();
    /* Same independent count the QEMU smoke oracle recomputes (0x53). */
    assert(je_arm_mmu_last_audit_leaves == region_pages + tables);
    assert(je_arm_mmu_last_audit_leaves == UINT64_C(0x53));
    /* The audit refuses a region list that disagrees with the tables. */
    const je_map_region fewer[] = { good[0], good[1], good[2], good[3] };
    assert(je_arm_mmu_audit(&je_arm_mmu_context, fewer, 4) == JE_ERR_INVALID);
    AUDIT_OK();

    /* ---- Tampering: each broken invariant must be rejected. ---- */
    uint64_t *text = l3_slot(root, TEXT_LO);
    uint64_t *ro = l3_slot(root, TEXT_HI);
    uint64_t *data = l3_slot(root, RODATA_HI);
    uint64_t *stack = l3_slot(root, STACK_LO);
    uint64_t *uart = l3_slot(root, UART);
    uint64_t *tbl = l3_slot(root, JE_PHYSICAL_BASE);
    tamper(text, 0, D_AP(3), "text writable (W+X)");
    tamper(text, D_PXN, 0, "text not executable (class mismatch)");
    tamper(text, 0, D_UXN, "text EL0-executable");
    tamper(ro, 0, D_PXN, "rodata executable (WXN does not cover it)");
    tamper(ro, 0, D_AP(3), "rodata writable");
    tamper(data, 0, D_PXN, "data executable (W+X)");
    tamper(data, D_AP(1), 0, "data EL0-accessible (AP[1]=1)");
    tamper(stack, 0, D_AF, "stack AF=0");
    tamper(stack, D_NG, 0, "stack nG=1");
    tamper(stack, 0, D_SH(3), "Normal RAM non-shareable");
    tamper(uart, 0, D_ATTR(7), "UART as Normal (AttrIdx 0)");
    tamper(uart, 0, D_PXN, "UART executable");
    tamper(tbl, 0, D_PXN, "page-table frame executable");
    tamper(tbl, D_AP(2), 0, "page-table frame read-only (class mismatch)");
    /* RW data leaf whose OA is a TEXT frame: a writable alias of code. */
    tamper(data, TEXT_LO, D_OA_MASK, "writable alias of a text frame");
    /* Text leaf pointing at a data frame: executable alias of writable RAM. */
    tamper(text, RODATA_HI, D_OA_MASK, "executable alias of a data frame");
    /* Map the guard page (an extra leaf no region asked for). */
    uint64_t *guard = l3_slot(root, GUARD);
    assert(guard && *guard == 0u);
    tamper(guard, spec_leaf(GUARD, JE_MAP_RW), 0, "guard page mapped");
    /* Hierarchical attributes on a table descriptor (L1[1] -> kernel L2). */
    uint64_t *l1_kernel = (uint64_t *)&l1[1];
    tamper(l1_kernel, B(59), 0, "PXNTable");
    tamper(l1_kernel, B(60), 0, "UXNTable");
    tamper(l1_kernel, B(61), 0, "APTable[0]");
    tamper(l1_kernel, B(62), 0, "APTable[1]");
    tamper(l1_kernel, B(63), 0, "NSTable");
    tamper(l1_kernel, 0, B(1), "table entry turned into a 1 GiB block");
    assert(failures_seen == 23);
}

/* ---- Structural tampers (fix-gate F04). ---- */

/* Field-for-field mirror of paging.c's struct je_arm_mmu (layout verified by
 * check_mirror before use). MIRROR_MAX_TABLES mirrors JE_ARM_MAX_TABLES; the
 * COUNT boundary case (128 -> not COUNT, 129 -> COUNT) independently pins it. */
#define MIRROR_MAX_TABLES 128u
#define MIRROR_EXTERNAL_FRAMES 64u
struct mmu_mirror {
    bool valid;
    uint64_t root_pa;
    uint64_t table_pa[MIRROR_MAX_TABLES];
    size_t table_count;
    uint64_t table_start;
    uint64_t table_end;
    uint64_t image_start;
    uint64_t image_end;
    je_pages *pages;
    uint8_t frame_flags[JE_PAGE_COUNT];
    uint64_t external_pa[MIRROR_EXTERNAL_FRAMES];
    uint8_t external_flags[MIRROR_EXTERNAL_FRAMES];
    size_t external_count;
};

static struct mmu_mirror *mirror(void)
{
    return (struct mmu_mirror *)(void *)&je_arm_mmu_context;
}

static void check_mirror(size_t tables)
{
    const struct mmu_mirror *m = mirror();
    assert(m->valid);
    assert(m->root_pa == je_arm_mmu_root(&je_arm_mmu_context));
    assert(m->root_pa == JE_PHYSICAL_BASE);
    assert(m->table_count == tables);
    for (size_t i = 0; i < tables; ++i)
        assert(m->table_pa[i] == JE_PHYSICAL_BASE + i * PG);
    for (size_t i = tables; i < MIRROR_MAX_TABLES; ++i)
        assert(m->table_pa[i] == 0u);
    assert(m->table_start == JE_PHYSICAL_BASE);
    assert(m->table_end == JE_PHYSICAL_BASE + tables * PG);
    assert(m->image_start == (uint64_t)(uintptr_t)_text);
    assert(m->image_end == (uint64_t)(uintptr_t)_end);
    assert(m->pages == &pages);
    /* Fields AFTER the JE_PAGE_COUNT-sized frame_flags[]: only the UART is
     * outside the allocator window. */
    assert(m->external_count == 1u);
    assert(m->external_pa[0] == UART);
}

static int structural_seen;

/* A good audit: OK, reject reason NONE, and the enable stamp is set (so the
 * following reject proves REVOCATION, not "never granted"). */
static void struct_ok(void)
{
    AUDIT_OK();
    assert(je_arm_mmu_last_audit_reject == JE_ARM_AUDIT_REJECT_NONE);
    assert(je_arm_mmu_last_audit_leaves == UINT64_C(0x53));
}

/* (a) audit INVALID, (b) the EXACT expected reason, (c) enable refused and the
 * asm leaf never reached. Returns the descriptor-read count of the failed audit
 * so a caller can bound it. */
static uint64_t struct_rejects(je_arm_mmu_audit_reject want, const char *what)
{
    const int calls = leaf_rec.calls;
    assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_ERR_INVALID);
    if (je_arm_mmu_last_audit_reject != want) {
        fprintf(stderr, "test_paging_bits: FAIL %s: reject reason %d, want %d\n",
                what, (int)je_arm_mmu_last_audit_reject, (int)want);
        assert(0 && "wrong audit reject reason");
    }
    assert(je_arm_mmu_last_audit_leaves == 0u);
    assert(je_arm_mmu_enable(&je_arm_mmu_context) == JE_ERR_INVALID);
    assert(leaf_rec.calls == calls);
    ++structural_seen;
    return je_arm_mmu_last_audit_reads;
}

/* Replace one 64-bit word (descriptor, root_pa or ledger entry) with `value`,
 * require the exact reject, restore, require OK again. */
static uint64_t struct_tamper(uint64_t *slot, uint64_t value,
                              je_arm_mmu_audit_reject want, const char *what)
{
    struct_ok();
    const uint64_t saved = *slot;
    assert(value != saved);
    *slot = value;
    const uint64_t reads = struct_rejects(want, what);
    *slot = saved;
    struct_ok();
    return reads;
}

static void count_tamper(size_t value, je_arm_mmu_audit_reject want,
                         const char *what)
{
    struct_ok();
    size_t *slot = &mirror()->table_count;
    const size_t saved = *slot;
    *slot = value;
    const uint64_t reads = struct_rejects(want, what);
    *slot = saved;
    if (want == JE_ARM_AUDIT_REJECT_COUNT || want == JE_ARM_AUDIT_REJECT_LEDGER)
        assert(reads == 0u);   /* phase 0: rejected before ANY table read */
    struct_ok();
}

static void test_structural_tampers(void)
{
    const size_t tables = 6;
    check_mirror(tables);
    struct mmu_mirror *m = mirror();
    const uint64_t root = m->root_pa;
    uint64_t *l0 = (uint64_t *)(uintptr_t)root;
    const uint64_t l1_pa = l0[0] & D_OA_MASK;
    uint64_t *l1 = (uint64_t *)(uintptr_t)l1_pa;
    const uint64_t l2k_pa = l1[1] & D_OA_MASK;     /* L2 for 1 GiB @0x40000000 */
    uint64_t *l2k = (uint64_t *)(uintptr_t)l2k_pa;
    /* Unused slots chosen below are really unused in the good build. */
    assert(l0[7] == 0u && l0[300] == 0u && l0[511] == 0u);
    assert(l1[5] == 0u && l2k[100] == 0u && l2k[200] == 0u);
    assert(FIELD(TEXT_LO, 21, 9) == 0u && (l2k[0] & 3u) == 3u);

    /* Deterministic-fault foreign frame: any dereference is a SIGSEGV. */
    void *pn = mmap(NULL, (size_t)PG, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pn != MAP_FAILED);
    const uint64_t foreign = (uint64_t)(uintptr_t)pn;
    assert((foreign & ~D_OA_MASK) == 0u);          /* encodable as an OA */
    assert(foreign < JE_PHYSICAL_BASE || foreign >= JE_PHYSICAL_BASE + JE_PHYSICAL_SPAN);

    /* 1. FOREIGN late sibling: root[511] is OFF every descent path. Phase 1
     *    reads exactly the root's 512 entries and rejects before reading ANY
     *    child (the BFS has enqueued but not read L1). */
    uint64_t reads = struct_tamper(&l0[511], foreign | D_VALID_PAGE,
                                   JE_ARM_AUDIT_REJECT_FOREIGN,
                                   "foreign late sibling root[511] (PROT_NONE)");
    assert(reads == 512u);
    /*    Same at a deeper level (L2 sibling of the kernel L3), and with a
     *    readable in-window frame that is simply not in the ledger. */
    struct_tamper(&l2k[200], foreign | D_VALID_PAGE, JE_ARM_AUDIT_REJECT_FOREIGN,
                  "foreign L2 sibling (PROT_NONE)");
    struct_tamper(&l0[511], (JE_PHYSICAL_BASE + 10 * PG) | D_VALID_PAGE,
                  JE_ARM_AUDIT_REJECT_FOREIGN, "in-window non-ledger child");

    /* 2. Root not a ledger member (PROT_NONE): ROOT, before any table read. */
    reads = struct_tamper(&m->root_pa, foreign, JE_ARM_AUDIT_REJECT_ROOT,
                          "root_pa -> PROT_NONE non-member");
    assert(reads == 0u);

    /* 3. Self-cycle: root[300] -> root. */
    struct_tamper(&l0[300], root | D_VALID_PAGE, JE_ARM_AUDIT_REJECT_REVISIT,
                  "self-cycle root[300] -> root");
    /* 4. Ancestor cycles: L2 -> its L1 parent; L1 -> the root. */
    struct_tamper(&l2k[100], l1_pa | D_VALID_PAGE, JE_ARM_AUDIT_REJECT_REVISIT,
                  "ancestor cycle L2[100] -> L1");
    struct_tamper(&l1[5], root | D_VALID_PAGE, JE_ARM_AUDIT_REJECT_REVISIT,
                  "ancestor cycle L1[5] -> root");
    /* 5. Shared child: root[7] -> the same L1 as root[0]. */
    struct_tamper(&l0[7], l1_pa | D_VALID_PAGE, JE_ARM_AUDIT_REJECT_REVISIT,
                  "shared child root[7] == root[0]");

    /* 6. Bad count. The huge value must be rejected before table_pa[] is
     *    indexed (reads == 0, no fault). 128 is the inclusive bound: it passes
     *    the COUNT check and fails the ledger (zero entry) instead. */
    count_tamper(0u, JE_ARM_AUDIT_REJECT_COUNT, "table_count 0");
    count_tamper(MIRROR_MAX_TABLES + 1u, JE_ARM_AUDIT_REJECT_COUNT,
                 "table_count MAX+1");
    count_tamper(SIZE_MAX, JE_ARM_AUDIT_REJECT_COUNT, "table_count SIZE_MAX");
    count_tamper(MIRROR_MAX_TABLES, JE_ARM_AUDIT_REJECT_LEDGER,
                 "table_count == MAX (boundary: ledger, not count)");

    /* 7. Ledger: duplicate, zero, misaligned, beyond the 44-bit PA. */
    reads = struct_tamper(&m->table_pa[5], m->table_pa[1],
                          JE_ARM_AUDIT_REJECT_LEDGER, "duplicate ledger entry");
    assert(reads == 0u);
    struct_tamper(&m->table_pa[3], 0u, JE_ARM_AUDIT_REJECT_LEDGER,
                  "zero ledger entry");
    struct_tamper(&m->table_pa[3], m->table_pa[3] + 8u,
                  JE_ARM_AUDIT_REJECT_LEDGER, "misaligned ledger entry");
    struct_tamper(&m->table_pa[3], UINT64_C(1) << 44,
                  JE_ARM_AUDIT_REJECT_LEDGER, "ledger entry beyond 44-bit PA");

    /* 8. Orphan ledger frames. The window is EXACTLY the 6 contiguous table
     *    frames [BASE, BASE+6*PG), so there is no spare in-window frame:
     *    (a) an orphan just past table_end is OUT of the window -> phase 0
     *        LEDGER (C2), before any table read;
     *    (b) the same orphan with table_end widened to cover it passes phase 0
     *        and no descriptor points at it -> phase 1 UNREACHED;
     *    (c) a detached subtree (L1[0] -> UART L2+L3 cut off) is UNREACHED in
     *        phase 1, before the leaf checks would notice the missing UART. */
    struct_ok();
    assert(m->table_pa[tables] == 0u);
    m->table_pa[tables] = JE_PHYSICAL_BASE + tables * PG;
    m->table_count = tables + 1u;
    reads = struct_rejects(JE_ARM_AUDIT_REJECT_LEDGER,
                           "orphan ledger frame outside [table_start,table_end)");
    assert(reads == 0u);
    const uint64_t saved_end = m->table_end;
    m->table_end = JE_PHYSICAL_BASE + (tables + 1u) * PG;
    struct_rejects(JE_ARM_AUDIT_REJECT_UNREACHED,
                   "in-window orphan ledger frame (widened window)");
    m->table_end = saved_end;
    m->table_count = tables;
    m->table_pa[tables] = 0u;
    struct_ok();
    struct_tamper(&l1[0], 0u, JE_ARM_AUDIT_REJECT_UNREACHED,
                  "detached UART subtree");

    /* 8d. Coupled ledger tamper (C2): move a real ledger entry to an aligned,
     *     unique, below-2^44 frame OUTSIDE the table window WITHOUT changing
     *     any descriptor. The frame is made PROT_NONE so a dereference would
     *     fault; phase 0 must reject LEDGER with zero reads. (Without the
     *     window check the walk would hit the now-unlisted old frame and say
     *     FOREIGN/ROOT instead.) */
    const uint64_t outside = JE_PHYSICAL_BASE + 32u * PG;
    assert(outside >= m->table_end && outside < (UINT64_C(1) << 44));
    for (size_t i = 0; i < tables; ++i) assert(m->table_pa[i] != outside);
    assert(mprotect((void *)(uintptr_t)outside, (size_t)PG, PROT_NONE) == 0);
    reads = struct_tamper(&m->table_pa[5], outside, JE_ARM_AUDIT_REJECT_LEDGER,
                          "ledger entry moved outside the table window");
    assert(reads == 0u);
    reads = struct_tamper(&m->table_pa[0], outside, JE_ARM_AUDIT_REJECT_LEDGER,
                          "root ledger entry moved outside the table window");
    assert(reads == 0u);
    assert(mprotect((void *)(uintptr_t)outside, (size_t)PG,
                    PROT_READ | PROT_WRITE) == 0);

    /* 8e. Cross-leaf W^X alias (reaches audit_alias_scan). Every leaf is
     *     identity-checked, so an alias needs a NON-identity leaf B; phase 2
     *     only rejects B as LEAF when it reaches B. The alias scan for an
     *     EARLIER valid leaf A looks ahead at every L3 entry, so if B sits
     *     after A in walk order the scan fires first -> ALIAS. (A tamper on
     *     the earlier leaf is caught as LEAF — why older alias tampers never
     *     reached the scan.) Both disjuncts of the scan predicate:
     *     (i)  A = text (exec) @TEXT_LO; later RW data leaf B -> TEXT_LO.
     *     (ii) A = table frame leaf (RW) @BASE; later text leaf B -> BASE. */
    uint64_t *a_text = l3_slot(root, TEXT_LO);
    uint64_t *b_data = l3_slot(root, RODATA_HI);
    uint64_t *a_tbl = l3_slot(root, JE_PHYSICAL_BASE);
    assert(a_text && b_data && a_tbl && a_tbl < a_text && a_text < b_data);
    assert(*b_data == spec_leaf(RODATA_HI, JE_MAP_RW));
    assert(*a_text == spec_leaf(TEXT_LO, JE_MAP_TEXT));
    assert(*a_tbl == spec_leaf(JE_PHYSICAL_BASE, JE_MAP_RW));
    struct_tamper(b_data, (*b_data & ~D_OA_MASK) | TEXT_LO,
                  JE_ARM_AUDIT_REJECT_ALIAS,
                  "later RW leaf aliases earlier text frame");
    struct_tamper(a_text, (*a_text & ~D_OA_MASK) | JE_PHYSICAL_BASE,
                  JE_ARM_AUDIT_REJECT_ALIAS,
                  "later RX leaf aliases earlier table frame");
    /*     Control: the same alias with the tampered leaf FIRST in walk order
     *     is caught by the identity check (LEAF), not the scan. */
    struct_tamper(a_text, (*a_text & ~D_OA_MASK) | RODATA_HI,
                  JE_ARM_AUDIT_REJECT_LEAF,
                  "earlier RX leaf aliases later RW frame (identity first)");

    /* 9. Malformed table descriptors. */
    struct_tamper(&l1[1], l1[1] & ~B(1), JE_ARM_AUDIT_REJECT_MALFORMED,
                  "L1 table entry as a block");
    struct_tamper(&l0[0], l0[0] & ~B(1), JE_ARM_AUDIT_REJECT_MALFORMED,
                  "root entry as a block");
    const unsigned hier[] = { 59, 60, 61, 62, 63 };  /* PXN/UXN/AP[2]/NSTable */
    for (size_t i = 0; i < sizeof(hier) / sizeof(hier[0]); ++i)
        struct_tamper(&l1[1], l1[1] | B(hier[i]), JE_ARM_AUDIT_REJECT_MALFORMED,
                      "hierarchical restriction bit on L1 table entry");
    /*    Encoding is checked before membership: a block-encoded foreign entry
     *    is MALFORMED, still never dereferenced. */
    struct_tamper(&l0[511], foreign | UINT64_C(1) /* valid, bit1=0: block */,
                  JE_ARM_AUDIT_REJECT_MALFORMED, "block-encoded foreign root[511]");

    /* A leaf tamper is classified LEAF (phase 2), not as a structural reason. */
    struct_tamper(l3_slot(root, TEXT_LO), *l3_slot(root, TEXT_LO) & ~D_AP(3),
                  JE_ARM_AUDIT_REJECT_LEAF, "text W+X (leaf reason)");

    check_mirror(tables);
    assert(munmap(pn, (size_t)PG) == 0);
    assert(structural_seen == 33);
}

static void test_enable_gating(void)
{
    /* C4: ANY audit call — even a NULL-context precondition failure that
     * names no context — revokes the prior enable grant. */
    AUDIT_OK();
    assert(je_arm_mmu_audit(NULL, good, NGOOD) == JE_ERR_INVALID);
    assert(je_arm_mmu_last_audit_reject == JE_ARM_AUDIT_REJECT_PRECONDITION);
    assert(je_arm_mmu_enable(&je_arm_mmu_context) == JE_ERR_INVALID);
    assert(leaf_rec.calls == 0);

    /* A failed audit revokes the enable permission. */
    uint64_t *text = l3_slot(je_arm_mmu_root(&je_arm_mmu_context), TEXT_LO);
    uint64_t saved = *text;
    *text &= ~D_AP(3);
    assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_ERR_INVALID);
    assert(je_arm_mmu_enable(&je_arm_mmu_context) == JE_ERR_INVALID);
    assert(leaf_rec.calls == 0);
    *text = saved;
    AUDIT_OK();

    assert(je_arm_mmu_enable(&je_arm_mmu_context) == JE_OK);
    assert(leaf_rec.calls == 1);
    assert(leaf_rec.a[0] == JE_PHYSICAL_BASE);                 /* TTBR0 root */
    assert(leaf_rec.a[1] == UINT64_C(0xff));                   /* MAIR */
    assert(leaf_rec.a[2] == UINT64_C(0x0000000480903510));     /* TCR */
    assert(leaf_rec.a[3] == JE_PHYSICAL_BASE);                 /* tables lo */
    assert(leaf_rec.a[4] == JE_PHYSICAL_BASE + 6 * PG);        /* tables hi */
    assert(leaf_rec.a[5] == (uint64_t)(uintptr_t)_text);       /* image lo */
    assert(leaf_rec.a[6] == (uint64_t)(uintptr_t)_end);        /* image hi */
    assert(leaf_rec.a[7] == (B(0) | B(2) | B(12) | B(19)));    /* M|C|I|WXN */
    /* Live context: no second enable, no rebuild, no re-audit. */
    uint64_t r = 9;
    assert(je_arm_mmu_enable(&je_arm_mmu_context) == JE_ERR_INVALID);
    assert(je_arm_mmu_build(&je_arm_mmu_context, &pages, good, NGOOD, &r) == JE_ERR_INVALID);
    assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_ERR_INVALID);
    assert(leaf_rec.calls == 1);
}

int main(void)
{
    void *window = mmap((void *)(uintptr_t)JE_PHYSICAL_BASE,
                        (size_t)JE_PHYSICAL_SPAN, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                        MAP_FIXED_NOREPLACE, -1, 0);
    if (window != (void *)(uintptr_t)JE_PHYSICAL_BASE) {
        fprintf(stderr, "test_paging_bits: FAIL cannot map the ARM window at "
                "0x%llx (needed for identity table pointers)\n",
                (unsigned long long)JE_PHYSICAL_BASE);
        return 1;
    }
    test_constants();
    test_encoder();
    test_bad_builds();
    test_good_build_and_audit();
    test_structural_tampers();
    test_enable_gating();
    printf("test_paging_bits: PASS (constants, %u-class encoder vs DDI0487 "
           "spec, %d audit tamper rejections, %d structural rejections with "
           "exact reason, build rollback, enable gating)\n",
           (unsigned)JE_MAP_CLASS_COUNT, failures_seen, structural_seen);
    return 0;
}
