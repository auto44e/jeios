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

/* Object type values carried in a slot. Type 0 is reserved/invalid (a zeroed
 * slot never reads as a valid type). JE_HANDLE_OBJ_SELF is the M3b identity
 * capability — the ONLY type that may be duplicated via je_handle_dup_self,
 * because it owns no external resource and is never destroyed. It MUST equal the
 * ABI's JE_ABI_OBJ_SELF; a syscall-layer _Static_assert pins the two together.
 * (The table defines its own copy so handle.c has no ABI include.) */
#define JE_HANDLE_OBJ_SELF 1u

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

/* Attenuate (M3b final PR, ADR 0008 amendment): derive a NEW handle to the same
 * object as `src`, with a rights SUBSET. Fresh-slot semantics: the new handle
 * gets its own slot, index, generation and close, and counts against quota — it
 * is a genuine second name for the object, not a re-labelling of src's slot.
 *
 * SELF-ONLY by contract: `expected_type` must be the SELF type. The table copies
 * the object binding from src's slot into the new slot WITHOUT any retain/refcount
 * step, which is sound ONLY because SELF owns no external resource and is never
 * destroyed. A destroyable object type (M3c endpoints) MUST NOT be duplicated via
 * this path — M3c adds a retain-before-commit derive for those.
 *
 * Rights rule: `new_rights` must be a subset of src's rights
 * (new_rights & ~src.rights == 0); a request for any right src lacks returns
 * DENIED (never a clipped/weaker grant). Equality is allowed (a pure alias).
 * `new_rights` must contain only known bits — the CALLER (syscall glue) is
 * responsible for rejecting unknown bits as INVALID before calling this.
 *
 * Order: resolve src (BADINDEX -> STALE -> WRONGTYPE), then the subset check
 * (DENIED), then allocate the new slot (QUOTA before NOSLOTS). On any failure
 * *out is 0 and the table is unchanged. `out` is REQUIRED. A closed src cannot
 * be duplicated (STALE); an already-derived handle survives src's close (its own
 * slot). */
je_handle_status je_handle_dup_self(je_handle_table *t, je_handle src,
                                    uint32_t expected_type, uint32_t new_rights,
                                    je_handle *out);

/* Close a handle: LIVE -> FREE, bump generation; if the bump would exceed
 * JE_HANDLE_GEN_MAX the slot goes RETIRED instead (never wraps). Closing a
 * stale/free/retired handle returns STALE and changes nothing. */
je_handle_status je_handle_close(je_handle_table *t, je_handle h);

/* Read-only slot enumerator (M3c): if slot `index` is LIVE, write its current
 * handle (encode of index+generation) to *out_handle and its object type to
 * *out_type, and return true; otherwise return false and write nothing. Never
 * mutates the table and never exposes the object pointer. This lets an
 * object-aware layer (e.g. the endpoint release walk + the typed-close
 * dispatcher) find and re-address LIVE slots of its type WITHOUT reaching into
 * `slots` directly, preserving table purity. `index` >= capacity returns false.
 * Both out params are REQUIRED. */
/* ADR 0010 D3 typed dispatch: resolve a handle of ANY type. Same precedence as
 * lookup minus the type and rights checks. A NULL t or out pointer is a caller bug and
 * is reported as BADINDEX (the pure enum has no INVALID). Otherwise: index out of
 * range -> BADINDEX; slot not LIVE or generation mismatch -> STALE. On OK, writes the
 * slot's type, rights and object. On failure the outputs are left unchanged. Pure,
 * read-only. */
je_handle_status je_handle_resolve_any(const je_handle_table *t, je_handle h,
                                       uint32_t *type, uint32_t *rights, void **obj);

bool je_handle_live_at(const je_handle_table *t, uint32_t index,
                       je_handle *out_handle, uint32_t *out_type);

#endif /* JEIOS_HANDLE_H */
