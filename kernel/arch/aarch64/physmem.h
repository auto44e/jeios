/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-1a (ADR 0011 D5): aarch64 physical-allocator bring-up adapter.
 *
 * Bridges the arch-neutral FDT boot context (je_boot_ctx, fdt.h) and the
 * kernel/DTB/initrd arch facts (je_boot_facts, fdt.h) into the arch-neutral page
 * allocator (je_pages, alloc.h) WITHOUT the allocator learning any FDT types.
 * The MMU is still OFF in this PR; this is a pure physical-memory-bookkeeping
 * step that lets a later PR (PR-1b) draw page-table frames from a real,
 * reservation-aware allocator.
 *
 * THIS HEADER IS THE FROZEN CONTRACT between the adapter implementation
 * (physmem.c), the hosted test (tests/test_physmem.c), and the ARM glue
 * (main.c), written by different authors (AGENTS.md multi-agent model). Change
 * it only by an explicit, reviewed contract update.
 *
 * ---- Default-deny model (ADR 0011 D5; the PR-H ".bss-zero means ALLOW" class) ----
 * A physical page is FREE iff ALL hold:
 *   (1) its whole page [pa, pa+4096) lies inside exactly one je_boot_ctx RAM
 *       range (membership is REQUIRED, never assumed), AND
 *   (2) it does not intersect ANY reserved region:
 *         - the kernel image  [kernel_image.base, +size)      (je_boot_facts)
 *         - the DTB source    [dtb_src.base, +size)           (je_boot_facts)
 *         - the initrd        [initrd.base, +size)  if has_initrd (je_boot_ctx)
 *         - every kept reservation ctx->rsv[0..rsv_count)     (incl. no-map)
 *         - the boot stack: it lives inside the kernel image extent
 *           [_text,_end), so (a) already covers it. (A dedicated unmapped stack
 *           GUARD page is PR-1b work — ADR 0011 D4 — and does not exist yet.)
 * A page the predicate cannot positively prove free (outside RAM, partially in
 * RAM, or touching any reserved region) is RESERVED. Any omission therefore
 * fails SAFE (unavailable), which is why the exhaustion oracle (smoke_arm.py)
 * recomputes the expected free set independently from the DTB fixture.
 *
 * rsv_dynamic (reserved-memory children with `size` but no `reg`, fdt.h) carry
 * no fixed address, so there is nothing to subtract here; MP1 allocated nothing
 * for them and this PR does not either. The obligation is recorded (ADR 0011 D5)
 * and honored when a dynamic reservation is actually placed (a later PR).
 *
 * Caller context: EL1, MMU off, single core, interrupts off. No allocation in
 * this path other than the one je_pages_init_pred pass. The je_pages object and
 * the facts/ctx it reads must outlive init (they are file-scope statics in the
 * glue). No pointers are retained past the call.
 */
#ifndef JEIOS_AARCH64_PHYSMEM_H
#define JEIOS_AARCH64_PHYSMEM_H

#include <jeios/alloc.h>
#include <jeios/fdt.h>

/*
 * Immutable view the predicate reads. All ranges are absolute physical, in the
 * same units as the allocator window [JE_PHYSICAL_BASE, JE_PHYSICAL_LIMIT). The
 * adapter does NOT copy the DTB context; it borrows it for the duration of init.
 */
typedef struct {
    const je_boot_ctx   *ctx;    /* parsed RAM/reservations/initrd (borrowed) */
    je_range             kernel; /* je_boot_facts.kernel_image */
    je_range             dtb;    /* je_boot_facts.dtb_src */
} je_arm_physmem_view;

/*
 * Default-deny free-page predicate (signature: je_page_free_pred, alloc.h).
 * `ctx` is a `const je_arm_physmem_view *`. Returns true iff the page whose base
 * is `phys` satisfies the model above. Pure, side-effect-free, total over the
 * allocator window. Exposed (not static) so the hosted test drives it directly.
 */
bool je_arm_page_free(const void *view, uint64_t phys);

/*
 * Initialize `pages` from the view using the default-deny predicate. Thin
 * wrapper over je_pages_init_pred(pages, je_arm_page_free, view). Returns
 * JE_ERR_INVALID if pages or view or view->ctx is NULL; otherwise the
 * je_pages_init_pred status. On success `pages->free_count` is the number of
 * genuinely free pages in the window.
 */
je_status je_arm_physmem_init(je_pages *pages, const je_arm_physmem_view *view);

#endif /* JEIOS_AARCH64_PHYSMEM_H */
