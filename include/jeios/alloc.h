#ifndef JEIOS_ALLOC_H
#define JEIOS_ALLOC_H
#include <jeios/boot.h>
#define JE_PAGE_COUNT (JE_PHYSICAL_LIMIT/JE_PAGE_SIZE)
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
