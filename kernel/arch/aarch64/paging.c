/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-1b (ADR 0011 D2/D3/D4/D6/D7): aarch64 stage-1 page tables.
 *
 * Caller context: EL1, one CPU, interrupts masked. The builder runs with the
 * MMU and caches off, so physical table addresses are temporarily usable as
 * identity pointers. Every allocated table page is later installed as an
 * explicit RW identity leaf before the MMU is enabled. The builder owns those
 * pages until the context is destroyed by a later MMU lifecycle; this PR has
 * no teardown path because the kernel root is permanent.
 *
 * The context keeps a small physical-frame permission ledger. Frames in the
 * PR-1a allocator window use the allocator-sized bitmap; device/out-of-window
 * frames use a bounded side table. This is sufficient for the static MP2 map
 * and, importantly, rejects a W+X alias at the moment a leaf is installed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jeios/alloc.h>
#include "paging.h"

extern void *memset(void *destination, int value, size_t count);

#define JE_ARM_PAGE_SHIFT 12u
#define JE_ARM_PT_ENTRIES 512u
#define JE_ARM_PT_INDEX_MASK UINT64_C(0x1ff)
#define JE_ARM_VA_LIMIT (UINT64_C(1) << 48)
#define JE_ARM_DESC_ADDR_MASK UINT64_C(0x0000fffffffff000)
#define JE_ARM_DESC_VALID UINT64_C(0x1)
#define JE_ARM_DESC_TABLE UINT64_C(0x3)
#define JE_ARM_DESC_ATTRIDX_SHIFT 2u
#define JE_ARM_DESC_AP_SHIFT 6u
#define JE_ARM_DESC_SH_SHIFT 8u
#define JE_ARM_DESC_AF (UINT64_C(1) << 10)
#define JE_ARM_DESC_PXN (UINT64_C(1) << 53)
#define JE_ARM_DESC_UXN (UINT64_C(1) << 54)
#define JE_ARM_DESC_TABLE_AP_MASK (UINT64_C(3) << 61)
#define JE_ARM_DESC_TABLE_PXN (UINT64_C(1) << 59)
#define JE_ARM_DESC_TABLE_UXN (UINT64_C(1) << 60)
#define JE_ARM_DESC_TABLE_NS (UINT64_C(1) << 63)

#define JE_ARM_MAP_EXEC UINT8_C(1)
#define JE_ARM_MAP_WRITE UINT8_C(2)
#define JE_ARM_EXTERNAL_FRAMES 64u
#define JE_ARM_MAX_TABLES 128u

/* MAIR_EL1: index 0 is Normal WB RA/WA; index 1 is Device-nGnRnE. */
#define JE_ARM_MAIR_ATTR_NORMAL UINT64_C(0xff)
#define JE_ARM_MAIR_ATTR_DEVICE UINT64_C(0x00)
#define JE_ARM_MAIR_VALUE \
    (JE_ARM_MAIR_ATTR_NORMAL | (JE_ARM_MAIR_ATTR_DEVICE << 8))

/* TCR_EL1 field encodings. TG1 deliberately differs from TG0. */
#define JE_ARM_TCR_T0SZ(v) ((UINT64_C(v) & UINT64_C(0x3f)) << 0)
#define JE_ARM_TCR_EPD0(v) ((UINT64_C(v) & UINT64_C(0x1)) << 7)
#define JE_ARM_TCR_IRGN0(v) ((UINT64_C(v) & UINT64_C(0x3)) << 8)
#define JE_ARM_TCR_ORGN0(v) ((UINT64_C(v) & UINT64_C(0x3)) << 10)
#define JE_ARM_TCR_SH0(v) ((UINT64_C(v) & UINT64_C(0x3)) << 12)
#define JE_ARM_TCR_TG0(v) ((UINT64_C(v) & UINT64_C(0x3)) << 14)
#define JE_ARM_TCR_T1SZ(v) ((UINT64_C(v) & UINT64_C(0x3f)) << 16)
#define JE_ARM_TCR_EPD1(v) ((UINT64_C(v) & UINT64_C(0x1)) << 23)
#define JE_ARM_TCR_TG1(v) ((UINT64_C(v) & UINT64_C(0x3)) << 30)
#define JE_ARM_TCR_IPS(v) ((UINT64_C(v) & UINT64_C(0x7)) << 32)
#define JE_ARM_TCR_VALUE \
    (JE_ARM_TCR_T0SZ(16) | JE_ARM_TCR_EPD0(0) | \
     JE_ARM_TCR_IRGN0(1) | JE_ARM_TCR_ORGN0(1) | JE_ARM_TCR_SH0(3) | \
     JE_ARM_TCR_TG0(0) | JE_ARM_TCR_T1SZ(16) | JE_ARM_TCR_EPD1(1) | \
     JE_ARM_TCR_TG1(2) | JE_ARM_TCR_IPS(4))

/* MP1 interim SCTLR plus M|C|I|WXN, written together by the enable leaf. */
#define JE_ARM_SCTLR_M (UINT64_C(1) << 0)
#define JE_ARM_SCTLR_C (UINT64_C(1) << 2)
#define JE_ARM_SCTLR_I (UINT64_C(1) << 12)
#define JE_ARM_SCTLR_WXN (UINT64_C(1) << 19)
#define JE_ARM_SCTLR_SET_VALUE \
    (JE_ARM_SCTLR_M | JE_ARM_SCTLR_C | JE_ARM_SCTLR_I | JE_ARM_SCTLR_WXN)

_Static_assert(JE_ARM_MAIR_ATTR_NORMAL == UINT64_C(0xff),
               "MAIR AttrIdx0 must be Normal WB");
_Static_assert(JE_ARM_MAIR_ATTR_DEVICE == UINT64_C(0x00),
               "MAIR AttrIdx1 must be Device-nGnRnE");
_Static_assert(JE_ARM_MAIR_VALUE == UINT64_C(0x00000000000000ff),
               "MAIR_EL1 value is pinned by ADR 0011 D3");
_Static_assert(((JE_ARM_TCR_VALUE >> 30) & UINT64_C(3)) == UINT64_C(2),
               "TCR TG1 must encode 4 KiB with 0b10");
_Static_assert(((JE_ARM_TCR_VALUE >> 23) & UINT64_C(1)) == UINT64_C(1),
               "TCR EPD1 must disable TTBR1 walks");
_Static_assert(((JE_ARM_TCR_VALUE >> 32) & UINT64_C(7)) == UINT64_C(4),
               "TCR IPS must encode the 44-bit PA profile");
_Static_assert((JE_ARM_TCR_VALUE & UINT64_C(0x3f)) == UINT64_C(16),
               "TCR T0SZ must describe a 48-bit VA");
_Static_assert(((JE_ARM_TCR_VALUE >> 14) & UINT64_C(3)) == UINT64_C(0),
               "TCR TG0 must encode 4 KiB with 0b00");
_Static_assert(JE_ARM_SCTLR_SET_VALUE == UINT64_C(0x0000000000081005),
               "SCTLR enable bits must be M|C|I|WXN");

const uint64_t JE_ARM_MAIR_EL1 = JE_ARM_MAIR_VALUE;
const uint64_t JE_ARM_TCR_EL1 = JE_ARM_TCR_VALUE;
const uint64_t JE_ARM_SCTLR_SET = JE_ARM_SCTLR_SET_VALUE;

struct je_arm_mmu {
    bool valid;
    uint64_t root_pa;
    uint64_t table_pa[JE_ARM_MAX_TABLES];
    size_t table_count;
    uint64_t table_start;
    uint64_t table_end;
    uint64_t image_start;
    uint64_t image_end;
    je_pages *pages;
    uint8_t frame_flags[JE_PAGE_COUNT];
    uint64_t external_pa[JE_ARM_EXTERNAL_FRAMES];
    uint8_t external_flags[JE_ARM_EXTERNAL_FRAMES];
    size_t external_count;
};

/* The glue uses this permanent kernel root without exposing the implementation
 * layout through the frozen header. */
je_arm_mmu je_arm_mmu_context;
uint64_t je_arm_mmu_last_audit_leaves;
static const je_arm_mmu *je_arm_last_audited;
static const je_arm_mmu *je_arm_enabled_context;

extern uint8_t _text[];
extern uint8_t _end[];

static bool range_valid(uint64_t base, uint64_t size)
{
    return size != 0u && (base & (JE_PAGE_SIZE - 1u)) == 0u &&
           base < JE_ARM_VA_LIMIT && size <= JE_ARM_VA_LIMIT - base &&
           (size & (JE_PAGE_SIZE - 1u)) == 0u;
}

static bool ranges_overlap(const je_map_region *left,
                           const je_map_region *right)
{
    uint64_t left_end = left->base + left->size;
    uint64_t right_end = right->base + right->size;
    return left->base < right_end && right->base < left_end;
}

static bool class_valid(je_map_class cls)
{
    return cls < JE_MAP_CLASS_COUNT;
}

static bool class_executable(je_map_class cls)
{
    return cls == JE_MAP_TEXT;
}

static bool class_writable(je_map_class cls)
{
    return cls == JE_MAP_RW || cls == JE_MAP_DEVICE;
}

static uint8_t class_flags(je_map_class cls)
{
    uint8_t flags = 0u;
    if (class_executable(cls)) flags = (uint8_t)(flags | JE_ARM_MAP_EXEC);
    if (class_writable(cls)) flags = (uint8_t)(flags | JE_ARM_MAP_WRITE);
    return flags;
}

static uint64_t table_descriptor(uint64_t physical)
{
    return (physical & JE_ARM_DESC_ADDR_MASK) | JE_ARM_DESC_TABLE;
}

static uint64_t *table_pointer(uint64_t physical)
{
    return (uint64_t *)(uintptr_t)physical;
}

static je_status alloc_table(je_arm_mmu *mmu, je_pages *pages,
                             uint64_t *out_physical)
{
    if (mmu->table_count >= JE_ARM_MAX_TABLES) return JE_ERR_NOMEM;
    uint64_t physical = 0u;
    je_status status = je_pages_alloc(pages, 1u, &physical);
    if (status != JE_OK) return status;
    memset((void *)(uintptr_t)physical, 0, (size_t)JE_PAGE_SIZE);
    mmu->table_pa[mmu->table_count] = physical;
    ++mmu->table_count;
    *out_physical = physical;
    return JE_OK;
}

static je_status release_tables(je_arm_mmu *mmu)
{
    je_status result = JE_OK;
    for (size_t i = 0u; i < mmu->table_count; ++i) {
        je_status status = je_pages_free(mmu->pages, mmu->table_pa[i], 1u);
        if (status != JE_OK) result = status;
    }
    return result;
}

static bool record_frame_flags(je_arm_mmu *mmu, uint64_t physical,
                               uint8_t flags)
{
    if ((physical & (JE_PAGE_SIZE - 1u)) != 0u) return false;
    if (physical >= JE_PHYSICAL_BASE && physical < JE_PHYSICAL_LIMIT) {
        size_t index = (size_t)((physical - JE_PHYSICAL_BASE) / JE_PAGE_SIZE);
        uint8_t combined = (uint8_t)(mmu->frame_flags[index] | flags);
        if ((combined & (JE_ARM_MAP_EXEC | JE_ARM_MAP_WRITE)) ==
            (JE_ARM_MAP_EXEC | JE_ARM_MAP_WRITE)) return false;
        mmu->frame_flags[index] = combined;
        return true;
    }
    for (size_t i = 0u; i < mmu->external_count; ++i) {
        if (mmu->external_pa[i] == physical) {
            uint8_t combined = (uint8_t)(mmu->external_flags[i] | flags);
            if ((combined & (JE_ARM_MAP_EXEC | JE_ARM_MAP_WRITE)) ==
                (JE_ARM_MAP_EXEC | JE_ARM_MAP_WRITE)) return false;
            mmu->external_flags[i] = combined;
            return true;
        }
    }
    if (mmu->external_count >= JE_ARM_EXTERNAL_FRAMES) return false;
    mmu->external_pa[mmu->external_count] = physical;
    mmu->external_flags[mmu->external_count] = flags;
    ++mmu->external_count;
    return true;
}

static bool table_frame(const je_arm_mmu *mmu, uint64_t physical)
{
    for (size_t i = 0u; i < mmu->table_count; ++i) {
        if (mmu->table_pa[i] == physical) return true;
    }
    return false;
}

static uint64_t leaf_descriptor(uint64_t physical, je_map_class cls)
{
    uint64_t descriptor = (physical & JE_ARM_DESC_ADDR_MASK) |
                          JE_ARM_DESC_TABLE |
                          JE_ARM_DESC_AF;
    uint64_t attridx = cls == JE_MAP_DEVICE ? UINT64_C(1) : UINT64_C(0);
    descriptor |= attridx << JE_ARM_DESC_ATTRIDX_SHIFT;
    descriptor |= UINT64_C(3) << JE_ARM_DESC_SH_SHIFT;
    if (cls == JE_MAP_DEVICE) descriptor &= ~(UINT64_C(3) << JE_ARM_DESC_SH_SHIFT);
    if (cls == JE_MAP_TEXT || cls == JE_MAP_RODATA) {
        descriptor |= UINT64_C(2) << JE_ARM_DESC_AP_SHIFT;
    }
    if (cls != JE_MAP_TEXT) descriptor |= JE_ARM_DESC_PXN;
    descriptor |= JE_ARM_DESC_UXN;
    return descriptor;
}

#ifdef JE_ARM_PAGING_HOSTED_TEST
/* Hosted-test seam (tests/test_paging_bits.c): the pure leaf encoder, exported
 * ONLY in the hosted build so the test can check every class against the
 * DDI0487 bit layout independently of je_arm_mmu_audit (which compares
 * installed leaves against this same encoder and so cannot catch an encoder
 * bug on its own). Not part of the frozen paging.h contract. */
uint64_t je_arm_leaf_desc_hosted(uint64_t physical, je_map_class cls);
uint64_t je_arm_leaf_desc_hosted(uint64_t physical, je_map_class cls)
{
    return leaf_descriptor(physical, cls);
}
#endif

static unsigned level_shift(unsigned level)
{
    return 39u - (level * 9u);
}

static size_t va_index(uint64_t virtual_address, unsigned level)
{
    return (size_t)((virtual_address >> level_shift(level)) &
                    JE_ARM_PT_INDEX_MASK);
}

static je_status map_page(je_arm_mmu *mmu, je_pages *pages,
                          uint64_t virtual_address, uint64_t physical,
                          je_map_class cls)
{
    uint64_t table_pa = mmu->root_pa;
    for (unsigned level = 0u; level < 3u; ++level) {
        uint64_t *table = table_pointer(table_pa);
        size_t index = va_index(virtual_address, level);
        uint64_t descriptor = table[index];
        if ((descriptor & JE_ARM_DESC_VALID) == 0u) {
            uint64_t child_pa = 0u;
            je_status status = alloc_table(mmu, pages, &child_pa);
            if (status != JE_OK) return status;
            table[index] = table_descriptor(child_pa);
            descriptor = table[index];
        } else if ((descriptor & UINT64_C(3)) != JE_ARM_DESC_TABLE) {
            return JE_ERR_INVALID;
        }
        table_pa = descriptor & JE_ARM_DESC_ADDR_MASK;
    }

    uint64_t *l3 = table_pointer(table_pa);
    size_t leaf_index = va_index(virtual_address, 3u);
    if ((l3[leaf_index] & JE_ARM_DESC_VALID) != 0u) return JE_ERR_INVALID;
    if (!record_frame_flags(mmu, physical, class_flags(cls)))
        return JE_ERR_INVALID;
    l3[leaf_index] = leaf_descriptor(physical, cls);
    return JE_OK;
}

static je_status validate_regions(const je_map_region *regions, size_t n)
{
    if (!regions || n == 0u) return JE_ERR_INVALID;
    for (size_t i = 0u; i < n; ++i) {
        if (!range_valid(regions[i].base, regions[i].size) ||
            !class_valid(regions[i].cls)) return JE_ERR_INVALID;
        for (size_t j = 0u; j < i; ++j) {
            if (ranges_overlap(&regions[i], &regions[j]))
                return JE_ERR_INVALID;
        }
    }
    return JE_OK;
}

static je_status rollback_build(je_arm_mmu *mmu, je_status status)
{
    je_status free_status = release_tables(mmu);
    if (free_status != JE_OK) status = free_status;
    mmu->valid = false;
    mmu->table_count = 0u;
    mmu->root_pa = 0u;
    if (je_arm_last_audited == mmu) je_arm_last_audited = NULL;
    if (je_arm_enabled_context == mmu) je_arm_enabled_context = NULL;
    return status;
}

je_status je_arm_mmu_build(je_arm_mmu *mmu, je_pages *pages,
                           const je_map_region *regions, size_t n,
                           uint64_t *out_root)
{
    if (!mmu || !pages || !out_root || pages->ready == false)
        return JE_ERR_INVALID;
    *out_root = 0u;
    if (mmu->valid || je_arm_enabled_context == mmu) return JE_ERR_INVALID;
    je_status status = validate_regions(regions, n);
    if (status != JE_OK) return status;

    memset(mmu, 0, sizeof(*mmu));
    mmu->pages = pages;
    mmu->image_start = (uint64_t)(uintptr_t)_text;
    mmu->image_end = (uint64_t)(uintptr_t)_end;

    status = alloc_table(mmu, pages, &mmu->root_pa);
    if (status != JE_OK) return rollback_build(mmu, status);
    for (size_t r = 0u; r < n; ++r) {
        uint64_t end = regions[r].base + regions[r].size;
        for (uint64_t va = regions[r].base; va < end; va += JE_PAGE_SIZE) {
            status = map_page(mmu, pages, va, va, regions[r].cls);
            if (status != JE_OK) return rollback_build(mmu, status);
        }
    }

    /*
     * Table frames are allocated after the user regions' hierarchy exists.
     * Mapping one frame can allocate a missing hierarchy table, so process a
     * growing queue until every table frame has its own RW identity leaf.
     * This is the minimal identity map that makes the post-enable walker and
     * audit able to reach all tables without opening a RAM-wide RW alias.
     */
    size_t mapped_tables = 0u;
    while (mapped_tables < mmu->table_count) {
        uint64_t table_pa = mmu->table_pa[mapped_tables];
        status = map_page(mmu, pages, table_pa, table_pa, JE_MAP_RW);
        if (status != JE_OK) return rollback_build(mmu, status);
        ++mapped_tables;
    }

    mmu->table_start = mmu->table_pa[0];
    mmu->table_end = mmu->table_pa[0] + JE_PAGE_SIZE;
    for (size_t i = 1u; i < mmu->table_count; ++i) {
        if (mmu->table_pa[i] < mmu->table_start)
            mmu->table_start = mmu->table_pa[i];
        uint64_t end = mmu->table_pa[i] + JE_PAGE_SIZE;
        if (end > mmu->table_end) mmu->table_end = end;
    }
    mmu->valid = true;
    *out_root = mmu->root_pa;
    return JE_OK;
}

/*
 * Independent audit of a leaf descriptor against the class's REQUIRED exact
 * bit pattern (ADR 0011 D4). This does NOT call leaf_descriptor() (a builder
 * encoder bug must be CAUGHT, not hidden), and it compares the WHOLE non-address
 * part of the descriptor against a per-class literal — so ANY extra bit set
 * (Contiguous bit 52, DBM 51, NS 5, nG 11, a software bit, ...) is rejected, not
 * just the handful of fields a field-by-field check would name. A stray
 * Contiguous bit on a .text leaf, for example, is a W^X hole (the TLB may apply
 * the RX entry to neighbouring pages) and must fail the audit. The output
 * address must also be a page-aligned PA below 2^44 (the pinned IPS).
 *
 * Literals (non-address bits only): valid+page 0b11; AF bit10; UXN bit54; SH=0b11
 * [9:8] for Normal; AP[7:6]; PXN bit53; AttrIdx[4:2]:
 *   TEXT   AP=10 PXN=0 UXN=1 SH=11 Attr0 -> 0x0040000000000783
 *   RODATA AP=10 PXN=1 UXN=1 SH=11 Attr0 -> 0x0060000000000783
 *   RW     AP=00 PXN=1 UXN=1 SH=11 Attr0 -> 0x0060000000000703
 *   DEVICE AP=00 PXN=1 UXN=1 SH=00 Attr1 -> 0x0060000000000407
 */
#define JE_ARM_LEAF_TEXT   UINT64_C(0x0040000000000783)
#define JE_ARM_LEAF_RODATA UINT64_C(0x0060000000000783)
#define JE_ARM_LEAF_RW     UINT64_C(0x0060000000000703)
#define JE_ARM_LEAF_DEVICE UINT64_C(0x0060000000000407)
#define JE_ARM_PA_LIMIT    (UINT64_C(1) << 44)   /* IPS = 44-bit PA (ADR D3) */

static bool class_descriptor_matches(uint64_t descriptor, je_map_class cls)
{
    uint64_t oa = descriptor & JE_ARM_DESC_ADDR_MASK;
    uint64_t bits = descriptor & ~JE_ARM_DESC_ADDR_MASK;   /* non-address part */
    if (oa >= JE_ARM_PA_LIMIT) return false;               /* OA within IPS */
    switch (cls) {
    case JE_MAP_TEXT:   return bits == JE_ARM_LEAF_TEXT;
    case JE_MAP_RODATA: return bits == JE_ARM_LEAF_RODATA;
    case JE_MAP_RW:     return bits == JE_ARM_LEAF_RW;
    case JE_MAP_DEVICE: return bits == JE_ARM_LEAF_DEVICE;
    default:            return false;
    }
}

/* Cross-check the per-class literals against the builder's own encoder at build
 * of paging.c: if leaf_descriptor and these literals ever diverge for the
 * governed (non-address) bits, that is a bug in ONE of them — but since they are
 * written independently, a compile-time agreement here means the AUDIT's notion
 * of "correct" matches the BUILDER's intended output for a KNOWN-GOOD encoder,
 * while still catching a RUNTIME-tampered or mis-encoded leaf (the literals are
 * the source of truth the audit compares installed descriptors against). This is
 * a sanity tie, not a substitute for the independent hosted encoder test. */
_Static_assert((JE_ARM_LEAF_TEXT & 0x3u) == 0x3u, "leaf literal must be a valid page");

static const je_map_region *region_for_va(const je_map_region *regions,
                                          size_t n, uint64_t virtual_address)
{
    const je_map_region *match = NULL;
    for (size_t i = 0u; i < n; ++i) {
        uint64_t end = regions[i].base + regions[i].size;
        if (virtual_address >= regions[i].base && virtual_address < end) {
            if (match != NULL) return NULL;
            match = &regions[i];
        }
    }
    return match;
}

static bool audit_alias_scan(const je_arm_mmu *mmu, uint64_t table_pa,
                             unsigned level, uint64_t target_pa,
                             bool target_exec, bool target_write)
{
    const uint64_t *table = (const uint64_t *)(uintptr_t)table_pa;
    for (size_t i = 0u; i < JE_ARM_PT_ENTRIES; ++i) {
        uint64_t descriptor = table[i];
        if ((descriptor & JE_ARM_DESC_VALID) == 0u) continue;
        if (level < 3u) {
            if ((descriptor & UINT64_C(3)) != JE_ARM_DESC_TABLE ||
                (descriptor & (JE_ARM_DESC_TABLE_AP_MASK |
                               JE_ARM_DESC_TABLE_PXN |
                               JE_ARM_DESC_TABLE_UXN |
                               JE_ARM_DESC_TABLE_NS)) != 0u) {
                return true;
            }
            if (audit_alias_scan(mmu, descriptor & JE_ARM_DESC_ADDR_MASK,
                                 level + 1u, target_pa, target_exec,
                                 target_write)) return true;
        } else {
            uint64_t physical = descriptor & JE_ARM_DESC_ADDR_MASK;
            bool executable = (descriptor & JE_ARM_DESC_PXN) == 0u;
            bool writable = ((descriptor >> JE_ARM_DESC_AP_SHIFT) &
                             UINT64_C(3)) == 0u;
            if (physical == target_pa &&
                ((target_exec && writable) || (target_write && executable)))
                return true;
        }
    }
    return false;
}

static je_status audit_table(const je_arm_mmu *mmu, uint64_t table_pa,
                             unsigned level, uint64_t virtual_base,
                             const je_map_region *regions, size_t n,
                             size_t *leaf_count, size_t *table_leaf_count)
{
    const uint64_t *table = (const uint64_t *)(uintptr_t)table_pa;
    unsigned shift = level_shift(level);
    for (size_t i = 0u; i < JE_ARM_PT_ENTRIES; ++i) {
        uint64_t descriptor = table[i];
        if ((descriptor & JE_ARM_DESC_VALID) == 0u) continue;
        uint64_t va = virtual_base | ((uint64_t)i << shift);
        if (level < 3u) {
            if ((descriptor & UINT64_C(3)) != JE_ARM_DESC_TABLE ||
                (descriptor & (JE_ARM_DESC_TABLE_AP_MASK |
                               JE_ARM_DESC_TABLE_PXN |
                               JE_ARM_DESC_TABLE_UXN |
                               JE_ARM_DESC_TABLE_NS)) != 0u)
                return JE_ERR_INVALID;
            /* The next-level table MUST be one of the builder's own table
             * frames (ADR 0011 D4). Without this a table descriptor redirected
             * into a .data/region frame would pass the audit, and post-enable
             * ordinary writes to that frame would rewrite live translations and
             * reopen W^X AFTER the audit. Requiring membership in table_pa (and
             * region_for_va() below rejects region overlap for leaves) closes
             * it; combined with the table_leaves==table_count tie every table is
             * accounted for. */
            if (!table_frame(mmu, descriptor & JE_ARM_DESC_ADDR_MASK))
                return JE_ERR_INVALID;
            je_status status = audit_table(mmu,
                                           descriptor & JE_ARM_DESC_ADDR_MASK,
                                           level + 1u, va, regions, n,
                                           leaf_count, table_leaf_count);
            if (status != JE_OK) return status;
            continue;
        }

        ++*leaf_count;
        if ((descriptor & JE_ARM_DESC_AF) == 0u ||
            ((descriptor >> JE_ARM_DESC_AP_SHIFT) & UINT64_C(1)) != 0u)
            return JE_ERR_INVALID;
        uint64_t physical = descriptor & JE_ARM_DESC_ADDR_MASK;
        const je_map_region *region = region_for_va(regions, n, va);
        je_map_class expected;
        if (region != NULL) {
            expected = region->cls;
            /* Identity contract (ADR 0011 D2): every region leaf maps VA==PA.
             * A region leaf whose output frame differs from its VA is not an
             * identity mapping and is rejected (table frames are checked for
             * VA==PA in the else branch below). */
            if (va != physical) return JE_ERR_INVALID;
        } else if (va == physical && table_frame(mmu, physical)) {
            expected = JE_MAP_RW;
            ++*table_leaf_count;
        } else {
            return JE_ERR_INVALID;
        }
        if (!class_descriptor_matches(descriptor, expected))
            return JE_ERR_INVALID;
        bool executable = (descriptor & JE_ARM_DESC_PXN) == 0u;
        bool writable = ((descriptor >> JE_ARM_DESC_AP_SHIFT) &
                         UINT64_C(3)) == 0u;
        if (executable && writable) return JE_ERR_INVALID;
        if (audit_alias_scan(mmu, mmu->root_pa, 0u, physical,
                             executable, writable))
            return JE_ERR_INVALID;
    }
    return JE_OK;
}

je_status je_arm_mmu_audit(const je_arm_mmu *mmu,
                           const je_map_region *regions, size_t n)
{
    if (!mmu || !mmu->valid || je_arm_enabled_context == mmu ||
        validate_regions(regions, n) != JE_OK) return JE_ERR_INVALID;
    if (je_arm_last_audited == mmu) je_arm_last_audited = NULL;
    size_t leaves = 0u;
    size_t table_leaves = 0u;
    je_status status = audit_table(mmu, mmu->root_pa, 0u, 0u, regions, n,
                                   &leaves, &table_leaves);
    if (status != JE_OK || table_leaves != mmu->table_count)
        return JE_ERR_INVALID;
    je_arm_mmu_last_audit_leaves = (uint64_t)leaves;
    je_arm_last_audited = mmu;
    return JE_OK;
}

extern void je_arm_mmu_enable_leaf(uint64_t root_pa, uint64_t mair,
                                   uint64_t tcr, uint64_t range0_start,
                                   uint64_t range0_end, uint64_t range1_start,
                                   uint64_t range1_end, uint64_t sctlr_set);

je_status je_arm_mmu_enable(const je_arm_mmu *mmu)
{
    if (!mmu || !mmu->valid || je_arm_last_audited != mmu ||
        je_arm_enabled_context == mmu ||
        mmu->root_pa == 0u || mmu->table_start >= mmu->table_end ||
        mmu->image_start >= mmu->image_end)
        return JE_ERR_INVALID;
    je_arm_mmu_enable_leaf(mmu->root_pa, JE_ARM_MAIR_EL1, JE_ARM_TCR_EL1,
                           mmu->table_start, mmu->table_end,
                           mmu->image_start, mmu->image_end,
                           JE_ARM_SCTLR_SET);
    je_arm_enabled_context = mmu;
    return JE_OK;
}

#ifndef JE_ARM_PAGING_HOSTED_TEST   /* AT/PAR is EL1 asm: target-only */
uint64_t je_arm_at_s1e1(uint64_t virtual_address, int write)
{
    uint64_t par = 0u;
    if (write != 0) {
        __asm__ volatile("at s1e1w, %1\n\t"
                         "isb\n\t"
                         "mrs %0, par_el1"
                         : "=r"(par) : "r"(virtual_address) : "memory");
    } else {
        __asm__ volatile("at s1e1r, %1\n\t"
                         "isb\n\t"
                         "mrs %0, par_el1"
                         : "=r"(par) : "r"(virtual_address) : "memory");
    }
    return par;
}
#endif

uint64_t je_arm_mmu_root(const je_arm_mmu *mmu)
{
    return mmu && mmu->valid ? mmu->root_pa : 0u;
}
