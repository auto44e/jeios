/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/alloc.h>
#define RESERVED UINT16_MAX
#define TAIL (UINT16_MAX-1u)
je_status je_pages_init(je_pages *p,const je_boot_context *b)
{
    if (!p || !b || !b->valid || b->map_count>JE_BOOT_MAP_MAX || b->reserved_count>JE_BOOT_RESERVED_MAX)
        return JE_ERR_INVALID;
    p->ready=false; p->free_count=0;
    for (size_t i=0;i<JE_PAGE_COUNT;++i) {
        bool free=je_boot_free_page(b,i*JE_PAGE_SIZE);
        p->state[i]=free ? 0 : RESERVED;
        if (free) ++p->free_count;
    }
    p->ready=true;
    return JE_OK;
}
je_status je_pages_alloc(je_pages *p,size_t n,uint64_t *out)
{
    if (!out) return JE_ERR_INVALID;
    *out=0;
    if (!p || !p->ready || !n || n>JE_ALLOC_MAX_PAGES) return JE_ERR_INVALID;
    if (n>p->free_count) return JE_ERR_NOMEM;
    size_t run=0;
    for (size_t i=0;i<JE_PAGE_COUNT;++i) {
        run=p->state[i]==0 ? run+1 : 0;
        if (run==n) {
            size_t start=i+1-n;
            p->state[start]=(uint16_t)n;
            for (size_t j=start+1;j<=i;++j) p->state[j]=TAIL;
            p->free_count-=n;
            *out=start*JE_PAGE_SIZE;
            return JE_OK;
        }
    }
    return JE_ERR_NOMEM;
}
je_status je_pages_free(je_pages *p,uint64_t a,size_t n)
{
    if (!p || !p->ready || !n || n>JE_ALLOC_MAX_PAGES || (a&4095) || a>=JE_PHYSICAL_LIMIT)
        return JE_ERR_INVALID;
    size_t start=(size_t)(a/JE_PAGE_SIZE);
    if (n>JE_PAGE_COUNT-start || p->state[start]!=n) return JE_ERR_INVALID;
    for (size_t i=start+1;i<start+n;++i) if (p->state[i]!=TAIL) return JE_ERR_INVALID;
    for (size_t i=start;i<start+n;++i) p->state[i]=0;
    p->free_count+=n;
    return JE_OK;
}
je_status je_heap_init(je_heap *h,je_pages *p,je_map_ram map,void *context)
{
    if (!h || !p || !p->ready || !map) return JE_ERR_INVALID;
    *h=(je_heap){.pages=p,.map=map,.context=context};
    return JE_OK;
}
je_status je_heap_alloc(je_heap *h,size_t bytes,void **out)
{
    if (!out) return JE_ERR_INVALID;
    *out=NULL;
    if (!h || !h->pages || !h->map || !bytes || bytes>JE_ALLOC_MAX_PAGES*JE_PAGE_SIZE)
        return JE_ERR_INVALID;
    size_t slot=0;
    while (slot<JE_HEAP_SLOTS && h->records[slot].pointer) ++slot;
    if (slot==JE_HEAP_SLOTS) return JE_ERR_NOMEM;
    size_t count=(bytes+4095)/4096;
    uint64_t physical;
    je_status s=je_pages_alloc(h->pages,count,&physical);
    if (s!=JE_OK) return s;
    unsigned char *p=h->map(physical,count*4096,h->context);
    if (!p || ((uintptr_t)p&15)) {
        je_status cleanup=je_pages_free(h->pages,physical,count);
        return cleanup==JE_OK ? JE_ERR_NOMEM : cleanup;
    }
    for (size_t i=0;i<count*4096;++i) p[i]=0;
    h->records[slot]=(je_heap_record){p,physical,count};
    *out=p;
    return JE_OK;
}
je_status je_heap_free(je_heap *h,void *p)
{
    if (!h || !h->pages || !p) return JE_ERR_INVALID;
    for (size_t i=0;i<JE_HEAP_SLOTS;++i) if (h->records[i].pointer==p) {
        je_heap_record r=h->records[i];
        je_status s=je_pages_free(h->pages,r.physical,r.pages);
        if (s!=JE_OK) return s;
        h->records[i]=(je_heap_record){0};
        return JE_OK;
    }
    return JE_ERR_INVALID;
}

