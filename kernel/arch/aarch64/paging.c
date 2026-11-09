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
#include <jeios/memory.h>
#include "paging.h"

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
#define JE_ARM_DESC_NG (UINT64_C(1) << 11)
#define JE_ARM_DESC_PXN (UINT64_C(1) << 53)
#define JE_ARM_DESC_UXN (UINT64_C(1) << 54)
#define JE_ARM_DESC_TABLE_AP_MASK (UINT64_C(3) << 61)
#define JE_ARM_DESC_TABLE_PXN (UINT64_C(1) << 59)
#define JE_ARM_DESC_TABLE_UXN (UINT64_C(1) << 60)
#define JE_ARM_DESC_TABLE_NS (UINT64_C(1) << 63)
#define JE_ARM_PA_LIMIT    (UINT64_C(1) << 44)   /* IPS = 44-bit PA (ADR D3) */

#define JE_ARM_MAP_EXEC UINT8_C(1)
#define JE_ARM_MAP_WRITE UINT8_C(2)
/* Ledger bits beyond W/X (ADR 0013 D4). PRESENT: the frame has at least one
 * leaf (set by every install, static or runtime; RODATA frames would otherwise
 * read as 0 == "unmapped"). RUNTIME: the frame's single leaf was installed by
 * je_arm_mmu_map_page after enable and may be removed by unmap. Neither bit
 * participates in the W^X test, which only looks at EXEC|WRITE. */
#define JE_ARM_MAP_PRESENT UINT8_C(4)
#define JE_ARM_MAP_RUNTIME UINT8_C(8)
#define JE_ARM_MAP_USER UINT8_C(16)
#define JE_ARM_EXTERNAL_FRAMES 64u
/* Flex starting point for MP3a-1: 128 x 2 MiB RAM slots need one L3 each;
 * the kernel's upper tables and device/image branches bring the QEMU 256 MiB
 * profile to roughly 135 tables. 256 leaves review headroom for those tables
 * and for a future bounded RAM profile without changing the audit shape. */
#define JE_ARM_MAX_TABLES 256u

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
je_arm_mmu_audit_reject je_arm_mmu_last_audit_reject;
uint64_t je_arm_mmu_last_audit_reads;
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
    return cls == JE_MAP_TEXT || cls == JE_MAP_USER_CODE;
}

static bool class_writable(je_map_class cls)
{
    return cls == JE_MAP_RW || cls == JE_MAP_DEVICE || cls == JE_MAP_USER_DATA;
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

static bool table_ordinal(const je_arm_mmu *mmu, uint64_t physical,
                          size_t *ordinal)
{
    if (!mmu || !ordinal || mmu->table_count == 0u ||
        mmu->table_count > JE_ARM_MAX_TABLES)
        return false;
    for (size_t i = 0u; i < mmu->table_count; ++i) {
        if (mmu->table_pa[i] == physical) {
            *ordinal = i;
            return true;
        }
    }
    return false;
}

static bool table_frame(const je_arm_mmu *mmu, uint64_t physical)
{
    size_t ordinal;
    return table_ordinal(mmu, physical, &ordinal);
}

static uint64_t leaf_descriptor(uint64_t physical, je_map_class cls)
{
    if (cls == JE_MAP_USER_CODE)
        return (physical & JE_ARM_DESC_ADDR_MASK) | JE_ARM_USER_CODE_BITS;
    if (cls == JE_MAP_USER_RODATA)
        return (physical & JE_ARM_DESC_ADDR_MASK) | JE_ARM_USER_RODATA_BITS;
    if (cls == JE_MAP_USER_DATA)
        return (physical & JE_ARM_DESC_ADDR_MASK) | JE_ARM_USER_DATA_BITS;

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
    if (cls != JE_MAP_TEXT)
        descriptor |= JE_ARM_DESC_PXN;
    /* Preserve the frozen kernel TEXT literal (UXN=1, AP[1]=0); only the
     * new EL0 USER_CODE class clears UXN. */
    if (cls != JE_MAP_USER_CODE)
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
    if (!record_frame_flags(mmu, physical,
                            (uint8_t)(class_flags(cls) | JE_ARM_MAP_PRESENT)))
        return JE_ERR_INVALID;
    l3[leaf_index] = leaf_descriptor(physical, cls);
    return JE_OK;
}

static je_status ensure_l3_for_va(je_arm_mmu *mmu, je_pages *pages,
                                  uint64_t virtual_address)
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
        if ((descriptor & (JE_ARM_DESC_TABLE_AP_MASK |
                           JE_ARM_DESC_TABLE_PXN |
                           JE_ARM_DESC_TABLE_UXN |
                           JE_ARM_DESC_TABLE_NS)) != 0u)
            return JE_ERR_INVALID;
        table_pa = descriptor & JE_ARM_DESC_ADDR_MASK;
    }
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

je_status je_arm_mmu_prebuild_ram_l3(je_arm_mmu *mmu)
{
    const uint64_t l3_span = UINT64_C(0x200000);
    const uint64_t ram_base = JE_PHYSICAL_BASE;
    const uint64_t ram_end = JE_PHYSICAL_LIMIT;
    if (!mmu || !mmu->valid || je_arm_enabled_context == mmu ||
        mmu->pages == NULL || (ram_base & (l3_span - 1u)) != 0u ||
        (ram_end & (l3_span - 1u)) != 0u)
        return JE_ERR_INVALID;

    /* The user-test profile calls this once, before the fresh audit. */
    je_arm_last_audited = NULL;
    const size_t first_new_table = mmu->table_count;
    for (uint64_t va = ram_base; va < ram_end; va += l3_span) {
        je_status status = ensure_l3_for_va(mmu, mmu->pages, va);
        if (status != JE_OK) return status;
    }

    /*
     * Identity-map only the table frames just added. The queue can grow while
     * a table frame's identity leaf creates a missing hierarchy, although the
     * RAM L3 prebuild makes that growth bounded and normally unnecessary.
     */
    size_t mapped_tables = first_new_table;
    while (mapped_tables < mmu->table_count) {
        uint64_t table_pa = mmu->table_pa[mapped_tables];
        je_status status = map_page(mmu, mmu->pages, table_pa, table_pa, JE_MAP_RW);
        if (status != JE_OK) return status;
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
 *
 * MP3a-1 EL0 classes use the same documented fields:
 *   valid+page 0b11 | AttrIdx[4:2] | AP[7:6] | SH[9:8] | AF | nG |
 *   PXN[53] | UXN[54].
 *   USER_CODE   = 0x3 | (0<<2) | (0b11<<6) | (0b11<<8) | (1<<10) |
 *                 (1<<11) | (1<<53) | (0<<54) = 0x0020000000000fc3
 *   USER_RODATA = USER_CODE with UXN[54] set = 0x0060000000000fc3
 *   USER_DATA   = AP=0b01 (EL1+EL0 RW), UXN=1, PXN=1, nG=1 =
 *                 0x0060000000000f43.
 * AP[1] is descriptor bit 6; all three EL0 classes set it. USER_CODE is the
 * only EL0-executable class, and no class is both writable and executable. */
#define JE_ARM_LEAF_TEXT   UINT64_C(0x0040000000000783)
#define JE_ARM_LEAF_RODATA UINT64_C(0x0060000000000783)
#define JE_ARM_LEAF_RW     UINT64_C(0x0060000000000703)
#define JE_ARM_LEAF_DEVICE UINT64_C(0x0060000000000407)
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
    case JE_MAP_USER_CODE: return bits == JE_ARM_USER_CODE_BITS;
    case JE_MAP_USER_RODATA: return bits == JE_ARM_USER_RODATA_BITS;
    case JE_MAP_USER_DATA: return bits == JE_ARM_USER_DATA_BITS;
    default:            return false;
    }
}

/* The user literals are shared with vm_backend.c through paging.h. The hosted
 * encoder test remains independent of both the encoder and the audit. */
_Static_assert((JE_ARM_LEAF_TEXT & 0x3u) == 0x3u, "leaf literal must be a valid page");
_Static_assert(JE_ARM_USER_CODE_BITS == UINT64_C(0x0020000000000fc3),
               "EL0 code literal is pinned by the documented bit layout");
_Static_assert(JE_ARM_USER_RODATA_BITS == UINT64_C(0x0060000000000fc3),
               "EL0 rodata literal is pinned by the documented bit layout");
_Static_assert(JE_ARM_USER_DATA_BITS == UINT64_C(0x0060000000000f43),
               "EL0 data literal is pinned by the documented bit layout");
_Static_assert((JE_ARM_USER_CODE_BITS & (UINT64_C(1) << 6)) != 0u &&
               (JE_ARM_USER_RODATA_BITS & (UINT64_C(1) << 6)) != 0u &&
               (JE_ARM_USER_DATA_BITS & (UINT64_C(1) << 6)) != 0u,
               "EL0 classes must set AP[1]");
_Static_assert((JE_ARM_USER_CODE_BITS & (UINT64_C(1) << 54)) == 0u &&
               (JE_ARM_USER_RODATA_BITS & (UINT64_C(1) << 54)) != 0u &&
               (JE_ARM_USER_DATA_BITS & (UINT64_C(1) << 54)) != 0u,
               "only USER_CODE is EL0 executable");
_Static_assert((JE_ARM_USER_CODE_BITS & (UINT64_C(1) << 7)) != 0u &&
               (JE_ARM_USER_DATA_BITS & (UINT64_C(1) << 7)) == 0u,
               "EL0 AP encodings preserve RO versus RW");

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

typedef struct {
    size_t ordinal;
    unsigned level;
    uint64_t virtual_base;
} je_arm_audit_table;

static bool audit_alias_scan(const je_arm_mmu *mmu,
                             const je_arm_audit_table *work,
                             size_t work_count, uint64_t target_pa,
                             bool target_exec, bool target_write)
{
    /*
     * Phase 1 has proved every work item is a unique ledger member and every
     * level-3 item is reachable. This scan therefore never follows a raw
     * descriptor pointer. It is deliberately conservative: for each leaf,
     * it reads at most 512 entries in each recorded level-3 table, or
     * <= leaves * 512 * (#L3 tables) descriptor reads overall.
     */
    for (size_t table_index = 0u; table_index < work_count; ++table_index) {
        if (work[table_index].level != 3u) continue;
        const uint64_t *table =
            table_pointer(mmu->table_pa[work[table_index].ordinal]);
        for (size_t i = 0u; i < JE_ARM_PT_ENTRIES; ++i) {
            ++je_arm_mmu_last_audit_reads;
            uint64_t descriptor = table[i];
            if ((descriptor & JE_ARM_DESC_VALID) == 0u) continue;
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

static je_status audit_reject(je_arm_mmu_audit_reject reason)
{
    je_arm_mmu_last_audit_reject = reason;
    return JE_ERR_INVALID;
}

je_status je_arm_mmu_audit(const je_arm_mmu *mmu,
                           const je_map_region *regions, size_t n)
{
    /*
     * The table memory, ledger, and regions are immutable during this audit:
     * one CPU, pre-enable, and no DMA. The verdict assumes that contract.
     * Phase 2 only reads recorded level-3 ordinals, so it never follows a
     * descriptor pointer even if table contents change after Phase 1.
     */
    /* Any audit attempt invalidates a prior enable grant, regardless of which
     * context or arguments the caller supplies. Only the full-success path
     * below may publish a new eligibility stamp. */
    je_arm_last_audited = NULL;
    je_arm_mmu_last_audit_leaves = 0u;
    je_arm_mmu_last_audit_reads = 0u;
    je_arm_mmu_last_audit_reject = JE_ARM_AUDIT_REJECT_NONE;

    if (!mmu || !mmu->valid || je_arm_enabled_context == mmu ||
        validate_regions(regions, n) != JE_OK)
        return audit_reject(JE_ARM_AUDIT_REJECT_PRECONDITION);

    /*
     * Phase 0 validates the complete ledger before any table page is
     * dereferenced. In particular, bound table_count before indexing the
     * fixed-size table_pa[] array, then reject zero, misaligned, out-of-range,
     * or duplicate physical frames.
     */
    if (mmu->table_count == 0u || mmu->table_count > JE_ARM_MAX_TABLES)
        return audit_reject(JE_ARM_AUDIT_REJECT_COUNT);
    if (mmu->table_start >= mmu->table_end ||
        (mmu->table_start & (JE_PAGE_SIZE - 1u)) != 0u ||
        (mmu->table_end & (JE_PAGE_SIZE - 1u)) != 0u ||
        mmu->table_end > JE_ARM_PA_LIMIT)
        return audit_reject(JE_ARM_AUDIT_REJECT_LEDGER);
    for (size_t i = 0u; i < mmu->table_count; ++i) {
        uint64_t physical = mmu->table_pa[i];
        if (physical == 0u || (physical & (JE_PAGE_SIZE - 1u)) != 0u ||
            physical >= JE_ARM_PA_LIMIT ||
            physical < mmu->table_start || physical >= mmu->table_end)
            return audit_reject(JE_ARM_AUDIT_REJECT_LEDGER);
        for (size_t j = 0u; j < i; ++j) {
            if (mmu->table_pa[j] == physical)
                return audit_reject(JE_ARM_AUDIT_REJECT_LEDGER);
        }
    }

    size_t root_ordinal;
    if (!table_ordinal(mmu, mmu->root_pa, &root_ordinal))
        return audit_reject(JE_ARM_AUDIT_REJECT_ROOT);

    /*
     * Phase 1 is an explicit bounded structural walk. A table is marked and
     * enqueued before its page is read; every later table dereference uses an
     * ordinal that came from the validated ledger.
     */
    uint64_t visited[(JE_ARM_MAX_TABLES + 63u) / 64u] = { 0u };
    je_arm_audit_table work[JE_ARM_MAX_TABLES];
    size_t visited_count = 1u;
    size_t work_count = 1u;
    size_t work_head = 0u;
    visited[root_ordinal / 64u] |=
        UINT64_C(1) << (root_ordinal % 64u);
    work[0] = (je_arm_audit_table){ root_ordinal, 0u, 0u };

    while (work_head < work_count) {
        je_arm_audit_table current = work[work_head];
        ++work_head;
        const uint64_t *table =
            table_pointer(mmu->table_pa[current.ordinal]);
        unsigned shift = level_shift(current.level);
        for (size_t i = 0u; i < JE_ARM_PT_ENTRIES; ++i) {
            ++je_arm_mmu_last_audit_reads;
            uint64_t descriptor = table[i];
            if ((descriptor & JE_ARM_DESC_VALID) == 0u) continue;
            if (current.level >= 3u) continue;
            if ((descriptor & UINT64_C(3)) != JE_ARM_DESC_TABLE ||
                (descriptor & (JE_ARM_DESC_TABLE_AP_MASK |
                               JE_ARM_DESC_TABLE_PXN |
                               JE_ARM_DESC_TABLE_UXN |
                               JE_ARM_DESC_TABLE_NS)) != 0u)
                return audit_reject(JE_ARM_AUDIT_REJECT_MALFORMED);

            size_t child_ordinal;
            uint64_t child_pa = descriptor & JE_ARM_DESC_ADDR_MASK;
            if (!table_ordinal(mmu, child_pa, &child_ordinal))
                return audit_reject(JE_ARM_AUDIT_REJECT_FOREIGN);
            size_t child_word = child_ordinal / 64u;
            uint64_t child_bit = UINT64_C(1) << (child_ordinal % 64u);
            if ((visited[child_word] & child_bit) != 0u)
                return audit_reject(JE_ARM_AUDIT_REJECT_REVISIT);
            if (work_count >= JE_ARM_MAX_TABLES)
                return audit_reject(JE_ARM_AUDIT_REJECT_COUNT);
            visited[child_word] |= child_bit;
            ++visited_count;
            work[work_count] = (je_arm_audit_table){
                child_ordinal,
                current.level + 1u,
                current.virtual_base | ((uint64_t)i << shift)
            };
            ++work_count;
        }
    }
    if (visited_count != mmu->table_count)
        return audit_reject(JE_ARM_AUDIT_REJECT_UNREACHED);

    /*
     * Phase 2 checks leaves only in the validated work-list. This retains the
     * existing independent literals, identity mapping, W^X, and table-leaf
     * cardinality checks without dereferencing a raw child descriptor.
     */
    size_t leaves = 0u;
    size_t table_leaves = 0u;
    for (size_t work_index = 0u; work_index < work_count; ++work_index) {
        je_arm_audit_table current = work[work_index];
        if (current.level != 3u) continue;
        const uint64_t *table =
            table_pointer(mmu->table_pa[current.ordinal]);
        unsigned shift = level_shift(current.level);
        for (size_t i = 0u; i < JE_ARM_PT_ENTRIES; ++i) {
            ++je_arm_mmu_last_audit_reads;
            uint64_t descriptor = table[i];
            if ((descriptor & JE_ARM_DESC_VALID) == 0u) continue;
            ++leaves;
            uint64_t va = current.virtual_base | ((uint64_t)i << shift);
            if ((descriptor & JE_ARM_DESC_AF) == 0u ||
                ((descriptor >> JE_ARM_DESC_AP_SHIFT) & UINT64_C(1)) != 0u)
                return audit_reject(JE_ARM_AUDIT_REJECT_LEAF);
            uint64_t physical = descriptor & JE_ARM_DESC_ADDR_MASK;
            const je_map_region *region = region_for_va(regions, n, va);
            je_map_class expected;
            if (region != NULL) {
                expected = region->cls;
                /* Identity contract (ADR 0011 D2): every region leaf maps VA==PA.
                 * A region leaf whose output frame differs from its VA is not an
                 * identity mapping and is rejected (table frames are checked for
                 * VA==PA in the else branch below). */
                if (va != physical)
                    return audit_reject(JE_ARM_AUDIT_REJECT_LEAF);
            } else if (va == physical && table_frame(mmu, physical)) {
                expected = JE_MAP_RW;
                ++table_leaves;
            } else {
                return audit_reject(JE_ARM_AUDIT_REJECT_LEAF);
            }
            if (!class_descriptor_matches(descriptor, expected)) {
                return audit_reject(JE_ARM_AUDIT_REJECT_LEAF);
            }
            bool executable = (descriptor & JE_ARM_DESC_PXN) == 0u;
            bool writable = ((descriptor >> JE_ARM_DESC_AP_SHIFT) &
                             UINT64_C(3)) == 0u;
            if (executable && writable)
                return audit_reject(JE_ARM_AUDIT_REJECT_LEAF);
            if (audit_alias_scan(mmu, work, work_count, physical,
                                 executable, writable))
                return audit_reject(JE_ARM_AUDIT_REJECT_ALIAS);
        }
    }
    if (table_leaves != mmu->table_count)
        return audit_reject(JE_ARM_AUDIT_REJECT_LEAF);
    je_arm_mmu_last_audit_leaves = (uint64_t)leaves;
    je_arm_last_audited = mmu;
    return JE_OK;
}

extern void je_arm_mmu_enable_leaf(uint64_t root_pa, uint64_t mair,
                                   uint64_t tcr, uint64_t range0_start,
                                   uint64_t range0_end, uint64_t range1_start,
                                   uint64_t range1_end, uint64_t sctlr_set);
static bool in_window(uint64_t physical);
static size_t window_index(uint64_t physical);

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

bool je_arm_mmu_is_enabled(const je_arm_mmu *mmu)
{
    return mmu != NULL && je_arm_enabled_context == mmu;
}

je_status je_arm_mmu_publish_user_frame(je_arm_mmu *mmu, uint64_t physical,
                                         je_map_class cls)
{
    if (!mmu || !mmu->valid ||
        (cls != JE_MAP_USER_CODE && cls != JE_MAP_USER_RODATA) ||
        !in_window(physical) || table_frame(mmu, physical))
        return JE_ERR_INVALID;

    const size_t index = window_index(physical);
    if (mmu->frame_flags[index] != 0u)
        return JE_ERR_DENIED;
    mmu->frame_flags[index] = (uint8_t)(class_flags(cls) |
                                        JE_ARM_MAP_PRESENT |
                                        JE_ARM_MAP_USER);
    return JE_OK;
}

je_status je_arm_mmu_unpublish_user_frame(je_arm_mmu *mmu,
                                           uint64_t physical)
{
    if (!mmu || !mmu->valid ||
        !in_window(physical))
        return JE_ERR_INVALID;

    const size_t index = window_index(physical);
    const uint8_t flags = mmu->frame_flags[index];
    if ((flags & JE_ARM_MAP_USER) == 0u ||
        (flags & JE_ARM_MAP_RUNTIME) != 0u)
        return JE_ERR_INVALID;
    mmu->frame_flags[index] = 0u;
    return JE_OK;
}

je_status je_arm_mmu_sync_user_code(uint64_t physical)
{
    if (!in_window(physical) || (physical & (JE_PAGE_SIZE - 1u)) != 0u)
        return JE_ERR_INVALID;
#ifdef JE_ARM_PAGING_HOSTED_TEST
    return JE_OK;
#else
    /*
     * DDI0487 cache maintenance: clean the data cache to the point of
     * unification, invalidate the instruction cache to that point, then
     * complete both operations before an EL0 fetch.
     */
    for (uint64_t offset = 0u; offset < JE_PAGE_SIZE;
         offset += UINT64_C(64)) {
        const uint64_t va = physical + offset;
        __asm__ volatile("dc cvau, %0" : : "r"(va) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t offset = 0u; offset < JE_PAGE_SIZE;
         offset += UINT64_C(64)) {
        const uint64_t va = physical + offset;
        __asm__ volatile("ic ivau, %0" : : "r"(va) : "memory");
    }
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
    return JE_OK;
#endif
}

/*
 * ---- Runtime single-page map/unmap + guarded stacks (ADR 0013 D4) ----
 *
 * Contract in paging.h. Design notes the header does not carry:
 *
 * Missing tables (hazard a/b). The static build creates an L3 table only for a
 * 2 MiB region that holds an image page, a device page or a table frame. On
 * QEMU virt the first-fit allocator places the table frames at the bottom of
 * RAM, so the L3 for [0x40000000, 0x40200000) exists and covers the free frames
 * below the image; but nothing guarantees a later stack run lands in a region
 * that already has an L3. Creating one here would (1) memset a frame that has
 * no identity leaf yet -> translation fault with the MMU on, and (2) grow
 * table_pa[]/table_start/table_end, which the audit ledger treats as frozen
 * post-build. The runtime path therefore NEVER allocates a table: a missing
 * L0..L2 descriptor is JE_ERR_UNSUPPORTED with nothing written, and every
 * table it reads must be a build-time ledger member (each of which has its own
 * RW identity leaf), so a corrupt descriptor pointing at an unmapped or
 * foreign frame is JE_ERR_INVALID without being dereferenced.
 *
 * Ledger (hazard c). A runtime-mapped frame must have an EMPTY ledger entry
 * before the map (no leaf anywhere), so it is singly mapped and its entry is
 * set to class_flags | PRESENT | RUNTIME. unmap therefore owns the entry
 * outright and resets it to 0, so a later map of the same frame in another
 * class starts from a clean ledger (record_frame_flags only ORs, which would
 * otherwise make a later RX use of the frame look like a W+X alias). Static
 * leaves never carry RUNTIME, so unmap cannot remove kernel image, table or
 * device mappings.
 *
 * W^X. Only JE_MAP_RW / JE_MAP_RODATA are accepted; the encoded descriptor is
 * then compared against the audit's independent literal and its PXN and UXN
 * bits are required to be set before the store. With the empty-ledger rule no
 * other leaf (executable or not) can alias the frame.
 *
 * Ordering (hazard d). The leaf is written with one aligned 64-bit volatile
 * store (single-copy atomic for the walker), then leaf_changed_sync() runs the
 * barrier/TLBI sequence. Only invalid <-> valid transitions happen, so no
 * break-before-make step is needed. For unmap the ledger entry is cleared only
 * after the TLBI has completed (the translation is gone first).
 */
/*
 * Shared observability seam. Hosted tests count the existing runtime-map TLBI
 * maintenance, and MP3a-1 also records the ASIDE1IS operand on real ARM builds.
 * QEMU TCG does not model stale-TLB behavior; the maintenance sequence is
 * reviewed, not observed.
 */
volatile uint64_t je_arm_tlbi_hosted_calls;
volatile uint64_t je_arm_tlbi_hosted_last_operand;

/*
 * TLBI VAE1IS operand, Xt (Arm ARM DDI0487, "TLBI VAE1, TLBI VAE1IS, TLBI
 * VAE1ISNXS, TLBI VAE1NXS: TLB Invalidate by VA, EL1" in the A64
 * system-instruction chapter, C5.5 in DDI0487 K.a):
 *   Xt[63:48] ASID  = the caller-selected 16-bit ASID. Runtime kernel leaves
 *                     use ASID 0; process user leaves use their non-global
 *                     process ASID (currently ASID 1).
 *   Xt[47:44] TTL   = 0. "No information about the translation level" (the
 *                     FEAT_TTL hint is optional and not used).
 *   Xt[43:0]  VA[55:12]. va < 2^48, so this is va >> 12 in bits [35:0].
 * Semantics: invalidate cached stage-1 EL1&0 translations for that VA that come
 * from a LAST-level (leaf) entry, broadcast to the Inner Shareable domain. Upper
 * level walk caches are not invalidated, which is correct because the runtime
 * path never changes a table descriptor. The DSB ISHST before makes the
 * descriptor store visible to table walks before the invalidate; DSB ISH after
 * waits for the invalidate to complete; ISB makes later instructions use the
 * new translation (DDI0487 D8 "TLB maintenance requirements" / "Ordering and
 * completion of TLB maintenance instructions").
 */
static uint64_t tlbi_va_operand(uint64_t virtual_address, uint16_t asid)
{
    return ((uint64_t)asid << 48) |
           ((virtual_address >> JE_ARM_PAGE_SHIFT) &
           ((UINT64_C(1) << 44) - 1u));
}

static void leaf_changed_sync(uint64_t virtual_address, uint16_t asid)
{
    const uint64_t operand = tlbi_va_operand(virtual_address, asid);
#ifdef JE_ARM_PAGING_HOSTED_TEST
    ++je_arm_tlbi_hosted_calls;
    je_arm_tlbi_hosted_last_operand = operand;
#else
    __asm__ volatile("dsb ishst\n\t"
                     "tlbi vae1is, %0\n\t"
                     "dsb ish\n\t"
                     "isb"
                     : : "r"(operand) : "memory");
#endif
}

je_status je_arm_mmu_flush_va(uint64_t virtual_address)
{
    return je_arm_mmu_flush_va_asid(virtual_address, 0u);
}

je_status je_arm_mmu_flush_va_asid(uint64_t virtual_address, uint16_t asid)
{
    if (virtual_address >= JE_ARM_VA_LIMIT ||
        (virtual_address & (JE_PAGE_SIZE - 1u)) != 0u ||
        asid > UINT16_C(0xff))
        return JE_ERR_INVALID;
    leaf_changed_sync(virtual_address, asid);
    return JE_OK;
}

static bool runtime_context_ok(const je_arm_mmu *mmu)
{
    return mmu != NULL && mmu->valid && je_arm_enabled_context == mmu &&
           mmu->root_pa != 0u;
}

static bool page_aligned(uint64_t address)
{
    return (address & (JE_PAGE_SIZE - 1u)) == 0u;
}

static bool in_window(uint64_t physical)
{
    return physical >= JE_PHYSICAL_BASE && physical < JE_PHYSICAL_LIMIT;
}

static bool in_image(const je_arm_mmu *mmu, uint64_t address)
{
    return address >= mmu->image_start && address < mmu->image_end;
}

static size_t window_index(uint64_t physical)
{
    return (size_t)((physical - JE_PHYSICAL_BASE) / JE_PAGE_SIZE);
}

/* Locate the L3 slot for `va` WITHOUT allocating. JE_ERR_UNSUPPORTED: an
 * L0..L2 descriptor is invalid (no L3 table for this 2 MiB region).
 * JE_ERR_INVALID: a block descriptor, or a table that is not a ledger member
 * (never dereferenced). */
static je_status runtime_leaf_slot(const je_arm_mmu *mmu,
                                   uint64_t virtual_address, uint64_t **out_slot)
{
    *out_slot = NULL;
    uint64_t table_pa = mmu->root_pa;
    for (unsigned level = 0u; level < 3u; ++level) {
        if (!table_frame(mmu, table_pa)) return JE_ERR_INVALID;
        const uint64_t descriptor =
            table_pointer(table_pa)[va_index(virtual_address, level)];
        if ((descriptor & JE_ARM_DESC_VALID) == 0u) return JE_ERR_UNSUPPORTED;
        if ((descriptor & UINT64_C(3)) != JE_ARM_DESC_TABLE)
            return JE_ERR_INVALID;
        table_pa = descriptor & JE_ARM_DESC_ADDR_MASK;
    }
    if (!table_frame(mmu, table_pa)) return JE_ERR_INVALID;
    *out_slot = &table_pointer(table_pa)[va_index(virtual_address, 3u)];
    return JE_OK;
}

/* Shared by unmap and the stack reap pre-check: the slot holds a valid page
 * descriptor installed by the runtime path. Writes nothing. */
static je_status runtime_leaf_owned(const je_arm_mmu *mmu,
                                    uint64_t virtual_address,
                                    uint64_t **out_slot, size_t *out_index)
{
    uint64_t *slot = NULL;
    je_status status = runtime_leaf_slot(mmu, virtual_address, &slot);
    if (status == JE_ERR_UNSUPPORTED) return JE_ERR_INVALID; /* no L3: invalid */
    if (status != JE_OK) return status;
    const uint64_t descriptor = *slot;
    if ((descriptor & JE_ARM_DESC_VALID) == 0u) return JE_ERR_INVALID;
    if ((descriptor & UINT64_C(3)) != JE_ARM_DESC_TABLE) /* L3 0b01: not a page */
        return JE_ERR_INVALID;
    const uint64_t physical = descriptor & JE_ARM_DESC_ADDR_MASK;
    if (!in_window(physical)) return JE_ERR_DENIED;          /* static device */
    const size_t index = window_index(physical);
    if ((mmu->frame_flags[index] & JE_ARM_MAP_RUNTIME) == 0u)
        return JE_ERR_DENIED;                                /* static leaf */
    *out_slot = slot;
    *out_index = index;
    return JE_OK;
}

je_status je_arm_mmu_map_page(je_arm_mmu *mmu, uint64_t va, uint64_t pa,
                              je_map_class cls)
{
    if (!runtime_context_ok(mmu) || !class_valid(cls) ||
        !page_aligned(va) || !page_aligned(pa) || va >= JE_ARM_VA_LIMIT ||
        !in_window(pa))
        return JE_ERR_INVALID;
    if (cls != JE_MAP_RW && cls != JE_MAP_RODATA) return JE_ERR_DENIED;
    if (in_image(mmu, va) || in_image(mmu, pa)) return JE_ERR_DENIED;

    const uint64_t descriptor = leaf_descriptor(pa, cls);
    if (class_executable(cls) || !class_descriptor_matches(descriptor, cls) ||
        (descriptor & JE_ARM_DESC_PXN) == 0u ||
        (descriptor & JE_ARM_DESC_UXN) == 0u ||
        (descriptor & JE_ARM_DESC_ADDR_MASK) != pa)
        return JE_ERR_INVALID;

    uint64_t *slot = NULL;
    je_status status = runtime_leaf_slot(mmu, va, &slot);
    if (status != JE_OK) return status;
    if ((*slot & JE_ARM_DESC_VALID) != 0u) return JE_ERR_INVALID;

    const size_t index = window_index(pa);
    if (table_frame(mmu, pa) || mmu->frame_flags[index] != 0u)
        return JE_ERR_DENIED;          /* frame already has a leaf somewhere */

    mmu->frame_flags[index] = (uint8_t)(class_flags(cls) | JE_ARM_MAP_PRESENT |
                                        JE_ARM_MAP_RUNTIME);
    *(volatile uint64_t *)slot = descriptor;
    leaf_changed_sync(va, 0u);
    return JE_OK;
}

je_status je_arm_mmu_unmap_page(je_arm_mmu *mmu, uint64_t va)
{
    if (!runtime_context_ok(mmu) || !page_aligned(va) || va >= JE_ARM_VA_LIMIT)
        return JE_ERR_INVALID;
    uint64_t *slot = NULL;
    size_t index = 0u;
    je_status status = runtime_leaf_owned(mmu, va, &slot, &index);
    if (status != JE_OK) return status;
    *(volatile uint64_t *)slot = 0u;
    leaf_changed_sync(va, 0u);
    mmu->frame_flags[index] = 0u;      /* translation gone; frame unledgered */
    return JE_OK;
}

static bool stack_run_valid(uint64_t run_base)
{
    const uint64_t span = (uint64_t)JE_ARM_STACK_RUN_PAGES * JE_PAGE_SIZE;
    return page_aligned(run_base) && in_window(run_base) &&
           run_base <= JE_PHYSICAL_LIMIT - span;
}

/* JE_OK iff the guard VA has no valid leaf (a missing L3 counts as unmapped). */
static je_status stack_guard_unmapped(const je_arm_mmu *mmu, uint64_t guard)
{
    uint64_t *slot = NULL;
    je_status status = runtime_leaf_slot(mmu, guard, &slot);
    if (status == JE_ERR_UNSUPPORTED) return JE_OK;
    if (status != JE_OK) return status;
    return (*slot & JE_ARM_DESC_VALID) == 0u ? JE_OK : JE_ERR_INVALID;
}

je_status je_arm_mmu_stack_map(je_arm_mmu *mmu, uint64_t run_base,
                               uint64_t *out_top)
{
    if (out_top == NULL) return JE_ERR_INVALID;
    *out_top = 0u;
    if (!runtime_context_ok(mmu) || !stack_run_valid(run_base))
        return JE_ERR_INVALID;
    je_status status = stack_guard_unmapped(mmu, run_base);
    if (status != JE_OK) return status;
    for (uint64_t k = 0u; k < JE_ARM_STACK_PAGES; ++k) {
        const uint64_t va = run_base + (JE_ARM_STACK_GUARD_PAGES + k) * JE_PAGE_SIZE;
        status = je_arm_mmu_map_page(mmu, va, va, JE_MAP_RW);
        if (status == JE_OK) continue;
        /* All-or-nothing: undo the pages this call mapped, newest first. A
         * rollback failure is the more severe state, so it wins (as in
         * rollback_build). */
        while (k > 0u) {
            --k;
            const uint64_t undo_va =
                run_base + (JE_ARM_STACK_GUARD_PAGES + k) * JE_PAGE_SIZE;
            je_status undo = je_arm_mmu_unmap_page(mmu, undo_va);
            if (undo != JE_OK) status = undo;
        }
        return status;
    }
    *out_top = run_base + (uint64_t)JE_ARM_STACK_RUN_PAGES * JE_PAGE_SIZE;
    return JE_OK;
}

je_status je_arm_mmu_stack_unmap(je_arm_mmu *mmu, uint64_t run_base)
{
    if (!runtime_context_ok(mmu) || !stack_run_valid(run_base))
        return JE_ERR_INVALID;
    je_status status = stack_guard_unmapped(mmu, run_base);
    if (status != JE_OK) return status;
    /* Pre-check every stack leaf before changing any (no partial reap). */
    for (uint64_t k = 0u; k < JE_ARM_STACK_PAGES; ++k) {
        const uint64_t va = run_base + (JE_ARM_STACK_GUARD_PAGES + k) * JE_PAGE_SIZE;
        uint64_t *slot = NULL;
        size_t index = 0u;
        status = runtime_leaf_owned(mmu, va, &slot, &index);
        if (status != JE_OK) return status;
        if ((*slot & JE_ARM_DESC_ADDR_MASK) != va) return JE_ERR_INVALID;
    }
    for (uint64_t k = 0u; k < JE_ARM_STACK_PAGES; ++k) {
        const uint64_t va = run_base + (JE_ARM_STACK_GUARD_PAGES + k) * JE_PAGE_SIZE;
        status = je_arm_mmu_unmap_page(mmu, va);
        if (status != JE_OK) return status;   /* unreachable after pre-check */
    }
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

uint64_t je_arm_at_s1e0(uint64_t virtual_address, int write)
{
    uint64_t par = 0u;
    if (write != 0) {
        __asm__ volatile("at s1e0w, %1\n\t"
                         "isb\n\t"
                         "mrs %0, par_el1"
                         : "=r"(par) : "r"(virtual_address) : "memory");
    } else {
        __asm__ volatile("at s1e0r, %1\n\t"
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
