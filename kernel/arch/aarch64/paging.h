/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP2 PR-1b (ADR 0011 D2/D3/D4/D6/D7): aarch64 kernel MMU bring-up.
 *
 * Brings up a 4 KiB-granule, 48-bit, 4-level stage-1 translation with the kernel
 * IDENTITY-mapped in TTBR0_EL1 (VA == PA; TCR.EPD1=1, TTBR1=0 — the high-half
 * split is MP3, ADR 0011 D2). The kernel image is mapped W^X per section, device
 * MMIO (PL011) is Device-nGnRnE, and an unmapped guard page sits below the boot
 * stack. Page-table frames are drawn from the PR-1a physical allocator.
 *
 * THIS HEADER IS THE FROZEN CONTRACT between the builder/enable implementation
 * (paging.c + the asm enable leaf in paging_enable.S), the W^X/alias audit, the
 * hosted descriptor test, and the ARM glue (main.c). Change it only by a
 * reviewed contract update. The register values are pinned in paging.c against
 * DDI0487 and cross-checked by _Static_assert; this header fixes the SHAPE and
 * the permission contract.
 *
 * Caller context: EL1, single core, interrupts masked. je_arm_mmu_build runs
 * with the MMU still OFF and only writes RAM (page tables) + the allocator.
 * je_arm_mmu_enable runs the documented cache/barrier/TLB sequence and turns the
 * MMU on; after it returns the kernel executes translated (identity) addresses.
 *
 * HONESTY (ADR 0011 D6): QEMU TCG models no caches, so the DC IVAC / barrier
 * maintenance in the enable leaf is REVIEWED, not observed. Translation and
 * permissions ARE observed (a fault is a fault on TCG). main.c / HANDOFF must
 * say so; smoke evidence never backs the cache-maintenance claim.
 */
#ifndef JEIOS_AARCH64_PAGING_H
#define JEIOS_AARCH64_PAGING_H

#include <jeios/alloc.h>
#include <jeios/status.h>
#include <jeios/fdt.h>

/* Leaf permission classes (ADR 0011 D4). The builder translates these to the
 * exact descriptor bits (AP[2:1], PXN, UXN, AF, SH, AttrIdx); the audit checks
 * them back from the installed descriptors. */
typedef enum {
    JE_MAP_TEXT = 0,   /* RX  : AP=10 (EL1 RO), PXN=0, UXN=1, AttrIdx 0 (Normal) */
    JE_MAP_RODATA,     /* R   : AP=10,          PXN=1, UXN=1, AttrIdx 0 */
    JE_MAP_RW,         /* RW  : AP=00 (EL1 RW), PXN=1, UXN=1, AttrIdx 0 (data, tables) */
    JE_MAP_DEVICE,     /* RW+XN device: AP=00,  PXN=1, UXN=1, AttrIdx 1 (nGnRnE) */
    JE_MAP_CLASS_COUNT
} je_map_class;

/* One contiguous identity mapping request: physical == virtual, [base, base+size)
 * both page-aligned, mapped with `class` permissions. The builder splits into
 * 4 KiB leaves (no block mappings in MP2, so no page spans two classes). */
typedef struct {
    uint64_t base;
    uint64_t size;
    je_map_class cls;
} je_map_region;

/*
 * Opaque MMU bring-up context. Holds the root table physical address and a
 * handle to the allocator the builder draws table frames from. Lives in the
 * kernel image (.bss) in the glue; no dynamic allocation of the context itself.
 */
typedef struct je_arm_mmu je_arm_mmu;

/* Pinned register values (defined + _Static_assert-checked in paging.c). Exposed
 * so the hosted test and the audit can assert the exact bit patterns rather than
 * re-deriving them. */
extern const uint64_t JE_ARM_MAIR_EL1;   /* 0x00000000000000FF: Attr0 Normal WB, Attr1 Device-nGnRnE */
extern const uint64_t JE_ARM_TCR_EL1;    /* 4K granule, T0SZ=16, EPD1=1, IPS from PARange, IS/WBWA walk */
extern const uint64_t JE_ARM_SCTLR_SET;  /* bits OR'd into SCTLR_EL1 at enable: M|C|I|WXN */

/*
 * Build the identity page tables for `regions` (count `n`) using table frames
 * drawn from `pages`. MMU MUST be off. Writes only RAM. Does NOT enable the MMU.
 * Returns JE_ERR_INVALID on a bad argument (NULL, misaligned/zero region,
 * unknown class, overlapping regions), JE_ERR_NOMEM if the allocator cannot
 * supply a table frame, else JE_OK with *out populated (root table phys).
 *
 * Every leaf gets AF=1 and SH=11 (RAM) per D3/D4. The builder refuses to map a
 * frame that would become reachable both executable and writable (the no-W^X-
 * alias invariant is enforced at install time AND re-checked by the audit).
 */
je_status je_arm_mmu_build(je_arm_mmu *mmu, je_pages *pages,
                           const je_map_region *regions, size_t n, uint64_t *out_root);

/*
 * Audit the built tables BEFORE enable (ADR 0011 D4/D7). Walks every valid leaf
 * and asserts: no leaf is W+X; no physical frame is reachable via both a
 * writable and an executable leaf; AF=1, AP[1]=0 (no EL0) and the AttrIdx/PXN/
 * UXN match the region class of the VA; APTable/PXNTable/UXNTable are 0 at every
 * level. Returns JE_OK or JE_ERR_INVALID (the glue then FATAL-halts). Pure read
 * over the tables; MMU may be off or on.
 */
je_status je_arm_mmu_audit(const je_arm_mmu *mmu,
                           const je_map_region *regions, size_t n);

/*
 * Enable the MMU (ADR 0011 D6). MUST be preceded by a successful build+audit.
 * Performs, in one path with no stack writes between the maintenance and the
 * SCTLR write (the critical window is an asm leaf, je_arm_mmu_enable_leaf):
 *   DC IVAC over the table range and the writable image range [__data_start,_end)
 *     + the DTB copy (invalidate-only: tables were written MMU/C-off) ; DSB SY
 *   MSR MAIR/TCR/TTBR0 ; TTBR1=0 ; DSB SY ; ISB
 *   TLBI VMALLE1 ; DSB NSH ; ISB            (reset TLB state is IMPL DEFINED)
 *   SCTLR_EL1 |= M|C|I|WXN (one write) ; ISB
 *   IC IALLU ; DSB NSH ; ISB                (instrs fetched MMU-off may be stale)
 * After this returns the MMU is on; the next console_write reaches the UART via
 * its Device mapping. _Noreturn-safe: a fault during enable vectors to the
 * already-mapped handler. Returns JE_OK (it cannot meaningfully fail-and-return;
 * a precondition miss is JE_ERR_INVALID before touching SCTLR).
 */
je_status je_arm_mmu_enable(const je_arm_mmu *mmu);

/*
 * Translation probe for the proofs (ADR 0011 D7): run `AT S1E1R`/`AT S1E1W` on
 * `va` and return PAR_EL1. The caller decodes PAR.F (fault), PAR.PA, PAR.ATTR,
 * PAR.SH. Used to PROVE the MMU actually translates (a non-identity probe) and
 * that MMIO carries the Device attribute — not merely that the kernel "didn't
 * crash". write==true selects S1E1W. Must run with the MMU on.
 */
uint64_t je_arm_at_s1e1(uint64_t va, int write);

/* Root-table physical address of a built context (for markers/evidence). */
uint64_t je_arm_mmu_root(const je_arm_mmu *mmu);

#endif /* JEIOS_AARCH64_PAGING_H */
