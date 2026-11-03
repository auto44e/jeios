/* SPDX-License-Identifier: Apache-2.0 */
#include "physmem.h"

static bool range_contains_page(je_range range, uint64_t phys)
{
    uint64_t page_end = phys + JE_PAGE_SIZE;
    return range.base <= phys && page_end <= range.base + range.size;
}

static bool range_intersects_page(je_range range, uint64_t phys)
{
    uint64_t page_end = phys + JE_PAGE_SIZE;
    /* A range whose base+size wraps past 2^64 would make the half-open end
     * compare below the base and the intersection test fail OPEN (reporting a
     * reserved page as free). The FDT parser already rejects overflowing ranges
     * (BAD_RANGE) and je_boot_ctx_check validates the facts, so this is
     * unreachable in the live boot; but the predicate is documented as total and
     * safe to reuse, so treat an overflowing reserved range as covering
     * everything from its base up (fail SAFE). */
    if (range.size > UINT64_MAX - range.base)
        return phys >= range.base || page_end > range.base;
    uint64_t range_end = range.base + range.size;
    return phys < range_end && range.base < page_end;
}

bool je_arm_page_free(const void *view_ptr, uint64_t phys)
{
    const je_arm_physmem_view *view = view_ptr;
    if (!view || !view->ctx) return false;
    /* Default-deny on any phys whose page end would wrap u64: range_contains_page
     * and range_intersects_page both compute phys+JE_PAGE_SIZE, so reject the top
     * page of the address space outright. In-window pages (phys < 0x50000000)
     * never reach this, but keeping the predicate total and overflow-free lets it
     * be reused safely outside the current window. */
    if (phys > UINT64_MAX - JE_PAGE_SIZE) return false;
    if (view->ctx->ram_count > JE_FDT_MAX_RAM ||
        view->ctx->rsv_count > JE_FDT_MAX_RSV) return false;

    uint32_t ram_matches = 0u;
    for (uint32_t i = 0u; i < view->ctx->ram_count; ++i) {
        if (range_contains_page(view->ctx->ram[i], phys)) ++ram_matches;
    }
    if (ram_matches != 1u) return false;

    if (range_intersects_page(view->kernel, phys) ||
        range_intersects_page(view->dtb, phys)) return false;
    if (view->ctx->has_initrd &&
        range_intersects_page(view->ctx->initrd, phys)) return false;
    for (uint32_t i = 0u; i < view->ctx->rsv_count; ++i) {
        if (range_intersects_page(view->ctx->rsv[i], phys)) return false;
    }
    return true;
}

je_status je_arm_physmem_init(je_pages *pages, const je_arm_physmem_view *view)
{
    if (!pages || !view || !view->ctx) return JE_ERR_INVALID;
    return je_pages_init_pred(pages, je_arm_page_free, view);
}
