/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_VM_H
#define JEIOS_VM_H
/*
 * M3a issue #3: per-process address-space builder and bounded image loader.
 *
 * SCOPE (ADR 0007, amended 2026-10-17): PROVEN HOSTED-ONLY. This models
 * page-table contents, the per-frame alias state machine, ownership, admission
 * arithmetic and transactional rollback. It makes NO claim about real CR3
 * switching, TLB behaviour, invlpg/CR3 ordering, CPL3 execution, or U/S/NX
 * hardware enforcement; those are issue #4 and are verified in QEMU. The backend
 * (je_vm_ops) is injected so the same builder drives a hosted fake in tests and
 * (in #4) a real arch backend. Single BSP caller, interrupts disabled; no
 * SMP/TLB-shootdown contract here.
 *
 * Source-of-truth rules the contract depends on:
 *   - The builder NEVER passes a raw physical address to a mutating backend op.
 *     Frames are named by je_vm_ref (process, extent, page); the backend derives
 *     kind, owner and permitted permission from the extent it registered.
 *   - Fallible build ops may fail and are journalled. Undo ops MUST NOT fail on
 *     builder-produced input; the backend panics (ops->panic) if one does, which
 *     is why they return void.
 *
 * On-wire note: je_vm_image_validate here validates a host-layout struct for the
 * hosted model. The _Static_asserts below pin field sizes/offsets so a real
 * little-endian on-wire decoder (issue #4 / #5) can be checked against the same
 * layout. #3 does NOT decode a byte blob; it asserts layout only.
 */
#include <stddef.h>
#include <jeios/alloc.h>
#include <jeios/status.h>

/* User arena is the half-open range [JE_VM_ARENA_BASE, +JE_VM_ARENA_SIZE). */
#define JE_VM_ARENA_BASE   UINT64_C(0x40000000)
#define JE_VM_ARENA_SIZE   UINT64_C(0x00200000) /* 2 MiB */
#define JE_VM_ARENA_PAGES  (JE_VM_ARENA_SIZE / JE_PAGE_SIZE) /* 512 */
/* Stack is the top JE_VM_STACK_PAGES of the arena; one guard page sits below it.
 * Both are arena-relative byte offsets. Image segments may not touch either. */
#define JE_VM_STACK_PAGES  4u
#define JE_VM_STACK_OFFSET (JE_VM_ARENA_SIZE - JE_VM_STACK_PAGES * JE_PAGE_SIZE) /* 0x1fc000 */
#define JE_VM_GUARD_OFFSET (JE_VM_STACK_OFFSET - JE_PAGE_SIZE)                   /* 0x1fb000 */

/* Admission bounds (ADR D4). 73-page quota = 4 table + <=64 user + 5 kstack. */
#define JE_VM_MAX_PROCESSES   2u
#define JE_VM_TABLE_PAGES     4u   /* PML4, PDPT, user PD, user PT (one extent) */
#define JE_VM_MAX_USER_PAGES  64u  /* image + stack, per process */
#define JE_VM_KSTACK_PAGES    5u   /* reserved+held here, allocated in #4 */
#define JE_VM_MAX_SEGMENTS    8u
#define JE_VM_QUOTA_PAGES (JE_VM_TABLE_PAGES + JE_VM_MAX_USER_PAGES + JE_VM_KSTACK_PAGES)
_Static_assert(JE_VM_QUOTA_PAGES == 73u, "admission quota");

/* Frame kind and the single permitted leaf permission per kind:
 *   USER_CODE    -> installed RX;    identity alias demoted RW->RO_NX on publish.
 *   USER_RODATA  -> installed R/NX;  identity alias demoted RW->RO_NX on publish
 *                   (constants are not writable; ADR D3 "constants R/NX").
 *   USER_DATA    -> installed RW/NX; identity alias stays RW (scrub/recycle).
 *   TABLE        -> never populates a user PTE.
 * Both USER_CODE and USER_RODATA are "no writable user-visible mapping" frames;
 * only USER_CODE is executable. Neither keeps a writable alias after publish. */
typedef enum {
    JE_VM_USER_CODE = 1, JE_VM_USER_RODATA = 2, JE_VM_USER_DATA = 3, JE_VM_TABLE = 4
} je_vm_kind;

/* Image permission bitmask (ADR D8): reject unknown bits, 0, W|X, and X-or-W
 * without R. Legal user combinations and their kind:
 *   R|X -> USER_CODE     R   -> USER_RODATA     R|W -> USER_DATA  */
#define JE_VM_R 0x1u
#define JE_VM_W 0x2u
#define JE_VM_X 0x4u
#define JE_VM_PERM_BITS (JE_VM_R | JE_VM_W | JE_VM_X)

/* Modeled identity-alias state of a physical frame. RW is the allocator-facing
 * default; RO_NX is applied to a published USER_CODE/USER_RODATA frame so no
 * writable alias survives. The kernel-stack GUARD path is separate
 * (je_arch_page_guard/restore) and is NOT driven through this interface. */
typedef enum { JE_VM_ALIAS_RW = 0, JE_VM_ALIAS_RO_NX = 1 } je_vm_alias;

/* Modeled user leaf permission (hosted model; #4 maps to real PTE bits). */
typedef enum { JE_VM_PERM_RX = 1, JE_VM_PERM_R_NX = 2, JE_VM_PERM_RW_NX = 3 } je_vm_perm;

/* --- Image descriptor: fixed-width, explicitly laid out. --------------------
 * target_offset is arena-relative. source_size bytes are copied from the blob;
 * (memory_size - source_size) trailing bytes are BSS (already zero). */
typedef struct {
    uint64_t source_offset;  /* into blob */
    uint64_t source_size;    /* <= memory_size; initialized bytes */
    uint64_t target_offset;  /* arena-relative, page-aligned */
    uint64_t memory_size;    /* page-aligned span, > 0 */
    uint32_t perm;           /* JE_VM_R/W/X bitmask */
    uint32_t reserved;       /* must be 0 */
} je_vm_segment;

typedef struct {
    uint32_t version;        /* must be JE_VM_IMAGE_VERSION */
    uint32_t segment_count;  /* 1..JE_VM_MAX_SEGMENTS */
    uint64_t entry_offset;   /* arena-relative; inside an RX segment's source */
    uint64_t reserved;       /* must be 0 */
    je_vm_segment segments[JE_VM_MAX_SEGMENTS];
} je_vm_image;
#define JE_VM_IMAGE_VERSION 1u

_Static_assert(sizeof(je_vm_segment) == 40, "je_vm_segment size");
_Static_assert(offsetof(je_vm_segment, source_offset) == 0, "seg src_off");
_Static_assert(offsetof(je_vm_segment, source_size) == 8, "seg src_size");
_Static_assert(offsetof(je_vm_segment, target_offset) == 16, "seg tgt_off");
_Static_assert(offsetof(je_vm_segment, memory_size) == 24, "seg mem_size");
_Static_assert(offsetof(je_vm_segment, perm) == 32, "seg perm");
_Static_assert(offsetof(je_vm_segment, reserved) == 36, "seg reserved");
_Static_assert(offsetof(je_vm_image, version) == 0, "img version");
_Static_assert(offsetof(je_vm_image, segment_count) == 4, "img seg_count");
_Static_assert(offsetof(je_vm_image, entry_offset) == 8, "img entry");
_Static_assert(offsetof(je_vm_image, reserved) == 16, "img reserved");
_Static_assert(offsetof(je_vm_image, segments) == 24, "img segments");
_Static_assert(sizeof(je_vm_image) == 24 + 40 * JE_VM_MAX_SEGMENTS, "je_vm_image size");

/* Owned frame reference: never a raw physical address. */
typedef struct { uint32_t process; uint32_t extent; uint32_t page; } je_vm_ref;

/* --- Injected backend. -------------------------------------------------------
 * Fallible build ops (journalled): reserve_hold, register_extent, map (one page),
 * link_tables, install_pte, demote_alias.
 * Undo ops (void, panic on misuse): unmap, clear_pte, restore_alias,
 * free_extent, release_hold, drop_tables, flush.
 * Read-only (audit): read_user_pte, read_upper (level,index), read_alias,
 * extent_frame_phys.
 *
 * reserve_hold/release_hold hold a CONCRETE contiguous run of N pages, not just
 * a count. The real backend calls je_pages_alloc(N) (added nothing to M2; reuses
 * the existing allocator) and returns the run base in *out_physical; the page is
 * never mapped — it is handed to #4, which takes over the exact run instead of
 * allocating again, so #4 cannot fail after #3 admits even under fragmentation.
 * release_hold frees that exact run. The hosted fake uses the same je_pages.
 *
 * map maps exactly ONE page of a registered frame and returns a writable
 * pointer or NULL (no side effects on NULL). It rejects TABLE frames. The caller
 * zeroes the whole page, then copies source bytes, before install.
 *
 * demote_alias performs a compare-and-set RW->RO_NX on a USER_CODE or
 * USER_RODATA frame only (je_user_alias_set in the real backend). restore_alias
 * is the infallible RO_NX->RW undo for those frames.
 *
 * register_extent returns a single PAGE-ALIGNED CONTIGUOUS physical run: frame i
 * sits at base + i*JE_PAGE_SIZE (base = extent_frame_phys(.., page 0)). The real
 * backend gets this from je_pages_alloc; the audit relies on it to resolve a
 * physical address back to an owning frame by range. A backend that cannot
 * provide a contiguous run must fail register_extent. */
typedef struct je_vm_ops je_vm_ops;
struct je_vm_ops {
    void *context;
    /* fallible */
    je_status (*reserve_hold)(void *ctx, size_t pages, uint64_t *out_physical);
    je_status (*register_extent)(void *ctx, uint32_t process, size_t pages,
                                 je_vm_kind kind, uint32_t *out_extent);
    void     *(*map)(void *ctx, je_vm_ref frame); /* one page */
    je_status (*link_tables)(void *ctx, uint32_t process, uint32_t table_extent,
                             uint32_t *out_root);
    je_status (*install_pte)(void *ctx, je_vm_ref frame, uint64_t varena_off);
    je_status (*demote_alias)(void *ctx, je_vm_ref frame);
    /* undo (infallible on builder-produced input; panic otherwise) */
    void (*unmap)(void *ctx, je_vm_ref frame);
    void (*clear_pte)(void *ctx, uint32_t process, uint64_t varena_off);
    void (*restore_alias)(void *ctx, je_vm_ref frame);
    void (*free_extent)(void *ctx, uint32_t process, uint32_t extent);
    void (*release_hold)(void *ctx, uint64_t physical, size_t pages);
    void (*drop_tables)(void *ctx, uint32_t process);
    void (*flush)(void *ctx);
    /* read-only (audit) */
    je_status (*read_user_pte)(void *ctx, uint32_t process, uint64_t varena_off,
                               uint64_t *out_phys, je_vm_perm *out_perm,
                               int *out_present);
    /* level: 4=PML4,3=PDPT,2=PD,1=PT. index: entry within that table. flags:
     * bit0 P, bit1 W, bit2 U, bit3 NX. */
    je_status (*read_upper)(void *ctx, uint32_t process, int level, uint32_t index,
                            uint32_t *out_flags);
    je_status (*read_alias)(void *ctx, je_vm_ref frame, je_vm_alias *out);
    je_status (*extent_frame_phys)(void *ctx, uint32_t process, uint32_t extent,
                                   uint32_t page, uint64_t *out_phys);
    /* Fatal invariant hook. The builder only ever calls this on an impossible
     * state (an undo op failed on builder-produced input); it must not return.
     * Declared as a plain pointer because C11 forbids _Noreturn on a member;
     * the builder invokes it via je_vm_panic() which is _Noreturn. */
    void (*panic)(void *ctx, const char *why);
};

typedef enum { JE_VM_FREE = 0, JE_VM_BUILDING, JE_VM_READY } je_vm_state;

typedef enum {
    JE_VM_J_HOLD = 1, JE_VM_J_EXTENT, JE_VM_J_MAP, JE_VM_J_TABLES,
    JE_VM_J_PTE, JE_VM_J_DEMOTE
} je_vm_journal_kind;
typedef struct {
    je_vm_journal_kind kind;
    uint32_t extent;       /* EXTENT/MAP/PTE/DEMOTE */
    uint32_t page;         /* MAP/PTE/DEMOTE (one page) */
    uint64_t varena_off;   /* PTE */
    uint64_t physical;     /* HOLD run base */
    size_t   pages;        /* HOLD */
} je_vm_journal_entry;

typedef struct {
    uint64_t physical;
    size_t pages;
    je_vm_kind kind;
    uint32_t backend_id;   /* id returned by register_extent; used by audit */
} je_vm_extent;

#define JE_VM_MAX_EXTENTS (JE_VM_MAX_SEGMENTS + 2u) /* segments + stack + tables */
/* Worst case journal: 1 HOLD + 1 TABLES + up to MAX_USER_PAGES each of MAP, PTE
 * and DEMOTE, plus the per-extent registrations. 1+1+64*3 = 194 <= bound. */
#define JE_VM_MAX_JOURNAL (JE_VM_MAX_EXTENTS + JE_VM_MAX_USER_PAGES * 3u + 4u)

typedef struct {
    je_vm_state state;
    uint32_t    root;
    uint64_t    entry;             /* arena-relative, validated */
    uint64_t    kstack_physical;   /* concrete 5-page run held for #4 */
    size_t      held_kstack;       /* pages held (JE_VM_KSTACK_PAGES or 0) */
    size_t      extent_count;
    je_vm_extent extents[JE_VM_MAX_EXTENTS];
    size_t      journal_count;
    je_vm_journal_entry journal[JE_VM_MAX_JOURNAL];
} je_vm_process;

typedef struct {
    const je_vm_ops *ops;
    je_pages *pages;               /* admission arithmetic; same source the
                                    * backend allocates/holds from (single truth) */
    size_t    reserve_pages;       /* min free pages admission must leave; 0 ok */
    je_vm_process procs[JE_VM_MAX_PROCESSES];
} je_vm;

je_status je_vm_init(je_vm *vm, const je_vm_ops *ops, je_pages *pages,
                     size_t reserve_pages);

/* Pure validation, no allocation/side effects. JE_ERR_UNSUPPORTED on version
 * mismatch; JE_ERR_INVALID on any other malformed field or arithmetic. Bounds
 * are subtraction-based and applied before any page rounding. Overlap is tested
 * at page granule; segments must not touch the stack/guard pages; entry_offset
 * must lie in an RX segment's initialized (source) bytes. */
je_status je_vm_image_validate(const je_vm_image *image, size_t blob_len);

/* Admit + build, or unwind completely. Check order: (1) nx_available (D2,
 * JE_ERR_UNSUPPORTED), (2) free process slot (JE_ERR_BUSY), (3) image validation,
 * (4) atomic admission with need = JE_VM_TABLE_PAGES + user_pages +
 * JE_VM_KSTACK_PAGES, accepted iff need <= free_count &&
 * free_count - need >= reserve_pages (JE_ERR_NOMEM; no underflow). Admission
 * counts pages, not contiguity, so a later register_extent/reserve_hold may
 * still return JE_ERR_NOMEM on fragmentation; the journal unwinds that cleanly.
 * Only then mutate. On any later failure the journal unwinds in reverse and no
 * slot is left non-FREE. Publication per ADR D3. */
je_status je_vm_build(je_vm *vm, const je_vm_image *image, const void *blob,
                      size_t blob_len, int nx_available, uint32_t *out_index);

/* Tear down a READY process, reverse of publication; releases the held kstack
 * run. Returns JE_ERR_INVALID only for a bad index/non-READY slot; backend undo
 * failures panic. */
je_status je_vm_destroy(je_vm *vm, uint32_t index);

/* Audit oracle (see this header's design notes and ADR 0007). JE_OK iff every
 * frozen invariant holds for process `index`, including cross-process root/
 * extent disjointness. */
je_status je_vm_audit(const je_vm *vm, uint32_t index);

/* M3a #4: the real arch backend (kernel/arch/x86_64/vm_backend.c) implementing
 * the ops above against actual page tables. Returns a singleton ops vector bound
 * to `pages` (the same allocator admission uses). CPL0 construction only in #4
 * step 2; CR3 switching/ring-3 are later steps. Not part of the hosted #3
 * contract — hosted tests use their own fake ops. */
const je_vm_ops *je_arch_vm_ops(je_pages *pages, int nx_available);

#endif
