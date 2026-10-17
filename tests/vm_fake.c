/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hosted fake je_vm_ops backend for the M3a issue #3 unit test. TEST SUPPORT
 * ONLY: models page-table contents, frame ownership and the identity-alias state
 * machine in plain host memory. It proves nothing about CR3, TLB, invlpg, CPL3
 * or U/S/NX hardware enforcement (issue #4, QEMU).
 *
 * Hooks (the test declares these itself):
 *   je_vm_fake_init(ops, pages)  reset the model, bind it to `pages`, fill ops.
 *                                Disarms injection and zeroes the counter.
 *   je_vm_fake_fail_at(n)        zero the counter and arm injection: the n-th
 *                                fallible op after this call fails (one-shot,
 *                                0 = never). The op is counted, then fails
 *                                BEFORE validation and records nothing.
 *   je_vm_fake_fallible_count()  fallible ops attempted since the last reset
 *                                (init or fail_at), including rejected ones.
 *   je_vm_fake_peek(frame)       read-only view of the frame's one host backing
 *                                page, whatever its alias/map/install state;
 *                                changes no state. NULL if not registered.
 * Injected status: JE_ERR_NOMEM for reserve_hold/register_extent/link_tables,
 * JE_ERR_IO for install_pte/demote_alias, NULL for map.
 *
 * Contract notes / pushback for the header owner (none block this fake):
 *  P1. Extent-ID namespace is unspecified. This fake uses PER-PROCESS ids, the
 *      lowest free slot in [0, JE_VM_MAX_EXTENTS), so a builder that indexes
 *      its own extents[] by the returned id stays in bounds. Consequence: a
 *      "wrong-process" frame is not expressible — je_vm_ref.process selects the
 *      namespace and install_pte has no separate target process, so the PTE
 *      always lands in the owner's tables. Recommend the header pin this.
 *  P2. ADR 0007 D4 says the owned-extents list "holds only user-PTE-populating
 *      frames", yet the header registers TABLE as an extent (JE_VM_MAX_EXTENTS
 *      counts "+ tables"). Kind TABLE keeps it unpopulatable, so this is
 *      wording drift, not a hole; the ADR or header comment should be aligned.
 *  P3. reserve_hold has no process argument, so holds are a global ledger
 *      (capacity JE_VM_MAX_PROCESSES) matched by exact (physical, pages).
 *
 * Ordering checks enforced from ADR 0007 D3 (state checks, not new ops):
 *  - install_pte requires linked tables (the PT must exist and be zeroed).
 *  - USER_CODE/RODATA: demote requires the frame unmapped and not installed;
 *    install requires alias RO_NX and the frame unmapped, so a user-visible
 *    mapping and a writable alias never coexist. map refuses an RO_NX frame.
 *  - A frame is installed at most once; the guard page is never installed.
 *  - Teardown: restore_alias requires the PTE cleared; free_extent requires
 *    every page unmapped, uninstalled and alias RW (restore-before-free);
 *    drop_tables requires no present leaf; a linked TABLE extent cannot be freed.
 *  - Per-process user pages are capped at JE_VM_MAX_USER_PAGES (JE_ERR_DENIED),
 *    one TABLE extent of exactly JE_VM_TABLE_PAGES per process.
 * flush is modeled as a counter only (no TLB claim). Misuse of an undo op calls
 * ops->panic (which a test may replace, e.g. with longjmp); abort() backstops
 * a hook that returns.
 *
 * Table pages hold real x86-64-shaped entries (address | P,W,U,NX) in their host
 * buffers; PDPT[0] models the shared supervisor kernel branch with flags P|W and
 * address 0 because the kernel PD is not owned by the process. User frames are
 * filled with 0xA5 on registration so the caller's mandatory zeroing is visible.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jeios/vm.h>

void je_vm_fake_init(je_vm_ops *ops, je_pages *pages);
void je_vm_fake_fail_at(unsigned n);
size_t je_vm_fake_fallible_count(void);
const unsigned char *je_vm_fake_peek(je_vm_ref frame);

#define E_P  UINT64_C(0x1)
#define E_W  UINT64_C(0x2)
#define E_U  UINT64_C(0x4)
#define E_NX (UINT64_C(1) << 63)
#define E_ADDR UINT64_C(0x000ffffffffff000)
#define ENTRIES 512u

typedef struct {
    void *page;            /* host backing, one page */
    je_vm_alias alias;
    bool mapped;
    bool installed;
} fake_frame;

typedef struct {
    bool used;
    je_vm_kind kind;
    uint64_t physical;
    size_t pages;
    fake_frame frame[JE_VM_MAX_USER_PAGES];
} fake_extent;

typedef struct { bool present; uint32_t extent, page; } fake_leaf;

typedef struct {
    bool linked;
    uint32_t table_extent;
    size_t user_pages;
    fake_extent extent[JE_VM_MAX_EXTENTS];
    fake_leaf leaf[JE_VM_ARENA_PAGES];
} fake_proc;

typedef struct { bool used; uint64_t physical; size_t pages; } fake_hold;

static struct {
    je_vm_ops *ops;
    je_pages *pages;
    size_t attempts;
    unsigned fail_at;
    size_t flushes;
    fake_hold hold[JE_VM_MAX_PROCESSES];
    fake_proc proc[JE_VM_MAX_PROCESSES];
} g;

static _Noreturn void fake_die(const char *why)
{
    if (g.ops && g.ops->panic) g.ops->panic(g.ops->context, why);
    fprintf(stderr, "vm_fake panic: %s\n", why);
    abort();
}

static void fake_default_panic(void *ctx, const char *why)
{
    (void)ctx;
    fprintf(stderr, "vm_fake panic: %s\n", why);
    abort();
}

/* Count every fallible attempt; true means "inject a failure now". */
static bool fake_inject(void)
{
    ++g.attempts;
    return g.fail_at != 0 && g.attempts == (size_t)g.fail_at;
}

static fake_proc *proc_of(void *ctx, uint32_t process)
{
    if (ctx != &g || process >= JE_VM_MAX_PROCESSES) return NULL;
    return &g.proc[process];
}

static fake_extent *extent_of(void *ctx, uint32_t process, uint32_t extent)
{
    fake_proc *p = proc_of(ctx, process);
    if (!p || extent >= JE_VM_MAX_EXTENTS || !p->extent[extent].used) return NULL;
    return &p->extent[extent];
}

static fake_frame *frame_of(void *ctx, je_vm_ref r, fake_extent **out_extent)
{
    fake_extent *e = extent_of(ctx, r.process, r.extent);
    if (!e || r.page >= e->pages) return NULL;
    if (out_extent) *out_extent = e;
    return &e->frame[r.page];
}

static bool user_kind(je_vm_kind k)
{
    return k == JE_VM_USER_CODE || k == JE_VM_USER_RODATA || k == JE_VM_USER_DATA;
}

static bool demotable(je_vm_kind k)
{
    return k == JE_VM_USER_CODE || k == JE_VM_USER_RODATA;
}

static bool arena_page_ok(uint64_t off)
{
    return off < JE_VM_ARENA_SIZE && (off & (JE_PAGE_SIZE - 1u)) == 0;
}

/* level 4=PML4 (page 0) .. 1=PT (page 3) of the linked table extent. */
static uint64_t *table_of(fake_proc *p, int level)
{
    return (uint64_t *)p->extent[p->table_extent].frame[4 - level].page;
}

static uint64_t table_phys(const fake_proc *p, int level)
{
    return p->extent[p->table_extent].physical + (uint64_t)(4 - level) * JE_PAGE_SIZE;
}

static void release_backing(fake_extent *e)
{
    for (size_t i = 0; i < e->pages; ++i) {
        free(e->frame[i].page);
        e->frame[i].page = NULL;
    }
}

/* --- fallible ---------------------------------------------------------- */

static je_status fake_reserve_hold(void *ctx, size_t pages, uint64_t *out_physical)
{
    if (fake_inject()) return JE_ERR_NOMEM;
    if (ctx != &g || !out_physical || pages == 0) return JE_ERR_INVALID;
    size_t slot = 0;
    while (slot < JE_VM_MAX_PROCESSES && g.hold[slot].used) ++slot;
    if (slot == JE_VM_MAX_PROCESSES) return JE_ERR_NOMEM;
    uint64_t physical;
    je_status s = je_pages_alloc(g.pages, pages, &physical);
    if (s != JE_OK) return s;
    g.hold[slot] = (fake_hold){true, physical, pages};
    *out_physical = physical;
    return JE_OK;
}

static je_status fake_register_extent(void *ctx, uint32_t process, size_t pages,
                                      je_vm_kind kind, uint32_t *out_extent)
{
    if (fake_inject()) return JE_ERR_NOMEM;
    fake_proc *p = proc_of(ctx, process);
    if (!p || !out_extent || pages == 0) return JE_ERR_INVALID;
    if (kind == JE_VM_TABLE) {
        if (pages != JE_VM_TABLE_PAGES) return JE_ERR_INVALID;
        for (size_t i = 0; i < JE_VM_MAX_EXTENTS; ++i)
            if (p->extent[i].used && p->extent[i].kind == JE_VM_TABLE) return JE_ERR_BUSY;
    } else if (!user_kind(kind)) {
        return JE_ERR_INVALID;
    } else if (pages > JE_VM_MAX_USER_PAGES - p->user_pages) {
        return JE_ERR_DENIED;
    }
    uint32_t slot = 0;
    while (slot < JE_VM_MAX_EXTENTS && p->extent[slot].used) ++slot;
    if (slot == JE_VM_MAX_EXTENTS) return JE_ERR_NOMEM;
    fake_extent *e = &p->extent[slot];
    *e = (fake_extent){0};
    e->pages = pages;
    for (size_t i = 0; i < pages; ++i) {
        e->frame[i].page = aligned_alloc((size_t)JE_PAGE_SIZE, (size_t)JE_PAGE_SIZE);
        if (!e->frame[i].page) {
            release_backing(e);
            *e = (fake_extent){0};
            return JE_ERR_NOMEM;
        }
        memset(e->frame[i].page, 0xa5, (size_t)JE_PAGE_SIZE);
        e->frame[i].alias = JE_VM_ALIAS_RW;
    }
    uint64_t physical;
    je_status s = je_pages_alloc(g.pages, pages, &physical);
    if (s != JE_OK) {
        release_backing(e);
        *e = (fake_extent){0};
        return s;
    }
    e->used = true;
    e->kind = kind;
    e->physical = physical;
    if (kind != JE_VM_TABLE) p->user_pages += pages;
    *out_extent = slot;
    return JE_OK;
}

static void *fake_map(void *ctx, je_vm_ref frame)
{
    if (fake_inject()) return NULL;
    fake_extent *e;
    fake_frame *f = frame_of(ctx, frame, &e);
    if (!f || e->kind == JE_VM_TABLE || f->mapped || f->alias != JE_VM_ALIAS_RW)
        return NULL;
    f->mapped = true;
    return f->page;
}

static je_status fake_link_tables(void *ctx, uint32_t process, uint32_t table_extent,
                                  uint32_t *out_root)
{
    if (fake_inject()) return JE_ERR_NOMEM;
    fake_proc *p = proc_of(ctx, process);
    fake_extent *e = extent_of(ctx, process, table_extent);
    if (!p || !e || !out_root || e->kind != JE_VM_TABLE) return JE_ERR_INVALID;
    if (p->linked) return JE_ERR_BUSY;
    for (size_t i = 0; i < JE_VM_TABLE_PAGES; ++i)
        memset(e->frame[i].page, 0, (size_t)JE_PAGE_SIZE);
    p->table_extent = table_extent;
    p->linked = true;
    table_of(p, 4)[0] = table_phys(p, 3) | E_P | E_W | E_U;
    table_of(p, 3)[0] = E_P | E_W;                 /* kernel branch, U=0 */
    table_of(p, 3)[1] = table_phys(p, 2) | E_P | E_W | E_U;
    table_of(p, 2)[0] = table_phys(p, 1) | E_P | E_W | E_U;
    *out_root = (uint32_t)table_phys(p, 4);         /* < JE_PHYSICAL_LIMIT */
    return JE_OK;
}

static je_status fake_install_pte(void *ctx, je_vm_ref frame, uint64_t varena_off)
{
    if (fake_inject()) return JE_ERR_IO;
    fake_extent *e;
    fake_frame *f = frame_of(ctx, frame, &e);
    if (!f || !arena_page_ok(varena_off)) return JE_ERR_INVALID;
    fake_proc *p = &g.proc[frame.process];
    if (e->kind == JE_VM_TABLE || varena_off == JE_VM_GUARD_OFFSET) return JE_ERR_DENIED;
    if (!p->linked) return JE_ERR_INVALID;
    size_t index = (size_t)(varena_off / JE_PAGE_SIZE);
    if (p->leaf[index].present || f->installed) return JE_ERR_BUSY;
    if (demotable(e->kind) && (f->mapped || f->alias != JE_VM_ALIAS_RO_NX))
        return JE_ERR_BUSY;
    uint64_t bits = E_P | E_U;
    if (e->kind == JE_VM_USER_RODATA) bits |= E_NX;
    if (e->kind == JE_VM_USER_DATA) bits |= E_W | E_NX;
    table_of(p, 1)[index] = (e->physical + frame.page * JE_PAGE_SIZE) | bits;
    p->leaf[index] = (fake_leaf){true, frame.extent, frame.page};
    f->installed = true;
    return JE_OK;
}

static je_status fake_demote_alias(void *ctx, je_vm_ref frame)
{
    if (fake_inject()) return JE_ERR_IO;
    fake_extent *e;
    fake_frame *f = frame_of(ctx, frame, &e);
    if (!f) return JE_ERR_INVALID;
    if (!demotable(e->kind)) return JE_ERR_DENIED;
    if (f->mapped || f->installed || f->alias != JE_VM_ALIAS_RW) return JE_ERR_BUSY;
    f->alias = JE_VM_ALIAS_RO_NX;
    return JE_OK;
}

/* --- undo (panic on misuse) -------------------------------------------- */

static void fake_unmap(void *ctx, je_vm_ref frame)
{
    fake_frame *f = frame_of(ctx, frame, NULL);
    if (!f || !f->mapped) fake_die("unmap: frame not mapped");
    f->mapped = false;
}

static void fake_clear_pte(void *ctx, uint32_t process, uint64_t varena_off)
{
    fake_proc *p = proc_of(ctx, process);
    if (!p || !p->linked || !arena_page_ok(varena_off))
        fake_die("clear_pte: bad process or offset");
    size_t index = (size_t)(varena_off / JE_PAGE_SIZE);
    fake_leaf l = p->leaf[index];
    if (!l.present) fake_die("clear_pte: leaf not present");
    p->extent[l.extent].frame[l.page].installed = false;
    p->leaf[index] = (fake_leaf){0};
    table_of(p, 1)[index] = 0;
}

static void fake_restore_alias(void *ctx, je_vm_ref frame)
{
    fake_extent *e;
    fake_frame *f = frame_of(ctx, frame, &e);
    if (!f || !demotable(e->kind) || f->installed || f->mapped ||
        f->alias != JE_VM_ALIAS_RO_NX)
        fake_die("restore_alias: frame not a demoted, uninstalled code/rodata frame");
    f->alias = JE_VM_ALIAS_RW;
}

static void fake_free_extent(void *ctx, uint32_t process, uint32_t extent)
{
    fake_proc *p = proc_of(ctx, process);
    fake_extent *e = extent_of(ctx, process, extent);
    if (!p || !e) fake_die("free_extent: unknown extent");
    if (e->kind == JE_VM_TABLE && p->linked && p->table_extent == extent)
        fake_die("free_extent: tables still linked");
    for (size_t i = 0; i < e->pages; ++i) {
        const fake_frame *f = &e->frame[i];
        if (f->mapped || f->installed || f->alias != JE_VM_ALIAS_RW)
            fake_die("free_extent: page mapped, installed or alias not RW");
    }
    if (je_pages_free(g.pages, e->physical, e->pages) != JE_OK)
        fake_die("free_extent: je_pages_free rejected run");
    if (e->kind != JE_VM_TABLE) p->user_pages -= e->pages;
    release_backing(e);
    *e = (fake_extent){0};
}

static void fake_release_hold(void *ctx, uint64_t physical, size_t pages)
{
    if (ctx != &g) fake_die("release_hold: bad context");
    for (size_t i = 0; i < JE_VM_MAX_PROCESSES; ++i) {
        fake_hold *h = &g.hold[i];
        if (h->used && h->physical == physical && h->pages == pages) {
            if (je_pages_free(g.pages, physical, pages) != JE_OK)
                fake_die("release_hold: je_pages_free rejected run");
            *h = (fake_hold){0};
            return;
        }
    }
    fake_die("release_hold: no exact matching hold");
}

static void fake_drop_tables(void *ctx, uint32_t process)
{
    fake_proc *p = proc_of(ctx, process);
    if (!p || !p->linked) fake_die("drop_tables: tables not linked");
    for (size_t i = 0; i < JE_VM_ARENA_PAGES; ++i)
        if (p->leaf[i].present) fake_die("drop_tables: leaf still present");
    for (int level = 1; level <= 4; ++level)
        memset(table_of(p, level), 0, (size_t)JE_PAGE_SIZE);
    p->linked = false;
    p->table_extent = 0;
}

static void fake_flush(void *ctx)
{
    if (ctx != &g) fake_die("flush: bad context");
    ++g.flushes;
}

/* --- read-only (audit) -------------------------------------------------- */

static je_status fake_read_user_pte(void *ctx, uint32_t process, uint64_t varena_off,
                                    uint64_t *out_phys, je_vm_perm *out_perm,
                                    int *out_present)
{
    fake_proc *p = proc_of(ctx, process);
    if (!p || !p->linked || !arena_page_ok(varena_off) || !out_phys || !out_perm ||
        !out_present)
        return JE_ERR_INVALID;
    uint64_t pte = table_of(p, 1)[varena_off / JE_PAGE_SIZE];
    if (!(pte & E_P)) {
        *out_phys = 0;
        *out_perm = (je_vm_perm)0;
        *out_present = 0;
        return JE_OK;
    }
    je_vm_perm perm;
    if (!(pte & E_U)) return JE_ERR_INVALID;
    if (!(pte & E_W) && !(pte & E_NX)) perm = JE_VM_PERM_RX;
    else if (!(pte & E_W)) perm = JE_VM_PERM_R_NX;
    else if (pte & E_NX) perm = JE_VM_PERM_RW_NX;
    else return JE_ERR_INVALID; /* W+X: never produced by this model */
    *out_phys = pte & E_ADDR;
    *out_perm = perm;
    *out_present = 1;
    return JE_OK;
}

static je_status fake_read_upper(void *ctx, uint32_t process, int level, uint32_t index,
                                 uint32_t *out_flags)
{
    fake_proc *p = proc_of(ctx, process);
    if (!p || !p->linked || level < 1 || level > 4 || index >= ENTRIES || !out_flags)
        return JE_ERR_INVALID;
    uint64_t entry = table_of(p, level)[index];
    uint32_t flags = 0;
    if (entry & E_P) flags |= 1u;
    if (entry & E_W) flags |= 2u;
    if (entry & E_U) flags |= 4u;
    if (entry & E_NX) flags |= 8u;
    *out_flags = flags;
    return JE_OK;
}

static je_status fake_read_alias(void *ctx, je_vm_ref frame, je_vm_alias *out)
{
    fake_frame *f = frame_of(ctx, frame, NULL);
    if (!f || !out) return JE_ERR_INVALID;
    *out = f->alias;
    return JE_OK;
}

static je_status fake_extent_frame_phys(void *ctx, uint32_t process, uint32_t extent,
                                        uint32_t page, uint64_t *out_phys)
{
    fake_extent *e = extent_of(ctx, process, extent);
    if (!e || page >= e->pages || !out_phys) return JE_ERR_INVALID;
    *out_phys = e->physical + page * JE_PAGE_SIZE;
    return JE_OK;
}

/* --- hooks --------------------------------------------------------------- */

void je_vm_fake_init(je_vm_ops *ops, je_pages *pages)
{
    if (!ops || !pages) {
        fprintf(stderr, "vm_fake: init with NULL ops/pages\n");
        abort();
    }
    /* Drop host backing left by a previous run; its je_pages state belongs to
     * the caller, who may have reinitialized it, so it is not touched. */
    for (size_t i = 0; i < JE_VM_MAX_PROCESSES; ++i)
        for (size_t j = 0; j < JE_VM_MAX_EXTENTS; ++j)
            if (g.proc[i].extent[j].used) release_backing(&g.proc[i].extent[j]);
    memset(&g, 0, sizeof g);
    g.ops = ops;
    g.pages = pages;
    *ops = (je_vm_ops){
        .context = &g,
        .reserve_hold = fake_reserve_hold,
        .register_extent = fake_register_extent,
        .map = fake_map,
        .link_tables = fake_link_tables,
        .install_pte = fake_install_pte,
        .demote_alias = fake_demote_alias,
        .unmap = fake_unmap,
        .clear_pte = fake_clear_pte,
        .restore_alias = fake_restore_alias,
        .free_extent = fake_free_extent,
        .release_hold = fake_release_hold,
        .drop_tables = fake_drop_tables,
        .flush = fake_flush,
        .read_user_pte = fake_read_user_pte,
        .read_upper = fake_read_upper,
        .read_alias = fake_read_alias,
        .extent_frame_phys = fake_extent_frame_phys,
        .panic = fake_default_panic,
    };
}

void je_vm_fake_fail_at(unsigned n)
{
    g.attempts = 0;
    g.fail_at = n;
}

size_t je_vm_fake_fallible_count(void)
{
    return g.attempts;
}

const unsigned char *je_vm_fake_peek(je_vm_ref frame)
{
    const fake_frame *f = frame_of(&g, frame, NULL);
    return f ? (const unsigned char *)f->page : NULL;
}
