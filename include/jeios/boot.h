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
#define JE_PAGE_SIZE UINT64_C(4096)

/*
 * Physical allocator window (ADR 0011, owner-approved tested-path change,
 * 2026-11-03). The allocator manages the half-open physical range
 * [JE_PHYSICAL_BASE, JE_PHYSICAL_LIMIT); page index i maps to physical
 * JE_PHYSICAL_BASE + i * JE_PAGE_SIZE. Both bounds are per-arch compile-time
 * constants so one allocator serves both targets without a fork:
 *   - x86-64: base 0, limit 0x40000000 — the alloc/free/heap code is IDENTICAL
 *     at -O2 to the pre-ADR-0011 build (base 0 => index*PAGE_SIZE as before), so
 *     the existing M1/M2 allocator tests exercise unchanged behavior. (The TU
 *     gains two new symbols, je_pages_init_pred + the x86 predicate, so the
 *     object is behavior-identical, not byte-identical.)
 *   - aarch64 (QEMU virt-8.2): base 0x40000000, limit 0x50000000 (256 MiB RAM).
 * The window MUST satisfy base < limit, both page-aligned, and
 * (limit - base) a multiple of JE_PAGE_SIZE; a static assertion in alloc.c
 * enforces this. JE_PHYSICAL_SPAN is the managed byte span; JE_PAGE_COUNT (in
 * alloc.h) is JE_PHYSICAL_SPAN / JE_PAGE_SIZE — i.e. the window SIZE, not the
 * absolute limit, so the state[] array sizes to the window, not to the address.
 *
 * The window is selected by JE_ARCH_AARCH64 (set by mk/aarch64.mk for every ARM
 * object). Compiling an aarch64 translation unit WITHOUT that macro would
 * silently pick the x86 window and a different JE_PAGE_COUNT, giving an ODR/ABI
 * mismatch across objects — so guard it: an aarch64 target without the macro is
 * a hard error, not a silent fallback.
 */
#if defined(__aarch64__) && !defined(JE_ARCH_AARCH64)
#error "aarch64 target must define JE_ARCH_AARCH64 (see mk/aarch64.mk) so the allocator window matches across all objects"
#endif
#if defined(JE_ARCH_AARCH64)
#define JE_PHYSICAL_BASE  UINT64_C(0x40000000)
#define JE_PHYSICAL_LIMIT UINT64_C(0x50000000)
#else
#define JE_PHYSICAL_BASE  UINT64_C(0x00000000)
#define JE_PHYSICAL_LIMIT UINT64_C(0x40000000)
#endif
#define JE_PHYSICAL_SPAN  (JE_PHYSICAL_LIMIT - JE_PHYSICAL_BASE)
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

