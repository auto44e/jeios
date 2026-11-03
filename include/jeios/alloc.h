/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ALLOC_H
#define JEIOS_ALLOC_H
#include <jeios/boot.h>
/* Page index space covers the WINDOW [JE_PHYSICAL_BASE, JE_PHYSICAL_LIMIT):
 * index i <-> physical JE_PHYSICAL_BASE + i*JE_PAGE_SIZE. Sizing state[] to the
 * span (not the absolute limit) keeps the x86 footprint unchanged (base 0) and
 * bounds the ARM array to its 256 MiB window rather than its 0x50000000 top. */
#define JE_PAGE_COUNT (JE_PHYSICAL_SPAN/JE_PAGE_SIZE)
#define JE_ALLOC_MAX_PAGES 256u
#define JE_HEAP_SLOTS 64u
typedef struct {
    bool ready;
    size_t free_count;
    uint16_t state[JE_PAGE_COUNT];
} je_pages;
/* All operations require external serialization; kernel wrappers disable BSP
 * interrupts. No SMP, no allocation in handlers, no ability to free reserved RAM.
 * Free validates exact head+length before mutation. Addresses are physical. */
je_status je_pages_init(je_pages *pages, const je_boot_context *boot);
je_status je_pages_alloc(je_pages *pages, size_t count, uint64_t *physical);
je_status je_pages_free(je_pages *pages, uint64_t physical, size_t count);

/* Default-deny free-page predicate (ADR 0011 D5). Returns true iff the page at
 * absolute physical `phys` (always a window-aligned page base) is allocatable.
 * It is called once per managed page during je_pages_init_pred; it must have no
 * side effects and must answer purely from `ctx`. Any page it does not approve
 * becomes RESERVED, so a predicate that cannot classify a page MUST return false
 * (never allocate on doubt). */
typedef bool (*je_page_free_pred)(const void *ctx, uint64_t phys);
je_status je_pages_init_pred(je_pages *pages, je_page_free_pred is_free, const void *ctx);
typedef void *(*je_map_ram)(uint64_t physical, size_t bytes, void *context);
typedef struct { void *pointer; uint64_t physical; size_t pages; } je_heap_record;
typedef struct {
    je_pages *pages; je_map_ram map; void *context;
    je_heap_record records[JE_HEAP_SLOTS];
} je_heap;
/* Bootstrap page-granular heap, 64 concurrent allocations, <=1 MiB each.
 * Mapper must return stable writable RAM or NULL, with no side effects on NULL.
 * Caller owns records and serialization; initialize a fresh heap only once.
 * Successful allocations are fully zeroed, page-aligned physical extents.
 * Returned virtual pointer must be at least 16-byte aligned. */
je_status je_heap_init(je_heap *heap, je_pages *pages, je_map_ram map, void *context);
je_status je_heap_alloc(je_heap *heap, size_t bytes, void **result);
je_status je_heap_free(je_heap *heap, void *pointer);
#endif

