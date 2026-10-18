/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_HANDLE_H
#define JEIOS_HANDLE_H
/*
 * M3b first PR (ADR 0008): a pure, process-local, bounded, typed handle table.
 * This is the standalone slot-table core only — NO syscalls, NO object
 * destruction, NO change to any M3a-tested path. It is hostable so `make check`
 * unit-tests every edge (forged / stale / wrong-type / denied / saturation /
 * retirement / quota) with no kernel state.
 *
 * A handle is stale-name protection, NOT an unforgeable capability (ADR 0008
 * D1): it is only ever minted by the kernel into the owning table and never
 * transferred raw between tables. Within one table, presenting one of your own
 * live handles grants nothing you were not already granted.
 *
 * Encoding (ADR 0008 D2), a frozen ABI once a syscall takes a handle:
 *   - 32-bit value: low JE_HANDLE_INDEX_BITS = slot index,
 *     high JE_HANDLE_GEN_BITS = slot generation; the two sum to 32.
 *   - generations start at 1 (never 0), so handle value 0 is a natural invalid
 *     null and allocation never returns 0.
 *   - the width is a COMPILE-TIME parameter: production is pinned 16/16; a
 *     reduced width (set by the hosted test) makes saturation/retirement
 *     exhaustively testable. Production pins are static-asserted in handle.c.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Compile-time width. The hosted test may predefine these (reduced) before
 * including this header; otherwise the production 16/16 split applies. */
#ifndef JE_HANDLE_INDEX_BITS
#define JE_HANDLE_INDEX_BITS 16u
#endif
#ifndef JE_HANDLE_GEN_BITS
#define JE_HANDLE_GEN_BITS 16u
#endif

typedef uint32_t je_handle;          /* the ABI handle value (0 == invalid) */

#define JE_HANDLE_INDEX_MASK ((1u << JE_HANDLE_INDEX_BITS) - 1u)
#define JE_HANDLE_GEN_MASK   ((1u << JE_HANDLE_GEN_BITS) - 1u)
#define JE_HANDLE_GEN_MAX    JE_HANDLE_GEN_MASK      /* saturation point */

/* Status (ADR 0008 D8). These names map to the v0 ABI status namespace at the
 * syscall boundary in a later PR; they never carry internal je_status numbers.
 * Kept as a distinct enum here so the pure table has no ABI coupling yet. */
typedef enum {
    JE_HANDLE_OK = 0,
    JE_HANDLE_BADINDEX,    /* handle 0, upper bits set, or index >= capacity */
    JE_HANDLE_STALE,       /* slot FREE/RETIRED, or generation mismatch */
    JE_HANDLE_WRONGTYPE,   /* live slot, wrong object type */
    JE_HANDLE_DENIED,      /* live slot, insufficient rights */
    JE_HANDLE_QUOTA,       /* allocate: live-slot count at the process quota */
    JE_HANDLE_NOSLOTS      /* allocate: no FREE slot remains (all LIVE/RETIRED) */
} je_handle_status;

typedef enum {
    JE_HSLOT_FREE = 0,
    JE_HSLOT_LIVE,
    JE_HSLOT_RETIRED
} je_handle_slot_state;

/* A bounded, typed, rights-bearing object binding. The object is an opaque
 * pointer in this PR (real kernel objects arrive with later PRs); the table
 * never dereferences it. */
typedef struct {
    je_handle_slot_state state;
    uint32_t             generation;  /* current; starts at 1, saturates */
    void                *object;      /* bound object (opaque here) */
    uint32_t             type;        /* object type tag */
    uint32_t             rights;      /* rights bitmask */
} je_handle_slot;

/* The process-local table. Fixed capacity; caller supplies slot storage so the
 * table itself allocates nothing. `quota` caps the number of LIVE slots. */
typedef struct {
    je_handle_slot *slots;
    uint32_t        capacity;   /* number of slots in `slots` (<= index space) */
    uint32_t        quota;      /* max LIVE slots (<= capacity) */
    uint32_t        live;       /* current LIVE count (accounting) */
} je_handle_table;

/* Pure encode/decode/validity (no table needed). */
je_handle je_handle_encode(uint32_t index, uint32_t generation);
void      je_handle_decode(je_handle h, uint32_t *index, uint32_t *generation);
bool      je_handle_is_wellformed(je_handle h); /* nonzero, upper bits clear */

/* Initialize a table over caller-provided slot storage. Zeroes accounting and
 * sets every slot FREE with generation 1. capacity must be in [1, 1<<INDEX_BITS]
 * and quota <= capacity, else returns false. MUST be called exactly once per
 * table lifetime: re-initializing a table whose old handles may still be
 * presented resets generations to 1 and revives RETIRED slots, which would make
 * those old handles match again. A quota of 0 is accepted and means every
 * allocate fails with QUOTA (a deliberately disabled table). */
bool je_handle_table_init(je_handle_table *t, je_handle_slot *slots,
                          uint32_t capacity, uint32_t quota);

/* Allocate the lowest FREE slot, bind {object,type,rights}, return its handle.
 * `out` is REQUIRED (non-NULL): a NULL out returns BADINDEX and allocates
 * nothing, so a handle is never minted into a slot the caller cannot receive.
 * Fails QUOTA (live == quota) or NOSLOTS (no FREE slot). On any failure *out is
 * set to 0 and the table is unchanged. */
je_handle_status je_handle_alloc(je_handle_table *t, void *object,
                                 uint32_t type, uint32_t rights,
                                 je_handle *out);

/* Look up a handle. Succeeds only if well-formed, index < capacity, slot LIVE,
 * generation matches, type == expected_type, and (slot.rights & required) ==
 * required. On success writes *out_object (may be NULL object) and returns OK;
 * on any failure returns the specific status and writes nothing. Never mutates
 * the table. expected_type/required_rights come from the caller, never a user. */
je_handle_status je_handle_lookup(const je_handle_table *t, je_handle h,
                                  uint32_t expected_type, uint32_t required_rights,
                                  void **out_object);

/* Read a live handle's rights mask. Same validity checks as lookup (well-formed,
 * index in range, slot LIVE, generation match, type == expected_type) AND the
 * slot must hold `required_rights`; on success writes the slot's full rights
 * mask to *out_rights and returns OK, else the specific status and nothing.
 * Pure; never mutates the table. (Added for the M3b rights-introspection op.) */
je_handle_status je_handle_rights_of(const je_handle_table *t, je_handle h,
                                     uint32_t expected_type, uint32_t required_rights,
                                     uint32_t *out_rights);

/* Close a handle: LIVE -> FREE, bump generation; if the bump would exceed
 * JE_HANDLE_GEN_MAX the slot goes RETIRED instead (never wraps). Closing a
 * stale/free/retired handle returns STALE and changes nothing. */
je_handle_status je_handle_close(je_handle_table *t, je_handle h);

#endif /* JEIOS_HANDLE_H */
