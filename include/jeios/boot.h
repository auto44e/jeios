/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_BOOT_H
#define JEIOS_BOOT_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <jeios/status.h>
#define JE_BOOT_MAP_MAX 128u
#define JE_BOOT_RESERVED_MAX 40u
#define JE_BOOT_BYTES_MAX (1024u * 1024u)
#define JE_PHYSICAL_LIMIT UINT64_C(0x40000000)
#define JE_PAGE_SIZE UINT64_C(4096)
typedef struct { uint64_t base, length; uint32_t type; } je_memory_range;
typedef struct { uint64_t begin, end; } je_reservation;
typedef struct {
    bool valid;
    size_t map_count, reserved_count;
    uint32_t boot_bytes;
    je_memory_range map[JE_BOOT_MAP_MAX];
    je_reservation reserved[JE_BOOT_RESERVED_MAX];
} je_boot_context;
/* Input must already be accessible for `accessible` bytes; neither output nor
 * input may alias. No retained input pointers; output invalid on any failure.
 * Reject ambiguous overlapping maps, unsupported versions and fixed-capacity
 * overflow. Kernel and loader data are reserved; only type-1 RAM is usable. */
je_status je_boot_parse(const void *data, size_t accessible, uint64_t physical,
                        uint64_t kernel_begin, uint64_t kernel_end,
                        je_boot_context *out);
bool je_boot_ram_page(const je_boot_context *ctx, uint64_t address);
bool je_boot_free_page(const je_boot_context *ctx, uint64_t address);
#endif

