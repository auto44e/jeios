/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3b first PR (ADR 0008): the pure, process-local, typed handle table.
 * Standalone — no syscalls, no object destruction, no M3a-tested path touched.
 * Allocates nothing itself (caller supplies slot storage). Sound only under the
 * ADR 0008 D6 uniprocessor / IF=0 precondition; this PR makes no SMP claim and
 * has no runtime (it is exercised by the hosted test and later wired in).
 */
#include <jeios/handle.h>

/* Production encoding is pinned exactly 16/16 (ADR 0008 D2/D5); a reduced width
 * is permitted ONLY in the hosted test build, which predefines the bits and
 * JE_HANDLE_TEST_WIDTH. A careless production edit therefore fails to compile. */
_Static_assert(JE_HANDLE_INDEX_BITS + JE_HANDLE_GEN_BITS == 32u,
               "handle index+generation bits must sum to 32");
/* Both fields must be non-degenerate: a 0-width field would make `1u << 32`
 * undefined and (for a 0-width generation) make the start-at-1 invariant and the
 * zero-is-invalid guarantee impossible. This holds for the test widths too. */
_Static_assert(JE_HANDLE_INDEX_BITS >= 1u && JE_HANDLE_INDEX_BITS <= 31u,
               "handle index bits must be in [1,31]");
_Static_assert(JE_HANDLE_GEN_BITS >= 1u && JE_HANDLE_GEN_BITS <= 31u,
               "handle generation bits must be in [1,31]");
#ifndef JE_HANDLE_TEST_WIDTH
_Static_assert(JE_HANDLE_INDEX_BITS == 16u && JE_HANDLE_GEN_BITS == 16u,
               "production handle width is pinned at 16 index / 16 generation");
#endif

je_handle je_handle_encode(uint32_t index, uint32_t generation)
{
    return (je_handle)(((generation & JE_HANDLE_GEN_MASK) << JE_HANDLE_INDEX_BITS)
                       | (index & JE_HANDLE_INDEX_MASK));
}

void je_handle_decode(je_handle h, uint32_t *index, uint32_t *generation)
{
    if (index)      *index      = (uint32_t)(h & JE_HANDLE_INDEX_MASK);
    if (generation) *generation = (uint32_t)((h >> JE_HANDLE_INDEX_BITS)
                                             & JE_HANDLE_GEN_MASK);
}

bool je_handle_is_wellformed(je_handle h)
{
    /* Nonzero (0 is the invalid null) and no bits above the 32-bit field. Since
     * je_handle is uint32_t the upper-bits check is structural; the syscall
     * boundary (later PR) additionally rejects a 64-bit arg with high bits set
     * before narrowing to je_handle. A nonzero value always has either a nonzero
     * index or a nonzero generation, so generation-starts-at-1 guarantees every
     * live handle is well-formed. */
    return h != 0u;
}

bool je_handle_table_init(je_handle_table *t, je_handle_slot *slots,
                          uint32_t capacity, uint32_t quota)
{
    if (!t || !slots) return false;
    /* capacity must fit the index space; a full (1<<INDEX_BITS) capacity would
     * make index == capacity unrepresentable only at the very top, but since
     * lookup rejects index >= capacity and allocation never emits capacity, a
     * capacity up to the whole index space is safe. Reject above it. */
    uint64_t index_space = (uint64_t)1u << JE_HANDLE_INDEX_BITS;
    if (capacity == 0u || (uint64_t)capacity > index_space) return false;
    if (quota > capacity) return false;
    t->slots = slots;
    t->capacity = capacity;
    t->quota = quota;
    t->live = 0u;
    for (uint32_t i = 0; i < capacity; ++i) {
        slots[i].state = JE_HSLOT_FREE;
        slots[i].generation = 1u;   /* never 0: handle value 0 stays invalid */
        slots[i].object = (void *)0;
        slots[i].type = 0u;
        slots[i].rights = 0u;
    }
    return true;
}

je_handle_status je_handle_alloc(je_handle_table *t, void *object,
                                 uint32_t type, uint32_t rights, je_handle *out)
{
    if (!out) return JE_HANDLE_BADINDEX;   /* out is required: never mint a
                                            * handle the caller cannot receive
                                            * (that would leak a LIVE slot). */
    *out = 0u;
    if (!t) return JE_HANDLE_BADINDEX;
    /* QUOTA takes precedence over NOSLOTS when both could apply (ADR 0008 D8). */
    if (t->live >= t->quota) return JE_HANDLE_QUOTA;
    for (uint32_t i = 0; i < t->capacity; ++i) {       /* lowest-FREE-index */
        if (t->slots[i].state == JE_HSLOT_FREE) {
            t->slots[i].state = JE_HSLOT_LIVE;
            t->slots[i].object = object;
            t->slots[i].type = type;
            t->slots[i].rights = rights;
            t->live++;
            *out = je_handle_encode(i, t->slots[i].generation);
            return JE_HANDLE_OK;
        }
    }
    return JE_HANDLE_NOSLOTS;    /* all slots LIVE or RETIRED */
}

/* Shared slot resolver for lookup and rights_of, so the two can never drift in
 * their validity precedence (BADINDEX -> STALE -> WRONGTYPE -> DENIED). On OK,
 * *out_slot points at the live slot; on any failure returns the specific status
 * and writes nothing. Pure; never mutates the table. */
static je_handle_status resolve_slot(const je_handle_table *t, je_handle h,
                                     uint32_t expected_type, uint32_t required_rights,
                                     const je_handle_slot **out_slot)
{
    uint32_t index, generation;
    if (!t) return JE_HANDLE_BADINDEX;
    if (!je_handle_is_wellformed(h)) return JE_HANDLE_BADINDEX;
    je_handle_decode(h, &index, &generation);
    if (index >= t->capacity) return JE_HANDLE_BADINDEX;
    const je_handle_slot *s = &t->slots[index];
    /* State first: a FREE or RETIRED slot is STALE regardless of generation. */
    if (s->state != JE_HSLOT_LIVE) return JE_HANDLE_STALE;
    if (s->generation != generation) return JE_HANDLE_STALE;
    if (s->type != expected_type) return JE_HANDLE_WRONGTYPE;
    if ((s->rights & required_rights) != required_rights) return JE_HANDLE_DENIED;
    *out_slot = s;
    return JE_HANDLE_OK;
}

je_handle_status je_handle_lookup(const je_handle_table *t, je_handle h,
                                  uint32_t expected_type, uint32_t required_rights,
                                  void **out_object)
{
    const je_handle_slot *s;
    je_handle_status r = resolve_slot(t, h, expected_type, required_rights, &s);
    if (r != JE_HANDLE_OK) return r;
    if (out_object) *out_object = s->object;
    return JE_HANDLE_OK;
}

je_handle_status je_handle_rights_of(const je_handle_table *t, je_handle h,
                                     uint32_t expected_type, uint32_t required_rights,
                                     uint32_t *out_rights)
{
    const je_handle_slot *s;
    je_handle_status r = resolve_slot(t, h, expected_type, required_rights, &s);
    if (r != JE_HANDLE_OK) return r;
    if (out_rights) *out_rights = s->rights;
    return JE_HANDLE_OK;
}

je_handle_status je_handle_dup_self(je_handle_table *t, je_handle src,
                                    uint32_t expected_type, uint32_t new_rights,
                                    je_handle *out)
{
    if (!out) return JE_HANDLE_BADINDEX;
    *out = 0u;
    /* SELF-ONLY by construction, not merely by what the caller passes. This path
     * copies the object binding with NO retain/refcount, which is sound only for
     * SELF (owns nothing, never destroyed). A destroyable type reaching here
     * would be a latent UAF, so reject anything but SELF outright — even if a
     * caller asks for a different expected_type. M3c adds a retain-based derive
     * for destroyable objects; it must NOT reuse this function. */
    if (expected_type != JE_HANDLE_OBJ_SELF) return JE_HANDLE_WRONGTYPE;
    /* Resolve the source as SELF with NO required rights (any holder may
     * attenuate); the subset check below enforces "only remove rights". This
     * also returns WRONGTYPE if the named slot is not actually a SELF object. */
    const je_handle_slot *s;
    je_handle_status r = resolve_slot(t, src, JE_HANDLE_OBJ_SELF, 0u, &s);
    if (r != JE_HANDLE_OK) return r;
    /* Subset: the derived handle may not hold a right the source lacks. */
    if ((new_rights & ~s->rights) != 0u) return JE_HANDLE_DENIED;
    /* Fresh slot bound to the same object; copy the binding (SELF owns nothing,
     * so no retain is needed). Read the fields before allocate (which never
     * touches this LIVE slot, but capture first regardless). */
    void *object = s->object;
    uint32_t type = s->type;
    return je_handle_alloc(t, object, type, new_rights, out);
}

je_handle_status je_handle_close(je_handle_table *t, je_handle h)
{
    uint32_t index, generation;
    if (!t) return JE_HANDLE_BADINDEX;
    if (!je_handle_is_wellformed(h)) return JE_HANDLE_BADINDEX;
    je_handle_decode(h, &index, &generation);
    if (index >= t->capacity) return JE_HANDLE_BADINDEX;
    je_handle_slot *s = &t->slots[index];
    if (s->state != JE_HSLOT_LIVE || s->generation != generation)
        return JE_HANDLE_STALE;
    /* LIVE -> FREE, bump generation; saturate into RETIRED rather than wrap. */
    s->object = (void *)0;
    s->type = 0u;
    s->rights = 0u;
    if (s->generation >= JE_HANDLE_GEN_MAX) {
        s->state = JE_HSLOT_RETIRED;   /* permanently unusable; capacity lost */
    } else {
        s->generation++;
        s->state = JE_HSLOT_FREE;
    }
    t->live--;
    return JE_HANDLE_OK;
}
