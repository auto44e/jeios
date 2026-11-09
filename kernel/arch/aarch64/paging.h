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
 * MP3a-1 limitation: per-process table/user-frame construction is deliberately
 * MMU-off, and the user backend rejects its identity-pointer map/clear/flush
 * operations after enable. The enable leaf's DC IVAC ranges cover the static
 * kernel tables/image, not later process tables/frames at or above
 * 0x40100000; their MMU-on mappings and cache maintenance are MP3a-2 work.
 *
 * HONESTY (ADR 0011 D6): QEMU TCG models no caches, so the DC IVAC / barrier
 * maintenance in the enable leaf is REVIEWED, not observed. Translation and
 * permissions ARE observed (a fault is a fault on TCG). main.c / HANDOFF must
 * say so; smoke evidence never backs the cache-maintenance claim.
 */
#ifndef JEIOS_AARCH64_PAGING_H
#define JEIOS_AARCH64_PAGING_H

#include <stdbool.h>

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
    /* MP3a-1 EL0 classes. AP[1] is descriptor bit 6, so these are the only
     * classes that may be reached by an EL0 translation. USER_CODE is the
     * sole EL0-executable class and remains EL0 read-only (W^X). */
    JE_MAP_USER_CODE,   /* EL0 RX: AP=11, PXN=1, UXN=0, nG=1, AttrIdx 0 */
    JE_MAP_USER_RODATA, /* EL0 R : AP=11, PXN=1, UXN=1, nG=1, AttrIdx 0 */
    JE_MAP_USER_DATA,   /* EL0 RW: AP=01, PXN=1, UXN=1, nG=1, AttrIdx 0 */
    JE_MAP_CLASS_COUNT
} je_map_class;

/* One source for the MP3a user-leaf literals. The ARM VM backend's
 * per-process audit and the paging.c encoder/ledger both consume these
 * definitions; keeping them in the contract header prevents the two
 * translation units from silently accepting different permissions. */
#define JE_ARM_USER_CODE_BITS   UINT64_C(0x0020000000000fc3)
#define JE_ARM_USER_RODATA_BITS UINT64_C(0x0060000000000fc3)
#define JE_ARM_USER_DATA_BITS   UINT64_C(0x0060000000000f43)

_Static_assert((JE_ARM_USER_CODE_BITS & (UINT64_C(1) << 6)) != 0u &&
               (JE_ARM_USER_RODATA_BITS & (UINT64_C(1) << 6)) != 0u &&
               (JE_ARM_USER_DATA_BITS & (UINT64_C(1) << 6)) != 0u,
               "ARM user leaves must set AP[1]");
_Static_assert((JE_ARM_USER_CODE_BITS & (UINT64_C(1) << 54)) == 0u &&
               (JE_ARM_USER_RODATA_BITS & (UINT64_C(1) << 54)) != 0u &&
               (JE_ARM_USER_DATA_BITS & (UINT64_C(1) << 54)) != 0u,
               "only ARM user code may execute at EL0");

/* One contiguous identity mapping request: physical == virtual, [base, base+size)
 * both page-aligned, mapped with `class` permissions. The builder splits into
 * 4 KiB leaves (no block mappings in MP2, so no page spans two classes). */
typedef struct {
    uint64_t base;
    uint64_t size;
    je_map_class cls;
} je_map_region;

typedef enum {
    JE_ARM_AUDIT_REJECT_NONE = 0,
    JE_ARM_AUDIT_REJECT_LEDGER,
    JE_ARM_AUDIT_REJECT_COUNT,
    JE_ARM_AUDIT_REJECT_ROOT,
    JE_ARM_AUDIT_REJECT_MALFORMED,
    JE_ARM_AUDIT_REJECT_FOREIGN,
    JE_ARM_AUDIT_REJECT_REVISIT,
    JE_ARM_AUDIT_REJECT_UNREACHED,
    JE_ARM_AUDIT_REJECT_LEAF,
    JE_ARM_AUDIT_REJECT_ALIAS,
    JE_ARM_AUDIT_REJECT_PRECONDITION
} je_arm_mmu_audit_reject;

/* Last pre-enable audit diagnostics. These are observability state only; the
 * enable gate remains je_arm_last_audited in paging.c. */
extern je_arm_mmu_audit_reject je_arm_mmu_last_audit_reject;
extern uint64_t je_arm_mmu_last_audit_reads;

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

/* MP3a-1 user-root construction is MMU-off only. This explicit decoration
 * hook is called once, immediately after the unchanged MP2 audit and before
 * any per-process root is built; link_tables never decorates lazily. */
je_status je_arch_vm_decorate_kernel_root(void);

/* Read-only state used by the MP3a-1 backend to fail closed before it
 * dereferences physical table/frame addresses through identity pointers. */
bool je_arm_mmu_is_enabled(const je_arm_mmu *mmu);

/*
 * Translation probe for the proofs (ADR 0011 D7): run `AT S1E1R`/`AT S1E1W` on
 * `va` and return PAR_EL1. The caller decodes PAR.F (fault), PAR.PA, PAR.ATTR,
 * PAR.SH. Used to PROVE the MMU actually translates (a non-identity probe) and
 * that MMIO carries the Device attribute — not merely that the kernel "didn't
 * crash". write==true selects S1E1W. Must run with the MMU on.
 */
uint64_t je_arm_at_s1e1(uint64_t va, int write);
/* MP3a-1 EL0-permission probe. AT executes at EL1 but applies EL0 access
 * permissions; no ERET or EL0 instruction is involved. */
uint64_t je_arm_at_s1e0(uint64_t va, int write);

/* Root-table physical address of a built context (for markers/evidence). */
uint64_t je_arm_mmu_root(const je_arm_mmu *mmu);

/*
 * Build empty L3 coverage for the managed RAM window. This deliberately adds
 * table pages only; it does NOT add valid RAM leaves or a linear map. The
 * MP3a-1 user-test profile uses it so every future runtime 2 MiB slot has a
 * prebuilt L3 table while ADR 0011's no-RAM-wide-map invariant remains true.
 * Caller context is EL1, MMU off, before the pre-enable audit.
 */
je_status je_arm_mmu_prebuild_ram_l3(je_arm_mmu *mmu);

/* The scheduler records the ASID operand here for evidence. QEMU TCG does not
 * model stale-TLB behavior; the MSR/TLBI/DSB/ISB sequence is reviewed, not
 * observed. */
extern volatile uint64_t je_arm_tlbi_hosted_calls;
extern volatile uint64_t je_arm_tlbi_hosted_last_operand;

/*
 * ---- Runtime single-page map/unmap (ADR 0013 D4; reviewed contract update) ----
 *
 * Caller context: EL1, AFTER a successful je_arm_mmu_enable(mmu) (the context
 * must be the enabled one), single core, and the caller serializes every call
 * (interrupts masked or the scheduler lock held). The ledger is unlocked.
 *
 * Each call edits exactly ONE 4 KiB level-3 leaf, and only between the two
 * states INVALID <-> VALID (never valid -> different valid, so break-before-make
 * never arises). After the store it runs, in this order:
 *     DSB ISHST ; TLBI VALE1IS, (va >> 12) ; DSB ISH ; ISB
 * (store visible to the Inner-Shareable walker; drop any last-level TLB entry
 * for va at EL1; wait for completion; resync the instruction stream).
 *
 * NO TABLE CREATION: the runtime path never allocates a table. If any of the
 * L0..L2 descriptors for `va` is invalid (the 2 MiB region has no L3 table) the
 * call returns JE_ERR_UNSUPPORTED and writes nothing. Every table walked must
 * be a member of the build-time table ledger (all of which are identity-mapped
 * RW), so the walk never dereferences an unmapped or foreign table frame; a
 * non-member or a block descriptor is JE_ERR_INVALID. table_count, the table
 * window and the audit ledger are therefore unchanged by runtime maps.
 *
 * je_arm_mmu_map_page(mmu, va, pa, cls):
 *   va, pa page-aligned; va < 2^48; pa inside the allocator window
 *   [JE_PHYSICAL_BASE, JE_PHYSICAL_LIMIT); neither inside the kernel image
 *   [_text,_end) (which contains the unmapped boot-stack guard). Stacks pass
 *   va == pa (identity); the API takes both. cls must be non-executable and
 *   Normal memory: JE_MAP_RW or JE_MAP_RODATA. JE_MAP_TEXT (would need I-cache
 *   maintenance and a new W^X proof) and JE_MAP_DEVICE are JE_ERR_DENIED.
 *   The frame must have NO existing leaf anywhere (its ledger entry is empty);
 *   a frame already mapped by the static build (image, table frame) or by an
 *   earlier runtime map is JE_ERR_DENIED, so every runtime frame is singly
 *   mapped. The encoded leaf is re-checked against the audit's independent
 *   per-class literal and must have PXN|UXN set before install.
 *   Returns JE_ERR_INVALID for a bad argument, a context that is not the
 *   enabled one, or an already-valid leaf at va.
 *
 * je_arm_mmu_unmap_page(mmu, va):
 *   va page-aligned. Returns JE_ERR_INVALID if the leaf is already invalid or
 *   is not a page descriptor; JE_ERR_DENIED if the leaf was installed by the
 *   static build (only runtime-mapped leaves may be removed). On success the
 *   leaf is zeroed AND the frame's ledger entry is cleared, so the frame can
 *   later be remapped in a different class without a false W^X reject.
 *
 * Audit scope (BY DESIGN): je_arm_mmu_audit is a PRE-enable proof of the static
 * map and refuses an enabled context. Runtime-mapped leaves exist only after
 * enable and are outside that audit's scope; they are validated by the checks
 * above at install time and by the post-enable guard-fault proof. The
 * pre-enable audit is unchanged.
 *
 * Hardware note: tables are Normal WB and the walk is Inner-Shareable WBWA
 * (TCR_EL1), so the walker is coherent with the data cache once the MMU is on;
 * no DC maintenance of the table line is required. QEMU TCG does not model TLB
 * staleness faithfully, so the barrier/TLBI sequence is REVIEWED, not observed.
 */
je_status je_arm_mmu_map_page(je_arm_mmu *mmu, uint64_t va, uint64_t pa,
                              je_map_class cls);
je_status je_arm_mmu_unmap_page(je_arm_mmu *mmu, uint64_t va);

/*
 * Register/unregister a published MP3a USER_CODE or USER_RODATA frame in the
 * same physical-frame ledger consulted by je_arm_mmu_map_page(). These calls
 * are construction/teardown operations for the MMU-off MP3a-1 backend; they
 * reject an enabled context. A later RW alias of a registered frame returns
 * JE_ERR_DENIED, and teardown clears the entry before the frame is freed.
 */
je_status je_arm_mmu_publish_user_frame(je_arm_mmu *mmu, uint64_t physical,
                                         je_map_class cls);
je_status je_arm_mmu_unpublish_user_frame(je_arm_mmu *mmu,
                                           uint64_t physical);

/*
 * Guarded kernel stack (ADR 0013 D4) over a run of JE_ARM_STACK_RUN_PAGES
 * contiguous frames from the page allocator starting at `run_base`:
 *   [run_base, +1 page)            GUARD: left UNMAPPED (must be invalid now)
 *   [run_base + 1 page, +4 pages)  stack: mapped JE_MAP_RW (RW, PXN, UXN), va==pa
 * *out_top = run_base + JE_ARM_STACK_RUN_PAGES * 4096 (16-byte aligned initial
 * SP; the stack grows down toward the guard, so overflow is a translation fault
 * with FAR in the guard page). All-or-nothing: on any failure every stack page
 * this call mapped is unmapped again and *out_top is 0. Errors are those of
 * je_arm_mmu_map_page; a guard page whose leaf is valid is JE_ERR_INVALID.
 *
 * je_arm_mmu_stack_unmap is the reap path: pre-checks that the guard is still
 * unmapped and all four stack leaves are valid runtime leaves, then unmaps all
 * four (nothing is changed if any pre-check fails). Only after JE_OK may the
 * caller return the run to the allocator. Same caller context as above.
 */
#define JE_ARM_STACK_GUARD_PAGES 1u
#define JE_ARM_STACK_PAGES 4u
#define JE_ARM_STACK_RUN_PAGES (JE_ARM_STACK_GUARD_PAGES + JE_ARM_STACK_PAGES)
je_status je_arm_mmu_stack_map(je_arm_mmu *mmu, uint64_t run_base,
                               uint64_t *out_top);
je_status je_arm_mmu_stack_unmap(je_arm_mmu *mmu, uint64_t run_base);

#endif /* JEIOS_AARCH64_PAGING_H */
