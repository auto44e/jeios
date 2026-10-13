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
    if (!(pt[a>>21][(a>>12)&511]&1)) return JE_ERR_INVALID;
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

