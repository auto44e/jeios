/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/boot.h>
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t u64(const unsigned char *p)
{ return (uint64_t)u32(p) | ((uint64_t)u32(p+4)<<32); }
static bool overlap(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{ return a < d && c < b; }
static je_status reserve(je_boot_context *c, uint64_t start, uint64_t end)
{
    if (start >= end) return JE_ERR_INVALID;
    if (c->reserved_count == JE_BOOT_RESERVED_MAX) return JE_ERR_NOMEM;
    c->reserved[c->reserved_count++] = (je_reservation){start,end};
    return JE_OK;
}
je_status je_boot_parse(const void *data, size_t accessible, uint64_t physical,
                        uint64_t kernel_begin, uint64_t kernel_end, je_boot_context *out)
{
    if (!out) return JE_ERR_INVALID;
    *out = (je_boot_context){0};
    if (!data || accessible < 16 || (physical & 7) || physical >= JE_PHYSICAL_LIMIT ||
        kernel_begin < 0x100000 || kernel_begin >= kernel_end || kernel_end > JE_PHYSICAL_LIMIT)
        return JE_ERR_INVALID;
    const unsigned char *p = data;
    uint32_t total = u32(p);
    if (u32(p+4) || total < 16 || (total & 7) || total > accessible ||
        total > JE_BOOT_BYTES_MAX || total > JE_PHYSICAL_LIMIT - physical)
        return JE_ERR_INVALID;
    je_boot_context draft = {0};
    draft.boot_bytes = total;
    (void)reserve(&draft,0,0x100000);
    (void)reserve(&draft,kernel_begin,kernel_end);
    (void)reserve(&draft,physical,physical+total);
    bool found_map = false, found_end = false;
    for (size_t offset = 8; offset < total;) {
        if (total-offset < 8) return JE_ERR_INVALID;
        uint32_t type = u32(p+offset), size = u32(p+offset+4);
        if (size < 8 || size > total-offset) return JE_ERR_INVALID;
        size_t step = ((size_t)size+7u) & ~(size_t)7u;
        if (step > total-offset) return JE_ERR_INVALID;
        if (type == 0) {
            if (size != 8 || offset+8 != total) return JE_ERR_INVALID;
            found_end = true;
            break;
        }
        if (type == 6) {
            if (found_map || size < 16) return JE_ERR_INVALID;
            found_map = true;
            uint32_t stride = u32(p+offset+8);
            if (stride < 24 || (stride & 7) || (size-16)%stride) return JE_ERR_INVALID;
            if (u32(p+offset+12) != 0) return JE_ERR_UNSUPPORTED;
            size_t count = (size-16)/stride;
            if (count > JE_BOOT_MAP_MAX) return JE_ERR_NOMEM;
            for (size_t j=0; j<count; ++j) {
                const unsigned char *e = p+offset+16+j*stride;
                uint64_t base=u64(e), length=u64(e+8);
                if (length > UINT64_MAX-base || u32(e+20)) return JE_ERR_INVALID;
                if (!length) continue;
                for (size_t k=0; k<draft.map_count; ++k) {
                    je_memory_range old = draft.map[k];
                    if (overlap(base,base+length,old.base,old.base+old.length)) return JE_ERR_INVALID;
                }
                draft.map[draft.map_count++] = (je_memory_range){base,length,u32(e+16)};
            }
        } else if (type == 3) {
            if (size < 17) return JE_ERR_INVALID;
            bool terminated = false;
            for (size_t j=16;j<size;++j) if (p[offset+j]==0) { terminated=true;break; }
            if (!terminated) return JE_ERR_INVALID;
            je_status s = reserve(&draft,u32(p+offset+8),u32(p+offset+12));
            if (s != JE_OK) return s;
        }
        offset += step;
    }
    if (!found_map || !found_end || !draft.map_count) return JE_ERR_INVALID;
    draft.valid = true;
    /* The complete image must be ordinary RAM, not merely identity mapped. */
    for (uint64_t a = kernel_begin & ~(JE_PAGE_SIZE-1); a < kernel_end; a += JE_PAGE_SIZE)
        if (!je_boot_ram_page(&draft,a)) return JE_ERR_INVALID;
    *out = draft;
    return JE_OK;
}
bool je_boot_ram_page(const je_boot_context *c, uint64_t a)
{
    if (!c || !c->valid || (a & (JE_PAGE_SIZE-1)) || a > JE_PHYSICAL_LIMIT-JE_PAGE_SIZE)
        return false;
    for (size_t i=0;i<c->map_count;++i) {
        je_memory_range m=c->map[i];
        if (m.type==1 && a>=m.base && a-m.base<=m.length && JE_PAGE_SIZE<=m.length-(a-m.base))
            return true;
    }
    return false;
}
bool je_boot_free_page(const je_boot_context *c, uint64_t a)
{
    if (!je_boot_ram_page(c,a)) return false;
    for (size_t i=0;i<c->reserved_count;++i)
        if (overlap(a,a+JE_PAGE_SIZE,c->reserved[i].begin,c->reserved[i].end)) return false;
    return true;
}

