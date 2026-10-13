/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/arch.h>
#include <jeios/kheap.h>
je_pages je_kernel_pages;
static je_heap heap;
static void *map(uint64_t physical,size_t bytes,void *context)
{
    (void)context;
    if (physical>=JE_PHYSICAL_LIMIT || bytes>JE_PHYSICAL_LIMIT-physical) return NULL;
    return (void *)(uintptr_t)physical;
}
je_status je_kheap_init(const je_boot_context *boot)
{
    je_status s=je_pages_init(&je_kernel_pages,boot);
    if (s!=JE_OK) return s;
    return je_heap_init(&heap,&je_kernel_pages,map,NULL);
}
je_status je_kalloc(size_t bytes,void **out)
{
    uint64_t flags=je_arch_irq_save();
    je_status s=je_heap_alloc(&heap,bytes,out);
    je_arch_irq_restore(flags);return s;
}
je_status je_kfree(void *p)
{
    uint64_t flags=je_arch_irq_save();
    je_status s=je_heap_free(&heap,p);
    je_arch_irq_restore(flags);return s;
}

