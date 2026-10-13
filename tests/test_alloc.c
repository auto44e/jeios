/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jeios/alloc.h>
static je_pages pages;
static unsigned char *arena;
static je_boot_context context(void)
{
    je_boot_context b={.valid=true,.map_count=1,.reserved_count=1};
    b.map[0]=(je_memory_range){0x100000,128*4096,1};
    b.reserved[0]=(je_reservation){0x100000,0x103000};return b;
}
static void *mapping(uint64_t a,size_t n,void *fail)
{
    if(fail || a<0x100000 || a>=0x180000 || n>0x180000-a) return NULL;
    return arena+(a-0x100000);
}
int main(void)
{
    je_boot_context b=context();uint64_t a;
    assert(je_pages_init(&pages,&b)==JE_OK && pages.free_count==125);
    assert(je_pages_alloc(&pages,3,&a)==JE_OK && a==0x103000);
    assert(je_pages_free(&pages,a+4096,2)==JE_ERR_INVALID);
    assert(je_pages_free(&pages,a,2)==JE_ERR_INVALID && pages.free_count==122);
    assert(je_pages_free(&pages,a,3)==JE_OK);
    assert(je_pages_free(&pages,a,3)==JE_ERR_INVALID);
    assert(je_pages_free(&pages,0x100000,1)==JE_ERR_INVALID);
    assert(je_pages_alloc(&pages,0,&a)==JE_ERR_INVALID && a==0);
    assert(je_pages_alloc(&pages,257,&a)==JE_ERR_INVALID);
    assert(je_pages_alloc(&pages,126,&a)==JE_ERR_NOMEM && a==0);
    uint64_t all[125];
    for(size_t i=0;i<125;++i) assert(je_pages_alloc(&pages,1,&all[i])==JE_OK);
    assert(pages.free_count==0 && je_pages_alloc(&pages,1,&a)==JE_ERR_NOMEM && a==0);
    /* Fragmentation: free enough pages in total but no contiguous pair. */
    for(size_t i=0;i<125;i+=2) assert(je_pages_free(&pages,all[i],1)==JE_OK);
    assert(je_pages_alloc(&pages,2,&a)==JE_ERR_NOMEM);
    for(size_t i=1;i<125;i+=2) assert(je_pages_free(&pages,all[i],1)==JE_OK);
    assert(pages.free_count==125);
    arena=aligned_alloc(4096,128*4096);assert(arena);memset(arena,0xa5,128*4096);
    je_heap h;void *p=NULL;
    assert(je_heap_init(&h,&pages,mapping,NULL)==JE_OK);
    assert(je_heap_alloc(&h,5000,&p)==JE_OK && p && pages.free_count==123);
    for(size_t i=0;i<8192;++i) assert(((unsigned char *)p)[i]==0);
    memset(p,0xa5,8192);
    assert(je_heap_free(&h,(unsigned char *)p+1)==JE_ERR_INVALID);
    assert(je_heap_free(&h,p)==JE_OK && pages.free_count==125);
    assert(je_heap_free(&h,p)==JE_ERR_INVALID);
    assert(je_heap_alloc(&h,5000,&p)==JE_OK);
    for(size_t i=0;i<8192;++i) assert(((unsigned char *)p)[i]==0);
    assert(je_heap_free(&h,p)==JE_OK);
    assert(je_heap_alloc(&h,SIZE_MAX,&p)==JE_ERR_INVALID && !p);
    assert(je_heap_alloc(&h,0,&p)==JE_ERR_INVALID);
    h.context=&h;assert(je_heap_alloc(&h,64,&p)==JE_ERR_NOMEM && !p && pages.free_count==125);
    h.context=NULL;
    void *objects[JE_HEAP_SLOTS];
    for(size_t i=0;i<JE_HEAP_SLOTS;++i) assert(je_heap_alloc(&h,1,&objects[i])==JE_OK);
    size_t left=pages.free_count;
    assert(je_heap_alloc(&h,1,&p)==JE_ERR_NOMEM && !p && pages.free_count==left);
    for(size_t i=0;i<JE_HEAP_SLOTS;++i) assert(je_heap_free(&h,objects[i])==JE_OK);
    assert(pages.free_count==125);
    free(arena);
    puts("allocator: PASS (reservations, exact frees, exhaustion, fragmentation, zeroing, rollback)");
}

