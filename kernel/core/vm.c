/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3a issue #3: per-process address-space builder and bounded image loader.
 * HOSTED-ONLY proof (ADR 0007): models via the injected je_vm_ops backend; no
 * real paging. The builder owns admission, the build journal, D3 publication
 * order, and reverse-order teardown. Critical profile (JCS rules): bounded
 * loops, explicit ownership/rollback, validated inputs, no unbounded allocation.
 */
#include <jeios/vm.h>

/* user_demo.S encodes JE_VM_HANDLE_CAP (index 40, BADINDEX) and CAP-1 (index 39,
 * in-range free slot, STALE) as assembly-only boundary probes.
 * Keep the literal coupled to the frozen production capacity rather than
 * allowing the ring-3 test to silently drift if the C header changes. */
_Static_assert(JE_VM_HANDLE_CAP == 40u, "user demo handle-capacity probe");

/* Builder-side _Noreturn panic wrapper over the backend hook (the header hook
 * cannot be _Noreturn as a struct member). A returning hook is itself a fatal
 * contract violation, so we loop. */
static _Noreturn void je_vm_panic(const je_vm *vm, const char *why)
{
    vm->ops->panic(vm->ops->context, why);
    for (;;) { }
}

/* Worst-case journal size is bounded and provably below JE_VM_MAX_JOURNAL, so a
 * push can never legitimately overflow; an overflow is an invariant violation,
 * not a runtime condition. This lets every fallible op journal its mutation
 * without a "push failed after the mutation succeeded" hole.
 *   1 HOLD + JE_VM_MAX_EXTENTS extents + 1 TABLES
 *   + JE_VM_MAX_USER_PAGES PTE + JE_VM_MAX_USER_PAGES DEMOTE  (MAP is popped) */
_Static_assert(1u + JE_VM_MAX_EXTENTS + 1u
               + JE_VM_MAX_USER_PAGES + JE_VM_MAX_USER_PAGES
               <= JE_VM_MAX_JOURNAL, "journal worst case fits");

/* ---- small helpers -------------------------------------------------------- */

static je_vm_kind perm_to_kind(uint32_t perm, je_vm_perm *leaf)
{
    /* Legal: R|X -> CODE (RX), R -> RODATA (R/NX), R|W -> DATA (RW/NX). */
    switch (perm & JE_VM_PERM_BITS) {
    case (JE_VM_R | JE_VM_X): *leaf = JE_VM_PERM_RX;    return JE_VM_USER_CODE;
    case  JE_VM_R:            *leaf = JE_VM_PERM_R_NX;  return JE_VM_USER_RODATA;
    case (JE_VM_R | JE_VM_W): *leaf = JE_VM_PERM_RW_NX; return JE_VM_USER_DATA;
    default:                  *leaf = JE_VM_PERM_RX;    return (je_vm_kind)0;
    }
}

static bool kind_is_nonwritable(je_vm_kind k)
{
    return k == JE_VM_USER_CODE || k == JE_VM_USER_RODATA;
}

/* ---- image validation (pure; ADR D8) -------------------------------------- */

je_status je_vm_image_validate(const je_vm_image *image, size_t blob_len)
{
    if (!image) return JE_ERR_INVALID;
    if (image->version != JE_VM_IMAGE_VERSION) return JE_ERR_UNSUPPORTED;
    if (image->reserved != 0) return JE_ERR_INVALID;
    if (image->segment_count == 0 || image->segment_count > JE_VM_MAX_SEGMENTS)
        return JE_ERR_INVALID;

    /* Per-segment validation; collect page ranges for overlap and stack/guard. */
    uint64_t total_user_pages = JE_VM_STACK_PAGES; /* stack always present */
    bool entry_ok = false;
    /* page-presence bitmap over the arena (512 pages) for page-granule overlap
     * and stack/guard exclusion. */
    uint8_t used[JE_VM_ARENA_PAGES];
    for (size_t i = 0; i < JE_VM_ARENA_PAGES; ++i) used[i] = 0;
    /* mark stack + guard pages as reserved so segments cannot touch them */
    for (uint64_t p = JE_VM_STACK_OFFSET / JE_PAGE_SIZE; p < JE_VM_ARENA_PAGES; ++p)
        used[p] = 1;
    used[JE_VM_GUARD_OFFSET / JE_PAGE_SIZE] = 1;

    for (uint32_t s = 0; s < image->segment_count; ++s) {
        je_vm_segment seg = image->segments[s];
        if (seg.reserved != 0) return JE_ERR_INVALID;

        /* permission legality + kind */
        if (seg.perm == 0 || (seg.perm & ~JE_VM_PERM_BITS)) return JE_ERR_INVALID;
        je_vm_perm leaf;
        je_vm_kind kind = perm_to_kind(seg.perm, &leaf);
        if (kind == (je_vm_kind)0) return JE_ERR_INVALID; /* W|X, X-only, W-only */

        /* memory_size: >0, page-aligned */
        if (seg.memory_size == 0) return JE_ERR_INVALID;
        if (seg.memory_size & (JE_PAGE_SIZE - 1)) return JE_ERR_INVALID;
        /* target_offset: page-aligned, inside arena (subtraction-based) */
        if (seg.target_offset & (JE_PAGE_SIZE - 1)) return JE_ERR_INVALID;
        if (seg.target_offset > JE_VM_ARENA_SIZE) return JE_ERR_INVALID;
        if (seg.memory_size > JE_VM_ARENA_SIZE - seg.target_offset)
            return JE_ERR_INVALID;
        /* source bounds: source_size <= memory_size; src range within blob */
        if (seg.source_size > seg.memory_size) return JE_ERR_INVALID;
        if (seg.source_offset > blob_len) return JE_ERR_INVALID;
        if (seg.source_size > (uint64_t)blob_len - seg.source_offset)
            return JE_ERR_INVALID;

        /* page accounting + page-granule overlap / stack-guard exclusion */
        uint64_t seg_pages = seg.memory_size / JE_PAGE_SIZE;
        uint64_t first = seg.target_offset / JE_PAGE_SIZE;
        if (seg_pages > JE_VM_ARENA_PAGES - first) return JE_ERR_INVALID;
        for (uint64_t p = first; p < first + seg_pages; ++p) {
            if (used[p]) return JE_ERR_INVALID; /* overlap or stack/guard */
            used[p] = 1;
        }
        /* underflow-safe budget check: total + seg_pages <= MAX_USER_PAGES.
         * A single oversized segment must not wrap the subtraction. */
        if (seg_pages > JE_VM_MAX_USER_PAGES) return JE_ERR_INVALID;
        if (total_user_pages > JE_VM_MAX_USER_PAGES - seg_pages)
            return JE_ERR_INVALID;
        total_user_pages += seg_pages;

        /* entry must lie in an RX segment's initialized (source) bytes */
        if (kind == JE_VM_USER_CODE) {
            uint64_t lo = seg.target_offset;
            uint64_t hi = seg.target_offset + seg.source_size; /* exclusive */
            if (image->entry_offset >= lo && image->entry_offset < hi)
                entry_ok = true;
        }
    }
    if (!entry_ok) return JE_ERR_INVALID;
    return JE_OK;
}

/* ---- journalling ---------------------------------------------------------- */

/* Append a journal entry. Overflow is impossible by the _Static_assert above, so
 * this never fails on builder-produced sequences; an overflow would be a fatal
 * invariant violation. Returns void so callers cannot leave a mutation
 * un-journalled. */
static void journal_push(je_vm *vm, je_vm_process *p, je_vm_journal_entry e)
{
    if (p->journal_count >= JE_VM_MAX_JOURNAL)
        je_vm_panic(vm, "vm: journal overflow");
    p->journal[p->journal_count++] = e;
}

/* Reverse-replay the journal, invoking the matching infallible undo op for each
 * step. Undo ops are void and panic internally on misuse (builder-produced input
 * never misuses them). After unwinding, the slot is cleared to FREE. */
static void unwind(je_vm *vm, uint32_t idx)
{
    const je_vm_ops *o = vm->ops;
    je_vm_process *p = &vm->procs[idx];
    for (size_t n = p->journal_count; n > 0; --n) {
        je_vm_journal_entry e = p->journal[n - 1];
        je_vm_ref ref = { idx, e.extent, e.page };
        switch (e.kind) {
        case JE_VM_J_PTE:
            /* ADR D3: clear PTE -> flush -> (later) restore alias -> free.
             * Flush immediately so no stale user translation survives before the
             * frame's alias is restored to RW or the frame is freed. */
            o->clear_pte(o->context, idx, e.varena_off);
            o->flush(o->context);
            break;
        case JE_VM_J_DEMOTE: o->restore_alias(o->context, ref); break;
        case JE_VM_J_MAP:    o->unmap(o->context, ref); break;
        case JE_VM_J_TABLES:
            o->drop_tables(o->context, idx);
            o->flush(o->context); /* retire paging-structure translations */
            break;
        case JE_VM_J_EXTENT: o->free_extent(o->context, idx, e.extent); break;
        case JE_VM_J_HOLD:   o->release_hold(o->context, e.physical, e.pages); break;
        default: je_vm_panic(vm, "vm: bad journal kind"); break;
        }
    }
    o->flush(o->context);
    *p = (je_vm_process){0};
    p->state = JE_VM_FREE;
}

/* ---- init ----------------------------------------------------------------- */

je_status je_vm_init(je_vm *vm, const je_vm_ops *ops, je_pages *pages,
                     size_t reserve_pages)
{
    if (!vm || !ops || !pages) return JE_ERR_INVALID;
    /* require the full op table */
    if (!ops->reserve_hold || !ops->register_extent || !ops->map ||
        !ops->link_tables || !ops->install_pte || !ops->demote_alias ||
        !ops->unmap || !ops->clear_pte || !ops->restore_alias ||
        !ops->free_extent || !ops->release_hold || !ops->drop_tables ||
        !ops->flush || !ops->read_user_pte || !ops->read_upper ||
        !ops->read_alias || !ops->extent_frame_phys || !ops->panic)
        return JE_ERR_INVALID;
    *vm = (je_vm){0};
    vm->ops = ops;
    vm->pages = pages;
    vm->reserve_pages = reserve_pages;
    return JE_OK;
}

/* ---- build ---------------------------------------------------------------- */

/* Register an extent, journal it, and return its index via *ex. */
static je_status build_extent(je_vm *vm, uint32_t idx, size_t npages,
                              je_vm_kind kind, uint32_t *ex)
{
    je_vm_process *p = &vm->procs[idx];
    je_status s = vm->ops->register_extent(vm->ops->context, idx, npages, kind, ex);
    if (s != JE_OK) return s;
    /* journal the allocation immediately (cannot be lost; push is infallible). */
    je_vm_journal_entry e = { JE_VM_J_EXTENT, *ex, 0, 0, 0, 0 };
    journal_push(vm, p, e);
    /* mirror into the local extent table (bookkeeping/audit). The table is
     * sized to the validated maximum, so overflow is an invariant violation. */
    if (p->extent_count >= JE_VM_MAX_EXTENTS)
        je_vm_panic(vm, "vm: extent table overflow");
    uint64_t base = 0;
    if (vm->ops->extent_frame_phys(vm->ops->context, idx, *ex, 0, &base) != JE_OK)
        je_vm_panic(vm, "vm: extent_frame_phys on just-registered extent");
    /* Verify the contiguity the audit relies on (register_extent contract):
     * frame i must sit at base + i*JE_PAGE_SIZE. A violating backend is fatal. */
    for (uint32_t fp = 1; fp < (uint32_t)npages; ++fp) {
        uint64_t fph = 0;
        if (vm->ops->extent_frame_phys(vm->ops->context, idx, *ex, fp, &fph) != JE_OK)
            je_vm_panic(vm, "vm: extent_frame_phys on owned frame");
        if (fph != base + (uint64_t)fp * JE_PAGE_SIZE)
            je_vm_panic(vm, "vm: non-contiguous extent violates register_extent");
    }
    p->extents[p->extent_count].physical = base;
    p->extents[p->extent_count].pages = npages;
    p->extents[p->extent_count].kind = kind;
    p->extents[p->extent_count].backend_id = *ex;
    p->extent_count++;
    return JE_OK;
}

/* Populate one user page: map, zero, copy source bytes, (for code/rodata)
 * demote the alias, then install the leaf PTE. Matches ADR D3 ordering. */
static je_status build_page(je_vm *vm, uint32_t idx, uint32_t ex, uint32_t page,
                            je_vm_kind kind, uint64_t varena_off,
                            const uint8_t *src, uint64_t src_bytes)
{
    je_vm_process *p = &vm->procs[idx];
    const je_vm_ops *o = vm->ops;
    je_vm_ref ref = { idx, ex, page };

    uint8_t *dst = o->map(o->context, ref);
    if (!dst) return JE_ERR_NOMEM;
    /* The staging map is transient: it is always dropped (unmap) below before we
     * return, so it is NOT journalled — there is no persistent state to undo. */

    for (uint64_t i = 0; i < JE_PAGE_SIZE; ++i) dst[i] = 0;        /* zero whole */
    for (uint64_t i = 0; i < src_bytes; ++i) dst[i] = src[i];      /* copy init  */

    /* D3: for non-writable frames, unmap the loader alias and flush, then demote
     * the identity alias to RO/NX (flush), then install — so a user mapping and a
     * writable alias never coexist. For writable frames, unmap, flush, install. */
    o->unmap(o->context, ref);
    o->flush(o->context);

    if (kind_is_nonwritable(kind)) {
        je_status s = o->demote_alias(o->context, ref);
        if (s != JE_OK) return s; /* alias still RW, not installed: extent free covers it */
        je_vm_journal_entry de = { JE_VM_J_DEMOTE, ex, page, 0, 0, 0 };
        journal_push(vm, p, de);
        o->flush(o->context);
    }

    je_status s = o->install_pte(o->context, ref, varena_off);
    if (s != JE_OK) return s;
    je_vm_journal_entry pe = { JE_VM_J_PTE, ex, page, varena_off, 0, 0 };
    journal_push(vm, p, pe);
    return JE_OK;
}

je_status je_vm_build_quota(je_vm *vm, const je_vm_image *image, const void *blob,
                            size_t blob_len, int nx_available, uint32_t handle_quota,
                            uint32_t *out_index)
{
    /* This is deliberately the first check: an invalid build parameter must
     * fail before any other validation or state change. */
    if (handle_quota == 0u || handle_quota > JE_VM_HANDLE_QUOTA_MAX)
        return JE_ERR_INVALID;
    if (!vm || !vm->ops || !image || !out_index) return JE_ERR_INVALID;
    const je_vm_ops *o = vm->ops;

    /* (1) NX gate (ADR D2). */
    if (!nx_available) return JE_ERR_UNSUPPORTED;

    /* (2) free process slot. */
    uint32_t idx = JE_VM_MAX_PROCESSES;
    for (uint32_t i = 0; i < JE_VM_MAX_PROCESSES; ++i)
        if (vm->procs[i].state == JE_VM_FREE) { idx = i; break; }
    if (idx == JE_VM_MAX_PROCESSES) return JE_ERR_BUSY;

    /* (3) image validation. */
    je_status s = je_vm_image_validate(image, blob_len);
    if (s != JE_OK) return s;

    /* A non-empty blob must be a real pointer. (Validation requires an RX
     * segment with source_size>0 for the entry, so a valid image always has
     * blob_len>=1; the guard is defensive.) */
    if (blob_len > 0 && !blob) return JE_ERR_INVALID;

    /* compute declared user pages (image segments + stack). */
    uint64_t user_pages = JE_VM_STACK_PAGES;
    for (uint32_t i = 0; i < image->segment_count; ++i)
        user_pages += image->segments[i].memory_size / JE_PAGE_SIZE;

    /* (4) atomic admission: need = tables + user + kstack, no underflow. */
    size_t need = JE_VM_TABLE_PAGES + (size_t)user_pages + JE_VM_KSTACK_PAGES;
    if (need > vm->pages->free_count) return JE_ERR_NOMEM;
    if (vm->pages->free_count - need < vm->reserve_pages) return JE_ERR_NOMEM;

    /* ---- begin mutation; from here any failure unwinds via journal. ---- */
    je_vm_process *p = &vm->procs[idx];
    *p = (je_vm_process){0};
    p->state = JE_VM_BUILDING;

    /* 4a. hold the concrete kstack run (ADR D4). */
    uint64_t kphys = 0;
    s = o->reserve_hold(o->context, JE_VM_KSTACK_PAGES, &kphys);
    if (s != JE_OK) { unwind(vm, idx); return s; }
    p->kstack_physical = kphys;
    p->held_kstack = JE_VM_KSTACK_PAGES;
    { je_vm_journal_entry he = { JE_VM_J_HOLD, 0, 0, 0, kphys, JE_VM_KSTACK_PAGES };
      journal_push(vm, p, he); }

    /* 4b. table extent + topology. */
    uint32_t table_ex;
    s = build_extent(vm, idx, JE_VM_TABLE_PAGES, JE_VM_TABLE, &table_ex);
    if (s != JE_OK) { unwind(vm, idx); return s; }
    uint32_t root;
    s = o->link_tables(o->context, idx, table_ex, &root);
    if (s != JE_OK) { unwind(vm, idx); return s; }
    p->root = root;
    { je_vm_journal_entry te = { JE_VM_J_TABLES, table_ex, 0, 0, 0, 0 };
      journal_push(vm, p, te); }

    /* 4c. image segments. */
    const uint8_t *bytes = blob;
    for (uint32_t si = 0; si < image->segment_count; ++si) {
        je_vm_segment seg = image->segments[si];
        je_vm_perm leaf;
        je_vm_kind kind = perm_to_kind(seg.perm, &leaf);
        uint64_t seg_pages = seg.memory_size / JE_PAGE_SIZE;
        uint32_t ex;
        s = build_extent(vm, idx, (size_t)seg_pages, kind, &ex);
        if (s != JE_OK) { unwind(vm, idx); return s; }
        for (uint64_t pg = 0; pg < seg_pages; ++pg) {
            uint64_t off_in_seg = pg * JE_PAGE_SIZE;
            uint64_t varena = seg.target_offset + off_in_seg;
            /* source bytes for this page (0 for BSS-only pages). Compute the
             * source pointer ONLY when there are bytes to copy, so we never form
             * an out-of-bounds pointer (UB) for a BSS page past the blob end. */
            uint64_t copy = 0;
            const uint8_t *src = NULL;
            if (off_in_seg < seg.source_size) {
                uint64_t remaining = seg.source_size - off_in_seg;
                copy = remaining < JE_PAGE_SIZE ? remaining : JE_PAGE_SIZE;
                src = bytes + seg.source_offset + off_in_seg;
            }
            s = build_page(vm, idx, ex, (uint32_t)pg, kind, varena, src, copy);
            if (s != JE_OK) { unwind(vm, idx); return s; }
        }
    }

    /* 4d. user stack (USER_DATA), top of arena. */
    uint32_t stack_ex;
    s = build_extent(vm, idx, JE_VM_STACK_PAGES, JE_VM_USER_DATA, &stack_ex);
    if (s != JE_OK) { unwind(vm, idx); return s; }
    for (uint64_t pg = 0; pg < JE_VM_STACK_PAGES; ++pg) {
        uint64_t varena = JE_VM_STACK_OFFSET + pg * JE_PAGE_SIZE;
        s = build_page(vm, idx, stack_ex, (uint32_t)pg, JE_VM_USER_DATA,
                       varena, NULL, 0);
        if (s != JE_OK) { unwind(vm, idx); return s; }
    }

    /* M3b/M4: initialize the per-process handle table over the inline slot
     * storage before publishing READY (ADR 0008/0010). The per-table quota is
     * below capacity by contract, so exhaustion reports QUOTA before NOSLOTS.
     * Compile-time-sized storage makes init infallible for a validated quota;
     * teardown/unwind zeroing reclaims it and re-init on slot reuse prevents
     * stale LIVE entries from surviving into a reused process slot. */
    if (!je_handle_table_init(&p->htable, p->hslots, JE_VM_HANDLE_CAP,
                              handle_quota)) {
        unwind(vm, idx);
        return JE_ERR_INVALID;   /* unreachable after the range check above */
    }

    /* publish. */
    p->entry = image->entry_offset;
    p->state = JE_VM_READY;
    *out_index = idx;
    return JE_OK;
}

je_status je_vm_build(je_vm *vm, const je_vm_image *image, const void *blob,
                      size_t blob_len, int nx_available, uint32_t *out_index)
{
    return je_vm_build_quota(vm, image, blob, blob_len, nx_available,
                             JE_VM_HANDLE_QUOTA_DEFAULT, out_index);
}

/* ---- destroy -------------------------------------------------------------- */

je_status je_vm_destroy(je_vm *vm, uint32_t index)
{
    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    if (vm->procs[index].state != JE_VM_READY) return JE_ERR_INVALID;
    /* A3-1: the release walk + unwind + the *p={0} zeroing in unwind() must be
     * ONE critical section. The direct main.c destroy callers may run with IF=1,
     * so an IF=0 precondition is not enough: a timer preempting between the walk
     * and the zeroing, while the peer process is live, would let the peer's
     * HANDLE_CLOSE mutate the same channel record mid-teardown (a UAF). Hold IRQs
     * off across the whole sequence when the kernel installed the guard; NULL
     * (the unit test) keeps the prior behaviour. The teardown hook (the endpoint
     * release walk) runs after the checks above and immediately before unwind. */
    uint64_t irq = 0u;
    if (vm->irq_save) irq = vm->irq_save();
    if (vm->teardown) vm->teardown(vm->teardown_ctx, &vm->procs[index].htable);
    unwind(vm, index);
    if (vm->irq_restore) vm->irq_restore(irq);
    return JE_OK;
}

/* ---- audit oracle --------------------------------------------------------- */

je_status je_vm_audit(const je_vm *vm, uint32_t index)
{
    if (!vm || !vm->ops || index >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    const je_vm_ops *o = vm->ops;
    const je_vm_process *p = &vm->procs[index];
    if (p->state != JE_VM_READY) return JE_ERR_INVALID;

    /* upper levels: PML4[0] P|U; kernel PDPT[0] P, !U; arena PDPT[1] P|U;
     * arena PD[0] P|U. */
    uint32_t fl;
    if (o->read_upper(o->context, index, 4, 0, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */
    if (o->read_upper(o->context, index, 3, 0, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || (fl & 4u)) return JE_ERR_INVALID;              /* P, !U */
    if (o->read_upper(o->context, index, 3, 1, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */
    if (o->read_upper(o->context, index, 2, 0, &fl) != JE_OK) return JE_ERR_INVALID;
    if (!(fl & 1u) || !(fl & 4u)) return JE_ERR_INVALID;              /* P|U */

    /* Every OTHER upper-table entry must be not-present. The build sets exactly
     * four upper entries (PML4[0], PDPT[0], PDPT[1], PD[0]); the other 1532 upper
     * entries must read P=0. (The 512 PT leaves are checked by the per-arena-page
     * walk below.) A stale present entry here — e.g. from a recycled table frame
     * that was not zeroed on free — is a live mapping that walk would miss, so it
     * is a disclosure hazard. This closes the gap the zero-on-free fix addresses
     * and fails closed if that fix regresses. The allow-list below assumes the
     * arena fits one PD entry and one PT (asserted), so PD has only [0] and there
     * is a single PT; a larger arena must change the topology and this list. */
    _Static_assert(JE_VM_ARENA_SIZE <= 0x200000u,
                   "audit allow-list assumes a one-PD-entry, single-PT arena");
    for (uint32_t i = 0; i < 512u; ++i) {
        if (o->read_upper(o->context, index, 4, i, &fl) != JE_OK) return JE_ERR_INVALID;
        if ((fl & 1u) && i != 0u) return JE_ERR_INVALID;             /* PML4: only [0] */
        if (o->read_upper(o->context, index, 3, i, &fl) != JE_OK) return JE_ERR_INVALID;
        if ((fl & 1u) && i != 0u && i != 1u) return JE_ERR_INVALID;  /* PDPT: only [0],[1] */
        if (o->read_upper(o->context, index, 2, i, &fl) != JE_OK) return JE_ERR_INVALID;
        if ((fl & 1u) && i != 0u) return JE_ERR_INVALID;             /* PD: only [0] */
    }

    /* Walk every arena page: a present user leaf must (a) not be the guard page,
     * (b) carry a perm that is one of the three legal values, (c) map a physical
     * frame that belongs to one of THIS process's USER_* extents, and (d) match
     * that extent's kind (CODE->RX, RODATA->R_NX, DATA->RW_NX). Also verify the
     * owning frame's identity alias state: RO_NX for CODE/RODATA, RW for DATA. */
    uint64_t seen_phys[JE_VM_ARENA_PAGES];
    size_t present_count = 0;
    for (uint64_t pg = 0; pg < JE_VM_ARENA_PAGES; ++pg) {
        uint64_t off = pg * JE_PAGE_SIZE;
        uint64_t phys; je_vm_perm perm; int present;
        if (o->read_user_pte(o->context, index, off, &phys, &perm, &present) != JE_OK)
            return JE_ERR_INVALID;
        if (!present) continue;
        if (off == JE_VM_GUARD_OFFSET) return JE_ERR_INVALID;
        if (perm != JE_VM_PERM_RX && perm != JE_VM_PERM_R_NX &&
            perm != JE_VM_PERM_RW_NX)
            return JE_ERR_INVALID;

        /* one-to-one: no physical frame may back two arena offsets. */
        for (size_t k = 0; k < present_count; ++k)
            if (seen_phys[k] == phys) return JE_ERR_INVALID;
        seen_phys[present_count++] = phys;

        /* find the owning extent (by physical range) and check kind/perm/alias */
        bool owned = false;
        for (size_t a = 0; a < p->extent_count && !owned; ++a) {
            const je_vm_extent *ex = &p->extents[a];
            if (ex->kind == JE_VM_TABLE) continue; /* tables never back a user PTE */
            uint64_t end = ex->physical + (uint64_t)ex->pages * JE_PAGE_SIZE;
            if (phys < ex->physical || phys >= end) continue;
            /* kind/perm consistency */
            je_vm_perm want = (ex->kind == JE_VM_USER_CODE)   ? JE_VM_PERM_RX
                            : (ex->kind == JE_VM_USER_RODATA) ? JE_VM_PERM_R_NX
                            :                                   JE_VM_PERM_RW_NX;
            if (perm != want) return JE_ERR_INVALID;
            /* alias state for the specific frame within the extent */
            uint32_t fpage = (uint32_t)((phys - ex->physical) / JE_PAGE_SIZE);
            je_vm_ref ref = { index, ex->backend_id, fpage };
            je_vm_alias al;
            if (o->read_alias(o->context, ref, &al) != JE_OK) return JE_ERR_INVALID;
            je_vm_alias want_al = (ex->kind == JE_VM_USER_DATA) ? JE_VM_ALIAS_RW
                                                                : JE_VM_ALIAS_RO_NX;
            if (al != want_al) return JE_ERR_INVALID;
            owned = true;
        }
        if (!owned) return JE_ERR_INVALID; /* leaf maps a non-owned/table frame */
    }

    /* completeness: the number of present user leaves must equal the sum of the
     * process's USER_* extent pages (image segments + stack) — no missing and no
     * extra mappings. */
    {
        uint64_t expect = 0;
        for (size_t a = 0; a < p->extent_count; ++a)
            if (p->extents[a].kind != JE_VM_TABLE) expect += p->extents[a].pages;
        if ((uint64_t)present_count != expect) return JE_ERR_INVALID;
    }

    /* cross-process disjointness: distinct roots and no shared physical frame. */
    for (size_t a = 0; a < p->extent_count; ++a) {
        for (uint32_t fp = 0; fp < p->extents[a].pages; ++fp) {
            uint64_t myp;
            if (o->extent_frame_phys(o->context, index, p->extents[a].backend_id,
                                     fp, &myp) != JE_OK)
                return JE_ERR_INVALID;
            for (uint32_t q = 0; q < JE_VM_MAX_PROCESSES; ++q) {
                if (q == index || vm->procs[q].state != JE_VM_READY) continue;
                const je_vm_process *op = &vm->procs[q];
                if (op->root == p->root) return JE_ERR_INVALID; /* distinct roots */
                for (size_t b = 0; b < op->extent_count; ++b)
                    for (uint32_t pg2 = 0; pg2 < op->extents[b].pages; ++pg2) {
                        uint64_t otherp;
                        if (o->extent_frame_phys(o->context, q,
                                                 op->extents[b].backend_id, pg2,
                                                 &otherp) != JE_OK)
                            return JE_ERR_INVALID;
                        if (otherp == myp) return JE_ERR_INVALID;
                    }
            }
        }
    }
    return JE_OK;
}
