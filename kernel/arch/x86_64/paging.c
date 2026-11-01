/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/arch.h>
#include <jeios/paging.h>
#include <jeios/trap.h>
static uint64_t pml4[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd[512] __attribute__((aligned(4096)));
static uint64_t pt[512][512] __attribute__((aligned(4096)));
static int active;
static uint64_t nx;
static void map_pages(uint64_t begin, uint64_t end)
{
    if (begin < 0x100000) begin = 0x100000;
    if (end > JE_PHYSICAL_LIMIT) end = JE_PHYSICAL_LIMIT;
    if (begin >= end) return;
    begin = (begin+4095) & ~UINT64_C(4095);
    end &= ~UINT64_C(4095);
    for (uint64_t a=begin; a<end; a+=4096) pt[a>>21][(a>>12)&511]=a|3|nx;
}
je_status je_arch_paging_init(const je_boot_context *ctx)
{
    if (!ctx || !ctx->valid || active) return JE_ERR_INVALID;
    nx = je_arch_has_nx() ? UINT64_C(1)<<63 : 0;
    pml4[0]=(uintptr_t)pdpt|3; pdpt[0]=(uintptr_t)pd|3;
    for (size_t i=0;i<512;++i) pd[i]=(uintptr_t)pt[i]|3;
    for (size_t i=0;i<ctx->map_count;++i) {
        je_memory_range m=ctx->map[i];
        if (m.type==1) map_pages(m.base,m.base+m.length);
    }
    /* Image must already be RAM-validated. No identity aliases outside these
     * mappings; text/rodata are read-only even for supervisor (CR0.WP). */
    for (uintptr_t a=(uintptr_t)__kernel_start; a<(uintptr_t)__text_end; a+=4096)
        pt[a>>21][(a>>12)&511]=(uint64_t)a|1;
    for (uintptr_t a=(uintptr_t)__text_end; a<(uintptr_t)__rodata_end; a+=4096)
        pt[a>>21][(a>>12)&511]=(uint64_t)a|1|nx;
    uintptr_t guards[]={(uintptr_t)je_boot_guard,(uintptr_t)je_df_stack,(uintptr_t)je_nmi_stack};
    for (size_t i=0;i<sizeof(guards)/sizeof(guards[0]);++i)
        pt[guards[i]>>21][(guards[i]>>12)&511]=0;
    if (nx) {
        uint32_t lo,hi;
        __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080u));
        lo |= 1u<<11;
        __asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(0xc0000080u):"memory");
    }
    uintptr_t cr0;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0)); cr0 |= (uintptr_t)1<<16;
    __asm__ volatile("mov %0,%%cr0; mov %1,%%cr3"::"r"(cr0),"r"((uintptr_t)pml4):"memory");
    active=1;
    return JE_OK;
}
je_status je_arch_page_guard(uintptr_t a)
{
    if (!active || (a&4095) || a>=JE_PHYSICAL_LIMIT || a<0x100000 ||
        (a>=(uintptr_t)__kernel_start && a<(uintptr_t)__kernel_end)) return JE_ERR_INVALID;
    /* Only a leaf in the exact allocator-default RW state may be guarded. This
     * refuses a demoted user alias (a|1|nx, RO_NX): guarding then restoring it
     * would silently rewrite it RW and destroy the "no writable alias survives"
     * invariant (ADR 0007 D3). M3a #4 step 2. */
    /* Only a present, writable, non-user leaf (the allocator-default RW alias)
     * may be guarded. Refusing a non-writable leaf refuses a demoted RO_NX user
     * alias (a|1|nx): guarding then restoring it would silently rewrite it RW and
     * destroy "no writable alias survives" (ADR 0007 D3). M3a #4 step 2. Mask off
     * CPU-maintained Accessed(5)/Dirty(6) bits before inspecting. */
    uint64_t leaf = pt[a>>21][(a>>12)&511] & ~UINT64_C(0x60);
    if (leaf != ((uint64_t)a|3|nx)) return JE_ERR_INVALID;
    pt[a>>21][(a>>12)&511]=0;
    __asm__ volatile("invlpg (%0)"::"r"(a):"memory");
    return JE_OK;
}

je_status je_arch_page_restore(uintptr_t a)
{
    if (!active || (a&4095) || a>=JE_PHYSICAL_LIMIT || a<0x100000 ||
        (a>=(uintptr_t)__kernel_start && a<(uintptr_t)__kernel_end)) return JE_ERR_INVALID;
    if (pt[a>>21][(a>>12)&511]) return JE_ERR_INVALID;
    pt[a>>21][(a>>12)&511]=(uint64_t)a|3|nx;
    __asm__ volatile("invlpg (%0)"::"r"(a):"memory");
    return JE_OK;
}

/* ---- M3a #4 step 2 seam: kernel-subtree share, user alias CAS, CR3 helpers -- */

uint64_t je_kernel_pd_phys(void) { return (uintptr_t)pd; }

uint64_t je_arch_kernel_root_phys(void) { return (uintptr_t)pml4; }

bool je_arch_page_user_accessible(uint64_t root_phys, uintptr_t address)
{
    if (root_phys == 0u || address >= JE_PHYSICAL_LIMIT) return false;
    uint64_t *l4 = (uint64_t *)(uintptr_t)root_phys;
    uint64_t e4 = l4[(address >> 39) & 511u];
    if ((e4 & 5u) != 5u) return false;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(e4 & ~UINT64_C(0xfff));
    uint64_t e3 = l3[(address >> 30) & 511u];
    if ((e3 & 5u) != 5u) return false;
    uint64_t *l2 = (uint64_t *)(uintptr_t)(e3 & ~UINT64_C(0xfff));
    uint64_t e2 = l2[(address >> 21) & 511u];
    if ((e2 & 5u) != 5u) return false;
    uint64_t *l1 = (uint64_t *)(uintptr_t)(e2 & ~UINT64_C(0xfff));
    uint64_t e1 = l1[(address >> 12) & 511u];
    return (e1 & 5u) == 5u;
}

static int alias_addr_ok(uint64_t a)
{
    /* A user frame's identity alias: page-aligned, inside the allocator window
     * [0x100000, JE_PHYSICAL_LIMIT), never the kernel image, never a guard. */
    if (!active || (a&4095) || a>=JE_PHYSICAL_LIMIT || a<0x100000) return 0;
    if (a>=(uintptr_t)__kernel_start && a<(uintptr_t)__kernel_end) return 0;
    uintptr_t guards[]={(uintptr_t)je_boot_guard,(uintptr_t)je_df_stack,(uintptr_t)je_nmi_stack};
    for (size_t i=0;i<sizeof(guards)/sizeof(guards[0]);++i)
        if (a==guards[i]) return 0;
    return 1;
}

je_status je_user_alias_set(uint64_t phys, je_paging_alias expect, je_paging_alias next)
{
    if (!alias_addr_ok(phys)) return JE_ERR_INVALID;
    if ((expect != JE_PAGING_ALIAS_RW && expect != JE_PAGING_ALIAS_RO_NX) ||
        (next   != JE_PAGING_ALIAS_RW && next   != JE_PAGING_ALIAS_RO_NX))
        return JE_ERR_INVALID; /* two legal states only */
    uint64_t rw = phys|3|nx, ro = phys|1|nx; /* RW == P|W|NX ; RO_NX == P|NX */
    uint64_t want_cur = (expect==JE_PAGING_ALIAS_RW) ? rw : ro;
    uint64_t set_to   = (next  ==JE_PAGING_ALIAS_RW) ? rw : ro;
    uint64_t *leaf = &pt[phys>>21][(phys>>12)&511];
    /* Mask CPU-maintained Accessed(5)/Dirty(6) when matching current state. */
    if ((*leaf & ~UINT64_C(0x60)) != want_cur) return JE_ERR_INVALID; /* CAS guard */
    *leaf = set_to; /* writing a fresh leaf clears A/D, as intended */
    __asm__ volatile("invlpg (%0)"::"r"(phys):"memory");
    return JE_OK;
}

je_status je_user_alias_get(uint64_t phys, je_paging_alias *out)
{
    if (!out || !alias_addr_ok(phys)) return JE_ERR_INVALID;
    uint64_t leaf = pt[phys>>21][(phys>>12)&511] & ~UINT64_C(0x60); /* ignore A/D */
    if (leaf == (phys|3|nx)) { *out = JE_PAGING_ALIAS_RW; return JE_OK; }
    if (leaf == (phys|1|nx)) { *out = JE_PAGING_ALIAS_RO_NX; return JE_OK; }
    return JE_ERR_INVALID; /* not a recognizable user-alias leaf */
}

uint64_t je_arch_cr3_read(void)
{
    uint64_t v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v;
}

void je_arch_cr3_load(uint64_t root_phys)
{
    __asm__ volatile("mov %0,%%cr3"::"r"((uintptr_t)root_phys):"memory");
}

