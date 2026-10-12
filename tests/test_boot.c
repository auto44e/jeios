#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jeios/boot.h>
static void w32(unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;++i) p[i]=(unsigned char)(v>>(8*i)); }
static void w64(unsigned char *p,uint64_t v)
{ w32(p,(uint32_t)v);w32(p+4,(uint32_t)(v>>32)); }
static size_t fixture(unsigned char *b)
{
    memset(b,0,256);
    w32(b,128); w32(b+8,6);w32(b+12,88);w32(b+16,24);
    w64(b+24,0);w64(b+32,0x9f000);w32(b+40,1);
    w64(b+48,0x9f000);w64(b+56,0x61000);w32(b+64,2);
    w64(b+72,0x100000);w64(b+80,0xf00000);w32(b+88,1);
    w32(b+96,3);w32(b+100,24);w32(b+104,0x200000);w32(b+108,0x202000);
    w32(b+120,0);w32(b+124,8);
    return 128;
}
static je_status parse(const unsigned char *b,size_t n,je_boot_context *c)
{ return je_boot_parse(b,n,0x300000,0x100000,0x110000,c); }
int main(void)
{
    unsigned char b[256];je_boot_context c;
    size_t n=fixture(b);
    assert(parse(b,n,&c)==JE_OK && c.valid && c.map_count==3 && c.reserved_count==4);
    assert(je_boot_ram_page(&c,0x100000));
    assert(!je_boot_free_page(&c,0x100000));
    assert(!je_boot_free_page(&c,0));
    assert(!je_boot_free_page(&c,0x9f000));
    assert(!je_boot_free_page(&c,0x200000));
    assert(!je_boot_free_page(&c,0x201000));
    assert(!je_boot_free_page(&c,0x300000));
    assert(je_boot_free_page(&c,0x110000));
    assert(!je_boot_free_page(&c,0x110001));
    assert(!je_boot_free_page(&c,UINT64_MAX));
    for(size_t len=0;len<n;++len) {
        unsigned char *tiny=malloc(len ? len : 1);assert(tiny);
        memcpy(tiny,b,len);
        assert(parse(tiny,len,&c)!=JE_OK && !c.valid);
        free(tiny);
    }
    fixture(b);w32(b+12,0);assert(parse(b,n,&c)==JE_ERR_INVALID && !c.valid);
    fixture(b);w32(b+16,0);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w32(b+16,25);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w32(b+20,1);assert(parse(b,n,&c)==JE_ERR_UNSUPPORTED);
    fixture(b);w64(b+72,UINT64_MAX-2);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w64(b+48,0x1000);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w32(b+124,16);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w32(b+120,55);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w32(b+104,0x203000);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);memset(b+112,'x',8);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w32(b,JE_BOOT_BYTES_MAX+8);assert(parse(b,n,&c)==JE_ERR_INVALID);
    fixture(b);w32(b+88,99);assert(parse(b,n,&c)==JE_ERR_INVALID); /* image no longer RAM */
    fixture(b);assert(je_boot_parse(b,n,JE_PHYSICAL_LIMIT-8,0x100000,0x110000,&c)==JE_ERR_INVALID);
    assert(je_boot_parse(b,n,0x300001,0x100000,0x110000,&c)==JE_ERR_INVALID);
    assert(je_boot_parse(b,n,0x300000,0,0x110000,&c)==JE_ERR_INVALID);
    assert(parse(NULL,0,&c)==JE_ERR_INVALID);
    assert(parse(b,n,NULL)==JE_ERR_INVALID);
    /* Deterministic mutation corpus under ASan/UBSan: parser must stay bounded.
     * On accepted inputs, retained state is internally bounded and reservation
     * predicates must never expose the kernel image or any low-memory page. */
    uint32_t rng=0x12345678;
    for(size_t i=0;i<20000;++i) {
        fixture(b);rng=rng*1664525u+1013904223u;
        b[rng%n]^=(unsigned char)((rng>>16)|1);
        if(parse(b,n,&c)==JE_OK) {
            assert(c.valid && c.map_count<=JE_BOOT_MAP_MAX && c.reserved_count<=JE_BOOT_RESERVED_MAX);
            assert(!je_boot_free_page(&c,0x100000) && !je_boot_free_page(&c,0));
        } else assert(!c.valid);
    }
    puts("boot parser: PASS (truncation, overflow, overlaps, modules, mutation corpus)");
}
