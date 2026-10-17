/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3a #4 step 2: the REAL arch backend for the je_vm_ops seam frozen in vm.h.
 *
 * The #3 builder (kernel/core/vm.c) drives this exactly as it drives the hosted
 * fake, but here every op mutates real page tables and real identity-alias
 * leaves. Produces a real per-process root whose physical address is a valid
 * CR3 value. This file is the ONLY writer of PROCESS tables; paging.c remains
 * the only writer of kernel tables and user identity aliases (the seam).
 *
 * SCOPE: CPL0-constructed and CPL0-exercised only. No ring-3 execution, no
 * sched.c CR3 switch, no TSS.RSP0 — those are step 3 and verified in QEMU.
 *
 * Topology each process (matches the frozen je_vm_audit, kernel/core/vm.c:389):
 *   PML4[0]  = PDPT        | P|U
 *   PDPT[0]  = kernel PD   | P        (U clear: kernel reachable supervisor-only)
 *   PDPT[1]  = PD          | P|U      (arena at 0x40000000 == PML4[0]/PDPT[1])
 *   PD[0]    = PT          | P|U      ((0x40000000>>21)&511 == 0)
 *   PT[i]    = user frame  | P|U|...  installed per kind
 * All four table frames are one TABLE extent (JE_VM_TABLE_PAGES == 4), ordered
 * PML4, PDPT, PD, PT. Frames are < JE_PHYSICAL_LIMIT so phys == identity VA and
 * the root fits a uint32_t out_root.
 */
#include <jeios/vm.h>
#include <jeios/paging.h>
#include <jeios/alloc.h>
#include <jeios/arch.h>

_Static_assert(JE_PHYSICAL_LIMIT <= JE_VM_ARENA_BASE,
    "owner-approved: kernel+RAM window must fit under PDPT[0] below the arena");
_Static_assert(JE_PHYSICAL_LIMIT <= UINT64_C(0x100000000),
    "owner-approved: process root physical must fit the uint32_t out_root");
_Static_assert(((JE_VM_ARENA_BASE >> 39) & 511) == 0, "arena PML4 index 0");
_Static_assert(((JE_VM_ARENA_BASE >> 30) & 511) == 1, "arena PDPT index 1");
_Static_assert(((JE_VM_ARENA_BASE >> 21) & 511) == 0, "arena PD index 0");

#define PTE_P 0x1u
#define PTE_W 0x2u
#define PTE_U 0x4u

/* Per-process backend record. extent ids are indices into `ext`. The table
 * extent, once linked, names the four table frames by position. */
typedef struct {
    int      used;
    uint64_t root_phys;        /* PML4 physical == CR3 value (0 until linked) */
    uint32_t table_extent;     /* extent id of the 4 table frames, or UINT32_MAX */
} vb_proc;

typedef struct {
    int        used;
    uint32_t   process;
    uint64_t   phys;           /* contiguous run base */
    size_t     pages;
    je_vm_kind kind;
} vb_extent;

typedef struct {
    je_pages  *pages;
    vb_proc    procs[JE_VM_MAX_PROCESSES];
    vb_extent  ext[JE_VM_MAX_PROCESSES * JE_VM_MAX_EXTENTS];
    uint64_t   nx;             /* NX bit mirror for leaf encoding, or 0 */
} vb_state;

static vb_state g_vb;

/* identity map: every frame handed out by the allocator is < 1 GiB and
 * identity-mapped by the kernel root, so phys doubles as a writable VA. */
static void *phys_ptr(uint64_t phys) { return (void *)(uintptr_t)phys; }

static vb_extent *ext_of(vb_state *s, uint32_t process, uint32_t id)
{
    if (id >= sizeof(s->ext)/sizeof(s->ext[0])) return NULL;
    vb_extent *e = &s->ext[id];
    if (!e->used || e->process != process) return NULL;
    return e;
}

/* ---- fallible build ops --------------------------------------------------- */

static je_status vb_reserve_hold(void *ctx, size_t pages, uint64_t *out_physical)
{
    vb_state *s = ctx;
    if (!out_physical || pages == 0) return JE_ERR_INVALID;
    return je_pages_alloc(s->pages, pages, out_physical); /* held, never mapped */
}

static je_status vb_register_extent(void *ctx, uint32_t process, size_t pages,
                                    je_vm_kind kind, uint32_t *out_extent)
{
    vb_state *s = ctx;
    if (!out_extent || process >= JE_VM_MAX_PROCESSES || pages == 0) return JE_ERR_INVALID;
    for (uint32_t i = 0; i < sizeof(s->ext)/sizeof(s->ext[0]); ++i) {
        if (s->ext[i].used) continue;
        uint64_t base;
        je_status st = je_pages_alloc(s->pages, pages, &base); /* contiguous */
        if (st != JE_OK) return st;
        s->ext[i] = (vb_extent){ .used = 1, .process = process, .phys = base,
                                 .pages = pages, .kind = kind };
        *out_extent = i;
        return JE_OK;
    }
    return JE_ERR_NOMEM;
}

static uint64_t frame_phys(vb_extent *e, uint32_t page)
{
    return e->phys + (uint64_t)page * JE_PAGE_SIZE;
}

static void *vb_map(void *ctx, je_vm_ref frame)
{
    vb_state *s = ctx;
    vb_extent *e = ext_of(s, frame.process, frame.extent);
    if (!e || e->kind == JE_VM_TABLE) return NULL;      /* never map a table */
    if (frame.page >= e->pages) return NULL;
    return phys_ptr(frame_phys(e, frame.page));          /* writable identity VA */
}

static je_status vb_link_tables(void *ctx, uint32_t process, uint32_t table_extent,
                                uint32_t *out_root)
{
    vb_state *s = ctx;
    if (!out_root || process >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    vb_extent *e = ext_of(s, process, table_extent);
    if (!e || e->kind != JE_VM_TABLE || e->pages != JE_VM_TABLE_PAGES) return JE_ERR_INVALID;
    uint64_t pml4 = frame_phys(e, 0), pdpt = frame_phys(e, 1),
             pd   = frame_phys(e, 2), pt   = frame_phys(e, 3);
    uint64_t *vpml4 = phys_ptr(pml4), *vpdpt = phys_ptr(pdpt), *vpd = phys_ptr(pd);
    /* je_pages_alloc returns zeroed frames; only the live entries are set. */
    /* Intermediate entries carry W|U; the effective permission is the AND of W
     * across all levels, so intermediates must allow writes and user access and
     * the LEAF bits decide the real per-page permission (RX / R / RW, U set).
     * PDPT[0] alone has U clear to keep the shared kernel subtree supervisor-
     * only. (The audit checks P and U at these levels, not W.) */
    vpml4[0] = pdpt | PTE_P | PTE_W | PTE_U;
    vpdpt[0] = je_kernel_pd_phys() | PTE_P | PTE_W;      /* U clear: kernel share */
    vpdpt[1] = pd   | PTE_P | PTE_W | PTE_U;
    vpd[0]   = pt   | PTE_P | PTE_W | PTE_U;
    s->procs[process].used = 1;
    s->procs[process].root_phys = pml4;
    s->procs[process].table_extent = table_extent;
    *out_root = (uint32_t)pml4;
    return JE_OK;
}

static je_status vb_install_pte(void *ctx, je_vm_ref frame, uint64_t varena_off)
{
    vb_state *s = ctx;
    if (frame.process >= JE_VM_MAX_PROCESSES) return JE_ERR_INVALID;
    vb_extent *e = ext_of(s, frame.process, frame.extent);
    vb_proc *p = &s->procs[frame.process];
    if (!e || e->kind == JE_VM_TABLE || !p->used) return JE_ERR_INVALID;
    if (frame.page >= e->pages || varena_off >= JE_VM_ARENA_SIZE || (varena_off & 4095))
        return JE_ERR_INVALID;
    vb_extent *te = ext_of(s, frame.process, p->table_extent);
    if (!te) return JE_ERR_INVALID;
    uint64_t *vpt = phys_ptr(frame_phys(te, 3));
    uint64_t idx = (varena_off >> 12) & 511;
    if (vpt[idx] & PTE_P) return JE_ERR_INVALID;         /* no double-install */
    uint64_t leaf = frame_phys(e, frame.page) | PTE_P | PTE_U;
    if (e->kind == JE_VM_USER_DATA) leaf |= PTE_W;       /* RW/NX */
    if (e->kind != JE_VM_USER_CODE) leaf |= s->nx;       /* NX unless executable */
    vpt[idx] = leaf;
    return JE_OK;
}

static je_status vb_demote_alias(void *ctx, je_vm_ref frame)
{
    vb_state *s = ctx;
    vb_extent *e = ext_of(s, frame.process, frame.extent);
    if (!e || (e->kind != JE_VM_USER_CODE && e->kind != JE_VM_USER_RODATA)) return JE_ERR_INVALID;
    if (frame.page >= e->pages) return JE_ERR_INVALID;
    return je_user_alias_set(frame_phys(e, frame.page),
                             JE_PAGING_ALIAS_RW, JE_PAGING_ALIAS_RO_NX);
}

/* ---- undo ops (void; panic on builder-impossible input) ------------------- */

static void vb_panic(void *ctx, const char *why);

static void vb_unmap(void *ctx, je_vm_ref frame) { (void)ctx; (void)frame; /* map had no side effects */ }

static void vb_clear_pte(void *ctx, uint32_t process, uint64_t varena_off)
{
    vb_state *s = ctx;
    if (process >= JE_VM_MAX_PROCESSES || varena_off >= JE_VM_ARENA_SIZE || (varena_off & 4095))
        { vb_panic(ctx, "clear_pte range"); return; }
    vb_proc *p = &s->procs[process];
    if (!p->used) { vb_panic(ctx, "clear_pte proc"); return; }
    vb_extent *te = ext_of(s, process, p->table_extent);
    if (!te) { vb_panic(ctx, "clear_pte tables"); return; }
    uint64_t *vpt = phys_ptr(frame_phys(te, 3));
    vpt[(varena_off >> 12) & 511] = 0;
}

static void vb_restore_alias(void *ctx, je_vm_ref frame)
{
    vb_state *s = ctx;
    vb_extent *e = ext_of(s, frame.process, frame.extent);
    if (!e || (e->kind != JE_VM_USER_CODE && e->kind != JE_VM_USER_RODATA) ||
        frame.page >= e->pages) { vb_panic(ctx, "restore_alias ref"); return; }
    if (je_user_alias_set(frame_phys(e, frame.page),
                          JE_PAGING_ALIAS_RO_NX, JE_PAGING_ALIAS_RW) != JE_OK)
        vb_panic(ctx, "restore_alias cas");
}

static void vb_free_extent(void *ctx, uint32_t process, uint32_t extent)
{
    vb_state *s = ctx;
    vb_extent *e = ext_of(s, process, extent);
    if (!e) { vb_panic(ctx, "free_extent id"); return; }
    if (je_pages_free(s->pages, e->phys, e->pages) != JE_OK) vb_panic(ctx, "free_extent pages");
    e->used = 0;
}

static void vb_release_hold(void *ctx, uint64_t physical, size_t pages)
{
    vb_state *s = ctx;
    if (je_pages_free(s->pages, physical, pages) != JE_OK) vb_panic(ctx, "release_hold");
}

static void vb_drop_tables(void *ctx, uint32_t process)
{
    vb_state *s = ctx;
    if (process >= JE_VM_MAX_PROCESSES) { vb_panic(ctx, "drop_tables range"); return; }
    vb_proc *p = &s->procs[process];
    if (!p->used) return;
    /* Never freed here: the table FRAMES are freed via free_extent on the TABLE
     * extent (by id), never by walking the tables — a walk would reach the
     * shared kernel PD through PDPT[0]. Refuse to drop the active root. */
    if (p->root_phys && je_arch_cr3_read() == p->root_phys) { vb_panic(ctx, "drop active root"); return; }
    p->used = 0; p->root_phys = 0; p->table_extent = UINT32_MAX;
}

static void vb_flush(void *ctx) { (void)ctx; /* single BSP, CR3 reload flushes */ }

/* ---- read-only audit ops -------------------------------------------------- */

static je_status vb_read_user_pte(void *ctx, uint32_t process, uint64_t varena_off,
                                  uint64_t *out_phys, je_vm_perm *out_perm, int *out_present)
{
    vb_state *s = ctx;
    if (process >= JE_VM_MAX_PROCESSES || !out_phys || !out_perm || !out_present ||
        varena_off >= JE_VM_ARENA_SIZE) return JE_ERR_INVALID;
    vb_proc *p = &s->procs[process];
    if (!p->used) return JE_ERR_INVALID;
    vb_extent *te = ext_of(s, process, p->table_extent);
    if (!te) return JE_ERR_INVALID;
    uint64_t leaf = ((uint64_t *)phys_ptr(frame_phys(te, 3)))[(varena_off >> 12) & 511];
    if (!(leaf & PTE_P)) { *out_present = 0; *out_phys = 0; *out_perm = 0; return JE_OK; }
    if (!(leaf & PTE_U)) return JE_ERR_INVALID; /* a present user leaf MUST be user-accessible */
    *out_present = 1;
    *out_phys = leaf & ~UINT64_C(0xfff) & ~(s->nx ? s->nx : 0);
    int w = (leaf & PTE_W) != 0, x = s->nx ? !(leaf & s->nx) : 1;
    if (x && !w)       *out_perm = JE_VM_PERM_RX;
    else if (!x && !w) *out_perm = JE_VM_PERM_R_NX;
    else if (!x && w)  *out_perm = JE_VM_PERM_RW_NX;
    else return JE_ERR_INVALID; /* W|X is illegal */
    return JE_OK;
}

static je_status vb_read_upper(void *ctx, uint32_t process, int level, uint32_t index,
                               uint32_t *out_flags)
{
    vb_state *s = ctx;
    if (process >= JE_VM_MAX_PROCESSES || !out_flags || index >= 512) return JE_ERR_INVALID;
    vb_proc *p = &s->procs[process];
    if (!p->used) return JE_ERR_INVALID;
    vb_extent *te = ext_of(s, process, p->table_extent);
    if (!te) return JE_ERR_INVALID;
    uint64_t frame; /* level 4=PML4(frame0),3=PDPT(1),2=PD(2),1=PT(3) */
    switch (level) { case 4: frame=0; break; case 3: frame=1; break;
                     case 2: frame=2; break; case 1: frame=3; break;
                     default: return JE_ERR_INVALID; }
    uint64_t e = ((uint64_t *)phys_ptr(frame_phys(te, (uint32_t)frame)))[index];
    uint32_t f = 0;
    if (e & PTE_P) f |= 1u;
    if (e & PTE_W) f |= 2u;
    if (e & PTE_U) f |= 4u;
    if (s->nx && (e & s->nx)) f |= 8u;
    *out_flags = f;
    return JE_OK;
}

static je_status vb_read_alias(void *ctx, je_vm_ref frame, je_vm_alias *out)
{
    vb_state *s = ctx;
    vb_extent *e = ext_of(s, frame.process, frame.extent);
    if (!e || !out || frame.page >= e->pages || e->kind == JE_VM_TABLE) return JE_ERR_INVALID;
    je_paging_alias a;
    if (je_user_alias_get(frame_phys(e, frame.page), &a) != JE_OK) return JE_ERR_INVALID;
    *out = (a == JE_PAGING_ALIAS_RO_NX) ? JE_VM_ALIAS_RO_NX : JE_VM_ALIAS_RW;
    return JE_OK;
}

static je_status vb_extent_frame_phys(void *ctx, uint32_t process, uint32_t extent,
                                      uint32_t page, uint64_t *out_phys)
{
    vb_state *s = ctx;
    vb_extent *e = ext_of(s, process, extent);
    if (!e || !out_phys || page >= e->pages) return JE_ERR_INVALID;
    *out_phys = frame_phys(e, page);
    return JE_OK;
}

static void vb_panic(void *ctx, const char *why)
{
    (void)ctx;
    je_arch_console_write("JeiOS fatal: vm_backend invariant: ");
    je_arch_console_write(why ? why : "(null)");
    je_arch_console_write("\r\n");
    je_arch_halt();
}

static const je_vm_ops g_ops = {
    .context = &g_vb,
    .reserve_hold = vb_reserve_hold, .register_extent = vb_register_extent,
    .map = vb_map, .link_tables = vb_link_tables, .install_pte = vb_install_pte,
    .demote_alias = vb_demote_alias,
    .unmap = vb_unmap, .clear_pte = vb_clear_pte, .restore_alias = vb_restore_alias,
    .free_extent = vb_free_extent, .release_hold = vb_release_hold,
    .drop_tables = vb_drop_tables, .flush = vb_flush,
    .read_user_pte = vb_read_user_pte, .read_upper = vb_read_upper,
    .read_alias = vb_read_alias, .extent_frame_phys = vb_extent_frame_phys,
    .panic = vb_panic,
};

const je_vm_ops *je_arch_vm_ops(je_pages *pages, int nx_available)
{
    g_vb = (vb_state){0};
    g_vb.pages = pages;
    g_vb.nx = nx_available ? (UINT64_C(1) << 63) : 0;
    for (uint32_t i = 0; i < JE_VM_MAX_PROCESSES; ++i) g_vb.procs[i].table_extent = UINT32_MAX;
    return &g_ops;
}
