/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hostile hosted suite for the bounded FDT boot-context parser (issue #52, MP1).
 *
 * Written from the frozen contract include/jeios/fdt.h ALONE, by an author other
 * than the parser's, so that either implementation is held to the header's
 * normative rules rather than to its own behavior. Where the header is silent the
 * suite asserts only what the header implies (see the "literal reading" notes).
 *
 * Mechanism:
 *  - An in-test FDT builder emits big-endian header / rsvmap / struct tokens /
 *    strings (with offset reuse, including dtc-style suffix sharing), so every
 *    negative case is a valid blob carrying exactly ONE defect.
 *  - Every blob handed to je_fdt_parse() through parse_bytes() with len > 0 is
 *    copied into an EXACT-size heap buffer so ASan reports any read at/after
 *    `len`; nothing is parsed out of a large static buffer. (Exceptions: a
 *    len == 0 call gets a 1-byte buffer because malloc(0) may return NULL, and
 *    test_bad_arg_parse() passes NULL / the fixture-size buffer directly.)
 *  - *out is pre-poisoned (0xA5) before each call so "zeroed on failure" is a real
 *    check, and *err_off is pre-poisoned so "0 on success" is too.
 *  - Robustness: an exhaustive truncation sweep of the QEMU fixture and a fixed-
 *    seed LCG mutation loop with determinism and post-condition invariants.
 */
#include <jeios/fdt.h>
#include "fixtures/qemu_virt82_dtb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* failure reporting (assert-like, but prints context and survives NDEBUG)    */

static const char *const k_names[JE_FDT_STATUS_COUNT] = {
    "OK", "BAD_ARG", "TRUNCATED", "BAD_MAGIC", "TOO_LARGE", "BAD_VERSION",
    "BAD_LAYOUT", "BAD_TOKEN", "BAD_STRING", "DEPTH_LIMIT", "NODE_LIMIT",
    "PROP_LIMIT", "DUPLICATE", "BAD_CELLS", "NO_MEMORY", "RANGE_LIMIT",
    "BAD_RANGE", "OVERLAP", "CONFLICT",
};

/* Local name table: diagnostics must not depend on the code under test. */
static const char *nm(je_fdt_status s)
{
    return ((unsigned)s < (unsigned)JE_FDT_STATUS_COUNT) ? k_names[s] : "<out-of-range>";
}

#define FAILF(...)                                                            \
    do {                                                                      \
        fprintf(stderr, "fdt: FAIL %s:%d: ", __FILE__, __LINE__);             \
        fprintf(stderr, __VA_ARGS__);                                         \
        fputc('\n', stderr);                                                  \
        abort();                                                              \
    } while (0)
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c))                                                             \
            FAILF("%s", #c);                                                  \
    } while (0)

/* ------------------------------------------------------------------------- */
/* big-endian helpers                                                         */

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void be64(uint8_t *p, uint64_t v)
{
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ------------------------------------------------------------------------- */
/* FDT builder                                                                */

#define T_BEGIN    1u
#define T_END_NODE 2u
#define T_PROP     3u
#define T_NOP      4u
#define T_END      9u

/* header field byte offsets */
#define H_MAGIC   0u
#define H_TOTAL   4u
#define H_OFF_ST  8u
#define H_OFF_STR 12u
#define H_OFF_RSV 16u
#define H_VER     20u
#define H_LCV     24u
#define H_SZ_STR  32u
#define H_SZ_ST   36u

#define ST_CAP  (64u * 1024u)
#define STR_CAP 8192u
#define RSV_CAP 40u
#define IMG_CAP (96u * 1024u)

static struct {
    uint8_t  st[ST_CAP];
    uint32_t st_len;
    char     str[STR_CAP];
    uint32_t str_len;
    uint64_t rsv_addr[RSV_CAP];
    uint64_t rsv_size[RSV_CAP];
    uint32_t rsv_n;
    bool     rsv_noterm;  /* omit the rsvmap terminator */
    uint32_t rsv_gap;     /* bytes (multiple of 8) between header and rsvmap */
    uint32_t st_gap;      /* bytes (multiple of 4) between rsvmap end and struct */
    uint32_t str_gap;     /* bytes between struct end and strings */
} B;

/* The finished image (construction scratch only; parse() copies it out). */
static struct {
    uint8_t  buf[IMG_CAP];
    uint32_t len;
    uint32_t off_rsv, off_st, size_st, off_str, size_str;
} I;

static void b_reset(void)
{
    memset(&B, 0, sizeof B);
}

static uint32_t st_word(uint32_t v)
{
    CHECK(B.st_len + 4u <= ST_CAP);
    uint32_t at = B.st_len;
    be32(&B.st[at], v);
    B.st_len += 4u;
    return at;
}

static void st_bytes(const void *p, uint32_t n)
{
    CHECK(B.st_len + n + 3u <= ST_CAP);
    if (n != 0u)
        memcpy(&B.st[B.st_len], p, n);
    B.st_len += n;
    while ((B.st_len & 3u) != 0u)
        B.st[B.st_len++] = 0u;
}

/* Returns the struct-relative offset of the BEGIN_NODE token. */
static uint32_t b_begin(const char *name)
{
    uint32_t at = st_word(T_BEGIN);
    st_bytes(name, (uint32_t)strlen(name) + 1u);
    return at;
}

static uint32_t b_end(void) { return st_word(T_END_NODE); }
static uint32_t b_nop(void) { return st_word(T_NOP); }
static uint32_t b_endtok(void) { return st_word(T_END); }

/* Always-append a new string (no reuse). */
static uint32_t b_str_new(const char *s)
{
    uint32_t n = (uint32_t)strlen(s);
    CHECK(B.str_len + n + 1u <= STR_CAP);
    uint32_t at = B.str_len;
    memcpy(&B.str[at], s, n + 1u);
    B.str_len += n + 1u;
    return at;
}

/* Reuse any existing NUL-terminated occurrence, including a suffix of a longer
 * string (dtc suffix sharing), else append. */
static uint32_t b_str(const char *s)
{
    uint32_t n = (uint32_t)strlen(s);
    for (uint32_t p = 0u; p + n < B.str_len; p++) {
        if (memcmp(&B.str[p], s, n) == 0 && B.str[p + n] == '\0')
            return p;
    }
    return b_str_new(s);
}

/* Returns the struct-relative offset of the PROP token. */
static uint32_t b_prop_off(uint32_t nameoff, const void *val, uint32_t len)
{
    uint32_t at = st_word(T_PROP);
    st_word(len);
    st_word(nameoff);
    st_bytes(val, len);
    return at;
}

static uint32_t b_prop(const char *name, const void *val, uint32_t len)
{
    return b_prop_off(b_str(name), val, len);
}

static uint32_t b_u32(const char *name, uint32_t v)
{
    uint8_t t[4];
    be32(t, v);
    return b_prop(name, t, 4u);
}

static uint32_t b_s(const char *name, const char *s)
{
    return b_prop(name, s, (uint32_t)strlen(s) + 1u);
}

static uint32_t b_empty(const char *name)
{
    return b_prop(name, NULL, 0u);
}

/* A 4- or 8-byte big-endian scalar property (initrd bounds). */
static uint32_t b_be(const char *name, uint64_t v, uint32_t nbytes)
{
    uint8_t t[8];
    if (nbytes == 4u) {
        CHECK(v <= 0xffffffffu);
        be32(t, (uint32_t)v);
    } else {
        CHECK(nbytes == 8u);
        be64(t, v);
    }
    return b_prop(name, t, nbytes);
}

static void enc1(uint8_t *out, uint32_t *o, uint64_t v, uint32_t cells)
{
    if (cells == 1u) {
        CHECK(v <= 0xffffffffu);
        be32(out + *o, (uint32_t)v);
        *o += 4u;
    } else {
        CHECK(cells == 2u);
        be64(out + *o, v);
        *o += 8u;
    }
}

/* `reg` with npairs {base,size} pairs encoded with the given cells. */
static uint32_t b_reg(uint32_t ac, uint32_t sc, uint32_t npairs, const uint64_t *pairs)
{
    uint8_t t[18u * 16u];
    uint32_t o = 0u;
    CHECK(npairs <= 18u);
    for (uint32_t i = 0u; i < npairs; i++) {
        enc1(t, &o, pairs[2u * i], ac);
        enc1(t, &o, pairs[2u * i + 1u], sc);
    }
    return b_prop("reg", t, o);
}

static uint32_t b_reg1(uint32_t ac, uint32_t sc, uint64_t base, uint64_t size)
{
    const uint64_t p[2] = { base, size };
    return b_reg(ac, sc, 1u, p);
}

static void b_rsv(uint64_t addr, uint64_t size)
{
    CHECK(B.rsv_n < RSV_CAP);
    B.rsv_addr[B.rsv_n] = addr;
    B.rsv_size[B.rsv_n] = size;
    B.rsv_n++;
}

static void fill(uint32_t at, uint32_t n, uint8_t v)
{
    CHECK(at + n <= IMG_CAP);
    memset(&I.buf[at], v, n);
}

static void b_finish(void)
{
    uint32_t o = JE_FDT_HEADER_SIZE;
    memset(&I, 0, sizeof I);
    fill(o, B.rsv_gap, 0xeeu);
    o += B.rsv_gap;
    I.off_rsv = o;
    for (uint32_t i = 0u; i < B.rsv_n; i++) {
        be64(&I.buf[o], B.rsv_addr[i]);
        be64(&I.buf[o + 8u], B.rsv_size[i]);
        o += 16u;
    }
    if (!B.rsv_noterm)
        o += 16u; /* zero terminator (buffer already zeroed) */
    fill(o, B.st_gap, 0xeeu);
    o += B.st_gap;
    I.off_st = o;
    CHECK(o + B.st_len <= IMG_CAP);
    memcpy(&I.buf[o], B.st, B.st_len);
    I.size_st = B.st_len;
    o += B.st_len;
    fill(o, B.str_gap, 0xeeu);
    o += B.str_gap;
    I.off_str = o;
    CHECK(o + B.str_len <= IMG_CAP);
    memcpy(&I.buf[o], B.str, B.str_len);
    I.size_str = B.str_len;
    o += B.str_len;
    I.len = o;

    be32(&I.buf[H_MAGIC], JE_FDT_MAGIC);
    be32(&I.buf[H_TOTAL], o);
    be32(&I.buf[H_OFF_ST], I.off_st);
    be32(&I.buf[H_OFF_STR], I.off_str);
    be32(&I.buf[H_OFF_RSV], I.off_rsv);
    be32(&I.buf[H_VER], 17u);
    be32(&I.buf[H_LCV], 16u);
    be32(&I.buf[H_SZ_STR], I.size_str);
    be32(&I.buf[H_SZ_ST], I.size_st);
}

static void h_set(uint32_t field, uint32_t v)
{
    be32(&I.buf[field], v);
}

/* Patch a word inside the finished struct block (struct-relative offset). */
static void st_patch(uint32_t rel, uint32_t v)
{
    be32(&I.buf[I.off_st + rel], v);
}

/* ---- common tree fragments ---- */

#define RAM_B 0x40000000u
#define RAM_S 0x10000000u

static void root_open(uint32_t ac, uint32_t sc)
{
    b_begin("");
    b_u32("#address-cells", ac);
    b_u32("#size-cells", sc);
}

static void root_close(void)
{
    b_end();
    b_endtok();
}

static void mem(const char *name, uint32_t ac, uint32_t sc, uint64_t base, uint64_t size)
{
    b_begin(name);
    b_s("device_type", "memory");
    b_reg1(ac, sc, base, size);
    b_end();
}

static void rm_open(uint32_t ac, uint32_t sc)
{
    b_begin("reserved-memory");
    b_u32("#address-cells", ac);
    b_u32("#size-cells", sc);
    b_empty("ranges");
}

/* Returns the struct-relative offset of the child's `reg` PROP token. */
static uint32_t rm_child(const char *name, uint32_t ac, uint32_t sc, uint64_t base,
                         uint64_t size, bool nomap)
{
    uint32_t reg_at;
    b_begin(name);
    reg_at = b_reg1(ac, sc, base, size);
    if (nomap)
        b_empty("no-map");
    b_end();
    return reg_at;
}

static void chosen_initrd(uint64_t s, uint32_t sb, uint64_t e, uint32_t eb)
{
    b_begin("chosen");
    b_be("linux,initrd-start", s, sb);
    b_be("linux,initrd-end", e, eb);
    b_end();
}

/* root 2/2 + one memory node {RAM_B, RAM_S}; finished. */
static void build_simple(void)
{
    b_reset();
    root_open(2u, 2u);
    mem("memory@40000000", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
}

/* ------------------------------------------------------------------------- */
/* parse harness                                                              */

static bool ctx_is_zero(const je_boot_ctx *c)
{
    const unsigned char *hb = (const unsigned char *)&c->has_initrd;
    if (c->totalsize || c->used || c->addr_cells || c->size_cells || c->node_count ||
        c->max_depth || c->ram_count || c->rsv_count || c->rsv_ignored ||
        c->rsv_dynamic || *hb != 0u || c->initrd.base || c->initrd.size)
        return false;
    for (uint32_t i = 0u; i < JE_FDT_MAX_RAM; i++)
        if (c->ram[i].base || c->ram[i].size || c->ram_off[i])
            return false;
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        if (c->rsv[i].base || c->rsv[i].size || c->rsv_off[i] || c->rsv_nomap[i])
            return false;
    return true;
}

static bool range_eq(je_range a, je_range b)
{
    return a.base == b.base && a.size == b.size;
}

/* Field-wise equality (padding bytes are not part of the contract). */
static bool ctx_eq(const je_boot_ctx *a, const je_boot_ctx *b)
{
    const unsigned char ha = *(const unsigned char *)&a->has_initrd;
    const unsigned char hb = *(const unsigned char *)&b->has_initrd;
    if (a->totalsize != b->totalsize || a->used != b->used ||
        a->addr_cells != b->addr_cells || a->size_cells != b->size_cells ||
        a->node_count != b->node_count || a->max_depth != b->max_depth ||
        a->ram_count != b->ram_count || a->rsv_count != b->rsv_count ||
        a->rsv_ignored != b->rsv_ignored || a->rsv_dynamic != b->rsv_dynamic ||
        ha != hb || !range_eq(a->initrd, b->initrd))
        return false;
    for (uint32_t i = 0u; i < JE_FDT_MAX_RAM; i++)
        if (!range_eq(a->ram[i], b->ram[i]) || a->ram_off[i] != b->ram_off[i])
            return false;
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        if (!range_eq(a->rsv[i], b->rsv[i]) || a->rsv_off[i] != b->rsv_off[i] ||
            a->rsv_nomap[i] != b->rsv_nomap[i])
            return false;
    return true;
}

/* Copy `len` bytes into an exact-size heap buffer and parse. Enforces the
 * universal post-conditions: valid enumerator; failure -> *out zeroed;
 * success -> *err_off == 0. */
static je_fdt_status parse_bytes(const uint8_t *src, size_t len, je_boot_ctx *out,
                                 uint32_t *eo)
{
    uint8_t *p = malloc(len);
    if (p == NULL && len == 0u)
        p = malloc(1u); /* malloc(0) may return NULL; never pass NULL here */
    CHECK(p != NULL);
    if (len != 0u)
        memcpy(p, src, len);
    memset(out, 0xa5, sizeof *out);
    *eo = 0xdeadbeefu;
    je_fdt_status s = je_fdt_parse(p, len, out, eo);
    free(p);
    if ((unsigned)s >= (unsigned)JE_FDT_STATUS_COUNT)
        FAILF("status %u is not a valid enumerator", (unsigned)s);
    if (s == JE_FDT_OK && *eo != 0u)
        FAILF("OK but err_off=%u (must be 0)", *eo);
    if (s != JE_FDT_OK && !ctx_is_zero(out))
        FAILF("status %s but *out not fully zeroed", nm(s));
    return s;
}

static je_boot_ctx C;
static uint32_t EO;

static je_fdt_status parse_img(void)
{
    return parse_bytes(I.buf, I.len, &C, &EO);
}

static je_fdt_status parse_img_len(uint32_t len)
{
    CHECK(len <= IMG_CAP);
    return parse_bytes(I.buf, len, &C, &EO);
}

static void want_(je_fdt_status got, je_fdt_status want, int line)
{
    if (got != want) {
        fprintf(stderr, "fdt: FAIL %s:%d: want %s got %s (err_off %u)\n",
                __FILE__, line, nm(want), nm(got), EO);
        abort();
    }
}

/* Parse the current image and require an exact status. */
#define EXPECT(st) want_(parse_img(), (st), __LINE__)
/* ... and an exact err_off. */
#define EXPECT_AT(st, off)                                                    \
    do {                                                                      \
        EXPECT(st);                                                           \
        if (EO != (uint32_t)(off))                                            \
            FAILF("err_off %u, want %u", EO, (uint32_t)(off));                \
    } while (0)
/* ... and err_off inside the struct block. */
#define EXPECT_IN_ST(st)                                                      \
    do {                                                                      \
        EXPECT(st);                                                           \
        if (EO < I.off_st || EO >= I.off_st + I.size_st)                      \
            FAILF("err_off %u outside struct [%u,%u)", EO, I.off_st,          \
                  I.off_st + I.size_st);                                      \
    } while (0)

/* BAD_STRING from a bad nameoff: err_off is EXACTLY that PROP token (contract
 * a4712a8 "err_off pins (round-4)"), not its len or nameoff word. */
#define EXPECT_BAD_NAMEOFF_AT(prop_rel) EXPECT_AT(JE_FDT_BAD_STRING, I.off_st + (prop_rel))

static bool has_ram(const je_boot_ctx *c, uint64_t base, uint64_t size)
{
    for (uint32_t i = 0u; i < c->ram_count && i < JE_FDT_MAX_RAM; i++)
        if (c->ram[i].base == base && c->ram[i].size == size)
            return true;
    return false;
}

static bool off_in_st(uint32_t off)
{
    return off >= I.off_st && off < I.off_st + I.size_st;
}

/* ------------------------------------------------------------------------- */
/* 2. real fixture                                                            */

/* Independent walker: count nodes and max depth of the fixture. */
static void fixture_walk(uint32_t *nodes, uint32_t *depth)
{
    const uint8_t *b = qemu_virt82_dtb;
    uint32_t o = rd32(b + H_OFF_ST), d = 0u, n = 0u, md = 0u;
    for (uint32_t guard = 0u; guard < 100000u; guard++) {
        uint32_t t = rd32(b + o);
        o += 4u;
        if (t == T_BEGIN) {
            n++;
            d++;
            if (d > md)
                md = d;
            while (b[o] != 0u)
                o++;
            o = (o + 4u) & ~3u;
        } else if (t == T_END_NODE) {
            d--;
        } else if (t == T_PROP) {
            uint32_t l = rd32(b + o);
            o += 8u + ((l + 3u) & ~3u);
        } else if (t == T_END) {
            break;
        } else {
            CHECK(t == T_NOP);
        }
    }
    *nodes = n;
    *depth = md;
}

static void test_fixture(void)
{
    uint32_t nodes, depth;
    fixture_walk(&nodes, &depth);

    CHECK(QEMU_VIRT82_DTB_LEN < QEMU_VIRT82_DTB_TOTALSIZE); /* padding not present */
    want_(parse_bytes(qemu_virt82_dtb, QEMU_VIRT82_DTB_LEN, &C, &EO), JE_FDT_OK, __LINE__);
    CHECK(EO == 0u);
    CHECK(C.addr_cells == 2u && C.size_cells == 2u);
    CHECK(C.ram_count == 1u);
    CHECK(C.ram[0].base == 0x40000000u && C.ram[0].size == 0x10000000u);
    CHECK(C.rsv_count == 0u && C.rsv_ignored == 0u && C.rsv_dynamic == 0u);
    CHECK(!C.has_initrd && C.initrd.base == 0u && C.initrd.size == 0u);
    CHECK(C.totalsize == 1048576u && C.totalsize == QEMU_VIRT82_DTB_TOTALSIZE);
    CHECK(C.used == 7426u && C.used == QEMU_VIRT82_DTB_LEN);
    CHECK(C.node_count == nodes);
    CHECK(C.max_depth == depth);
    {
        uint32_t ost = rd32(qemu_virt82_dtb + H_OFF_ST);
        uint32_t sst = rd32(qemu_virt82_dtb + H_SZ_ST);
        CHECK(C.ram_off[0] >= ost && C.ram_off[0] < ost + sst);
    }
    je_boot_ctx ref = C;

    /* Same blob with (zero) padding present: identical context. */
    {
        static uint8_t pad[16384];
        memset(pad, 0, sizeof pad);
        memcpy(pad, qemu_virt82_dtb, QEMU_VIRT82_DTB_LEN);
        want_(parse_bytes(pad, sizeof pad, &C, &EO), JE_FDT_OK, __LINE__);
        CHECK(ctx_eq(&C, &ref));
        /* Exactly the copy window (glue's real call shape). */
        static uint8_t win[JE_FDT_MAX_COPY];
        memset(win, 0, sizeof win);
        memcpy(win, qemu_virt82_dtb, QEMU_VIRT82_DTB_LEN);
        want_(parse_bytes(win, sizeof win, &C, &EO), JE_FDT_OK, __LINE__);
        CHECK(ctx_eq(&C, &ref));
    }

    /* totalsize boundaries on the real fixture. */
    {
        static uint8_t m[QEMU_VIRT82_DTB_LEN];
        memcpy(m, qemu_virt82_dtb, sizeof m);
        be32(m + H_TOTAL, JE_FDT_MAX_TOTAL);
        want_(parse_bytes(m, sizeof m, &C, &EO), JE_FDT_OK, __LINE__);
        CHECK(C.totalsize == JE_FDT_MAX_TOTAL);
        be32(m + H_TOTAL, JE_FDT_MAX_TOTAL + 1u);
        want_(parse_bytes(m, sizeof m, &C, &EO), JE_FDT_TOO_LARGE, __LINE__);
        CHECK(EO == 0u);
        be32(m + H_TOTAL, QEMU_VIRT82_DTB_LEN); /* totalsize == used */
        want_(parse_bytes(m, sizeof m, &C, &EO), JE_FDT_OK, __LINE__);
        CHECK(C.totalsize == QEMU_VIRT82_DTB_LEN && C.used == QEMU_VIRT82_DTB_LEN);
        be32(m + H_TOTAL, QEMU_VIRT82_DTB_LEN - 1u); /* strings end > totalsize */
        want_(parse_bytes(m, sizeof m, &C, &EO), JE_FDT_BAD_LAYOUT, __LINE__);
    }
}

/* ------------------------------------------------------------------------- */
/* 3. positive builder cases                                                  */

static void test_positive_cells(void)
{
    /* 1/1 cells */
    b_reset();
    root_open(1u, 1u);
    mem("memory@40000000", 1u, 1u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.addr_cells == 1u && C.size_cells == 1u);
    CHECK(C.ram_count == 1u && C.ram[0].base == RAM_B && C.ram[0].size == RAM_S);
    CHECK(off_in_st(C.ram_off[0]));
    CHECK(C.totalsize == I.len && C.used == I.len);
    CHECK(C.node_count == 2u && C.max_depth == 2u);
    CHECK(C.rsv_count == 0u && !C.has_initrd);

    /* 2/2 cells, 64-bit base above 4 GiB */
    b_reset();
    root_open(2u, 2u);
    mem("memory@100000000", 2u, 2u, 0x100000000ull, 0x20000000u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.addr_cells == 2u && C.size_cells == 2u);
    CHECK(C.ram_count == 1u && C.ram[0].base == 0x100000000ull && C.ram[0].size == 0x20000000u);

    /* mixed 2/1 and 1/2 cells */
    b_reset();
    root_open(2u, 1u);
    mem("m", 2u, 1u, 0x200000000ull, 0x1000u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.addr_cells == 2u && C.size_cells == 1u && has_ram(&C, 0x200000000ull, 0x1000u));
    b_reset();
    root_open(1u, 2u);
    mem("m", 1u, 2u, 0x80000000u, 0x100000000ull);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.addr_cells == 1u && C.size_cells == 2u && has_ram(&C, 0x80000000u, 0x100000000ull));

    /* largest non-overflowing range: base+size == UINT64_MAX (exclusive end) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, 0xfffffffffffff000ull, 0xfffu);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(has_ram(&C, 0xfffffffffffff000ull, 0xfffu));
}

static void test_positive_memory(void)
{
    /* Multiple memory nodes aggregate; one node may carry several pairs;
     * adjacent ranges are fine; the node name is not used. */
    b_reset();
    root_open(2u, 2u);
    mem("memory@40000000", 2u, 2u, 0x40000000u, 0x10000000u);
    {
        const uint64_t p[4] = { 0x50000000u, 0x10000000u, 0x100000000ull, 0x40000000u };
        b_begin("not-called-memory");
        b_s("device_type", "memory");
        b_reg(2u, 2u, 2u, p);
        b_end();
    }
    mem("memory@200000000", 2u, 2u, 0x200000000ull, 0x1000u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.ram_count == 4u);
    CHECK(has_ram(&C, 0x40000000u, 0x10000000u));
    CHECK(has_ram(&C, 0x50000000u, 0x10000000u));
    CHECK(has_ram(&C, 0x100000000ull, 0x40000000u));
    CHECK(has_ram(&C, 0x200000000ull, 0x1000u));
    for (uint32_t i = 0u; i < C.ram_count; i++)
        CHECK(off_in_st(C.ram_off[i]));
    CHECK(C.node_count == 4u);

    /* device_type AFTER reg (classified at END_NODE). */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_s("device_type", "memory");
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.ram_count == 1u && has_ram(&C, RAM_B, RAM_S));

    /* status: "disabled" skipped (even without reg, even with a zero-size or
     * overlapping reg), "okay"/"ok" kept, "fail" and "OK" skipped. Disabled
     * nodes still counted. (An EMPTY status is BAD_STRING since contract
     * 5a3c14b; see test_status_values.) */
    b_reset();
    root_open(2u, 2u);
    b_begin("off1");
    b_s("device_type", "memory");
    b_s("status", "disabled");
    b_end(); /* no reg: would be BAD_RANGE if not skipped */
    b_begin("off2");
    b_s("status", "fail");
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S); /* overlaps the kept range: would be OVERLAP */
    b_end();
    b_begin("off3");
    b_s("device_type", "memory");
    b_s("status", "OK");        /* exact match only: "OK" is disabled */
    b_reg1(2u, 2u, 0x0u, 0x0u); /* zero size: would be BAD_RANGE */
    b_end();
    b_begin("on1");
    b_s("device_type", "memory");
    b_s("status", "okay");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    b_begin("on2");
    b_s("status", "ok");
    b_s("device_type", "memory");
    b_reg1(2u, 2u, 0x80000000u, 0x1000u);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.ram_count == 2u && has_ram(&C, RAM_B, RAM_S) && has_ram(&C, 0x80000000u, 0x1000u));
    CHECK(C.node_count == 6u);

    /* NOPs everywhere (leading, between, inside, after the root's END_NODE). */
    b_reset();
    b_nop();
    b_nop();
    b_begin("");
    b_nop();
    b_u32("#address-cells", 2u);
    b_nop();
    b_u32("#size-cells", 2u);
    b_begin("m");
    b_nop();
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_nop();
    b_end();
    b_nop();
    b_end();
    b_nop();
    b_nop();
    b_endtok();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.ram_count == 1u && C.node_count == 2u);
}

static void test_positive_layout(void)
{
    /* Gaps between blocks (">=" ordering) and a non-40 rsvmap offset; garbage in
     * the gaps must be ignored. totalsize > len (padding absent). */
    b_reset();
    B.rsv_gap = 8u;
    B.st_gap = 16u;
    B.str_gap = 12u;
    b_rsv(RAM_B + 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    CHECK(I.off_rsv == 48u);
    h_set(H_TOTAL, I.len + 4096u);
    EXPECT(JE_FDT_OK);
    CHECK(C.totalsize == I.len + 4096u && C.used == I.len);
    CHECK(C.rsv_count == 1u && C.rsv_off[0] == I.off_rsv);

    /* version 17 / last_comp 17 and version 18 / last_comp 17 accepted. */
    build_simple();
    h_set(H_LCV, 17u);
    EXPECT(JE_FDT_OK);
    h_set(H_VER, 18u);
    EXPECT(JE_FDT_OK);

    /* name of exactly JE_FDT_MAX_NAME bytes accepted */
    {
        char name[JE_FDT_MAX_NAME + 1u];
        memset(name, 'a', JE_FDT_MAX_NAME);
        name[JE_FDT_MAX_NAME] = '\0';
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin(name);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
    }

    /* nameoff into the MIDDLE of a shared string: "xdevice_type"+1 and
     * "#size-cells"... built explicitly with raw nameoffs. */
    b_reset();
    {
        uint32_t s_long = b_str_new("xdevice_type");
        uint32_t s_cells = b_str_new("linux,#address-cells");
        b_begin("");
        b_prop_off(s_cells + 6u, "\0\0\0\2", 4u); /* "#address-cells" */
        b_u32("#size-cells", 2u);
        b_begin("m");
        b_prop_off(s_long + 1u, "memory", 7u); /* "device_type" */
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
    }
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.addr_cells == 2u && C.ram_count == 1u && has_ram(&C, RAM_B, RAM_S));
}

static void test_positive_rsv(void)
{
    /* memreserve: inside RAM kept (incl. the whole RAM range), outside ignored,
     * addr==0 with size!=0 is an entry, not the terminator. */
    b_reset();
    b_rsv(RAM_B + 0x1000u, 0x1000u);   /* kept 0 */
    b_rsv(0x80000000u, 0x1000u);       /* ignored */
    b_rsv(RAM_B, RAM_S);               /* kept 1 (exactly the RAM range) */
    b_rsv(0x0u, 0x1000u);              /* ignored (addr 0 is not a terminator) */
    b_rsv(RAM_B + RAM_S, 0x1000u);     /* ignored (adjacent above RAM) */
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 2u && C.rsv_ignored == 3u && C.rsv_dynamic == 0u);
    CHECK(C.rsv[0].base == RAM_B + 0x1000u && C.rsv[0].size == 0x1000u);
    CHECK(C.rsv[1].base == RAM_B && C.rsv[1].size == RAM_S);
    CHECK(C.rsv_off[0] == I.off_rsv && C.rsv_off[1] == I.off_rsv + 32u);
    CHECK(C.rsv_nomap[0] == 0u && C.rsv_nomap[1] == 0u);

    /* exactly JE_FDT_MAX_RSVMAP entries, all kept (also == JE_FDT_MAX_RSV) */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSVMAP; i++)
        b_rsv(RAM_B + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == JE_FDT_MAX_RSVMAP);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSVMAP; i++)
        CHECK(C.rsv[i].base == RAM_B + i * 0x1000u && C.rsv_off[i] == I.off_rsv + 16u * i);

    /* /reserved-memory + memreserve + /chosen initrd, all features together.
     * Reservations overlap each other and the initrd (allowed). */
    b_reset();
    b_rsv(RAM_B + 0x1000u, 0x1000u); /* rsv[0] (memreserve first) */
    root_open(2u, 2u);
    mem("memory@40000000", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("a@48000000", 2u, 2u, 0x48000000u, 0x100000u, true);     /* rsv[1] no-map */
    rm_child("b", 2u, 2u, RAM_B + 0x1800u, 0x1000u, false);           /* rsv[2] overlaps rsv[0] */
    b_begin("dyn");
    b_be("size", 0x400000u, 8u);
    b_end();                                                  /* rsv_dynamic */
    b_begin("off");
    b_s("status", "disabled");
    b_reg1(2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u);         /* partial; skipped */
    b_end();
    rm_child("out", 2u, 2u, 0x10000000u, 0x1000u, true);              /* ignored */
    b_begin("c");
    b_s("status", "okay");
    b_reg1(2u, 2u, 0x44000000u, 0x1000u);                     /* rsv[3] overlaps initrd */
    b_begin("grandchild");
    b_reg1(2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u);         /* depth 4: not a reservation */
    b_end();
    b_end();
    b_end(); /* reserved-memory */
    chosen_initrd(0x44000000u, 4u, 0x44100000u, 4u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 4u && C.rsv_ignored == 1u && C.rsv_dynamic == 1u);
    CHECK(C.rsv[0].base == RAM_B + 0x1000u && C.rsv_nomap[0] == 0u && C.rsv_off[0] == I.off_rsv);
    CHECK(C.rsv[1].base == 0x48000000u && C.rsv[1].size == 0x100000u && C.rsv_nomap[1] == 1u);
    CHECK(C.rsv[2].base == RAM_B + 0x1800u && C.rsv_nomap[2] == 0u);
    CHECK(C.rsv[3].base == 0x44000000u && C.rsv[3].size == 0x1000u && C.rsv_nomap[3] == 0u);
    for (uint32_t i = 1u; i < 4u; i++)
        CHECK(off_in_st(C.rsv_off[i]));
    CHECK(C.has_initrd && C.initrd.base == 0x44000000u && C.initrd.size == 0x100000u);
    CHECK(C.node_count == 11u && C.max_depth == 4u);

    /* 16 kept + extra reservations OUTSIDE RAM: ignored ones do not count. */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        b_rsv(RAM_B + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("o1", 2u, 2u, 0x1000u, 0x1000u, false);
    rm_child("o2", 2u, 2u, 0x90000000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == JE_FDT_MAX_RSV && C.rsv_ignored == 2u);

    /* /reserved-memory with 1/1 cells matching a 1/1 root. */
    b_reset();
    root_open(1u, 1u);
    mem("m", 1u, 1u, RAM_B, RAM_S);
    b_begin("reserved-memory");
    b_u32("#size-cells", 1u);
    b_empty("ranges");
    b_u32("#address-cells", 1u);
    rm_child("r", 1u, 1u, RAM_B, 0x1000u, true);     /* reg with the 1/1 cells */
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 1u && C.rsv[0].base == RAM_B && C.rsv[0].size == 0x1000u &&
          C.rsv_nomap[0] == 1u);
}

static void test_positive_initrd(void)
{
    static const uint32_t widths[4][2] = { { 4u, 4u }, { 8u, 8u }, { 4u, 8u }, { 8u, 4u } };
    for (uint32_t w = 0u; w < 4u; w++) {
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        chosen_initrd(0x48000000u, widths[w][0], 0x48800000u, widths[w][1]);
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.has_initrd && C.initrd.base == 0x48000000u && C.initrd.size == 0x800000u);
    }

    /* 8-byte bounds above 4 GiB; end order before start in the node. */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, 0x100000000ull, 0x100000000ull);
    b_begin("chosen");
    b_be("linux,initrd-end", 0x180000000ull, 8u);
    b_s("bootargs", "console=ttyAMA0");
    b_be("linux,initrd-start", 0x170000000ull, 8u);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.has_initrd && C.initrd.base == 0x170000000ull && C.initrd.size == 0x10000000u);

    /* initrd exactly covering a RAM range; chosen without initrd -> none. */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    chosen_initrd(RAM_B, 8u, RAM_B + RAM_S, 8u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.has_initrd && C.initrd.base == RAM_B && C.initrd.size == RAM_S);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_s("bootargs", "x");
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(!C.has_initrd && C.initrd.base == 0u && C.initrd.size == 0u);
}

/* Most-literal readings of scope rules (see WORKER_NOTES.md). */
static void test_positive_literal_scope(void)
{
    b_reset();
    root_open(2u, 2u);
    b_s("compatible", "a");
    b_s("compatible", "b");                    /* non-consumed repeat in root: OK */
    b_begin("m");
    b_u32("#address-cells", 1u);               /* memory's own cells do not govern its reg */
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);              /* parsed with ROOT 2/2 cells */
    b_end();
    b_begin("uart@9000000");
    b_u32("reg", 0x09000000u);                 /* non-memory depth-2 reg: no RAM */
    b_s("status", "okay");
    b_end();
    b_begin("soc");
    b_u32("#address-cells", 1u);               /* repeats the root's names: per-node */
    b_u32("#size-cells", 1u);                  /* sets, so NOT a DUPLICATE */
    b_begin("memory@0");                       /* depth 3: not a memory node */
    b_s("device_type", "memory");
    b_reg1(1u, 1u, RAM_B + RAM_S - 0x10u, 0x100u);
    b_end();
    b_begin("chosen");                         /* depth 3: not /chosen */
    b_u32("linux,initrd-start", 1u);
    b_end();
    b_end();
    b_begin("chosen@0");                       /* not exactly "chosen" */
    b_u32("linux,initrd-start", 1u);
    b_end();
    b_begin("reserved-memory@0");              /* not exactly "reserved-memory" */
    b_begin("r");
    b_reg1(2u, 2u, RAM_B + RAM_S - 0x10u, 0x100u);
    b_end();
    b_end();
    b_begin("mem2");
    b_s("device_type", "Memory");              /* not exactly "memory" */
    b_end();
    b_begin("mem3");
    b_s("device_type", "memor");
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.ram_count == 1u && has_ram(&C, RAM_B, RAM_S));
    CHECK(C.rsv_count == 0u && C.rsv_ignored == 0u && !C.has_initrd);
}

/* ------------------------------------------------------------------------- */
/* 4. negative: header, layout, rsvmap                                        */

static void test_bad_arg_parse(void)
{
    uint8_t *p = malloc(QEMU_VIRT82_DTB_LEN);
    CHECK(p != NULL);
    memcpy(p, qemu_virt82_dtb, QEMU_VIRT82_DTB_LEN);
    je_boot_ctx c;
    uint32_t eo;

    memset(&c, 0xa5, sizeof c);
    eo = 0xdeadbeefu;
    CHECK(je_fdt_parse(NULL, QEMU_VIRT82_DTB_LEN, &c, &eo) == JE_FDT_BAD_ARG);
    CHECK(ctx_is_zero(&c) && eo == 0u);
    memset(&c, 0xa5, sizeof c);
    eo = 0xdeadbeefu;
    CHECK(je_fdt_parse(NULL, 0u, &c, &eo) == JE_FDT_BAD_ARG); /* before TRUNCATED */
    CHECK(ctx_is_zero(&c) && eo == 0u);
    eo = 0xdeadbeefu;
    CHECK(je_fdt_parse(p, QEMU_VIRT82_DTB_LEN, NULL, &eo) == JE_FDT_BAD_ARG);
    CHECK(eo == 0u);
    memset(&c, 0xa5, sizeof c);
    CHECK(je_fdt_parse(p, QEMU_VIRT82_DTB_LEN, &c, NULL) == JE_FDT_BAD_ARG);
    CHECK(ctx_is_zero(&c));
    free(p);
}

static void test_header(void)
{
    /* TRUNCATED: len < 40 (err_off 0), and a block byte at/after len. */
    build_simple();
    want_(parse_img_len(39u), JE_FDT_TRUNCATED, __LINE__);
    CHECK(EO == 0u);
    want_(parse_img_len(0u), JE_FDT_TRUNCATED, __LINE__);
    want_(parse_img_len(40u), JE_FDT_TRUNCATED, __LINE__);
    want_(parse_img_len(I.len - 1u), JE_FDT_TRUNCATED, __LINE__);
    want_(parse_img_len(I.off_st + 4u), JE_FDT_TRUNCATED, __LINE__);

    /* BAD_MAGIC (err_off 0), incl. byte-swapped magic. */
    build_simple();
    h_set(H_MAGIC, 0xedfe0dd0u);
    EXPECT_AT(JE_FDT_BAD_MAGIC, 0u);
    h_set(H_MAGIC, JE_FDT_MAGIC ^ 1u);
    EXPECT_AT(JE_FDT_BAD_MAGIC, 0u);

    /* TOO_LARGE */
    build_simple();
    h_set(H_TOTAL, JE_FDT_MAX_TOTAL + 1u);
    EXPECT_AT(JE_FDT_TOO_LARGE, 0u);
    h_set(H_TOTAL, 0xffffffffu);
    EXPECT_AT(JE_FDT_TOO_LARGE, 0u);

    /* BAD_VERSION */
    build_simple();
    h_set(H_VER, 16u);
    EXPECT_AT(JE_FDT_BAD_VERSION, 0u);
    build_simple();
    h_set(H_VER, 0u);
    EXPECT_AT(JE_FDT_BAD_VERSION, 0u);
    build_simple();
    h_set(H_LCV, 18u);
    EXPECT_AT(JE_FDT_BAD_VERSION, 0u);

    /* header check ORDER */
    build_simple();
    h_set(H_MAGIC, 0u);
    want_(parse_img_len(39u), JE_FDT_TRUNCATED, __LINE__);  /* len before magic */
    h_set(H_TOTAL, JE_FDT_MAX_TOTAL + 1u);
    EXPECT(JE_FDT_BAD_MAGIC);                                /* magic before size */
    build_simple();
    h_set(H_TOTAL, JE_FDT_MAX_TOTAL + 1u);
    h_set(H_VER, 1u);
    EXPECT(JE_FDT_TOO_LARGE);                                /* size before version */
    build_simple();
    h_set(H_VER, 1u);
    h_set(H_OFF_RSV, 3u);
    EXPECT(JE_FDT_BAD_VERSION);                              /* version before layout */
    build_simple();
    h_set(H_OFF_ST, I.off_st + 2u);
    want_(parse_img_len(48u), JE_FDT_BAD_LAYOUT, __LINE__);  /* layout before TRUNCATED */
}

/* Zero-size blocks are valid, including one starting exactly at totalsize and
 * one starting exactly at `len` (it has no byte at/after len, so it is not
 * TRUNCATED) — contract update bdf446a. Only the strings block can be empty in
 * a parseable blob (the rsvmap always has its terminator, the struct block its
 * END). With no strings no PROP can be named, so the root cannot carry its
 * cells: a blob with an empty strings block can never parse OK. The "positive"
 * proof is therefore that header/layout/truncation all PASS and the walk runs to
 * the next rule: root cells absent -> BAD_CELLS at an exact struct token (or
 * BAD_STRING for a PROP's nameoff), never BAD_LAYOUT or TRUNCATED. */
static void test_zero_size_strings(void)
{
    uint32_t end_at, child_at;

    /* empty strings at struct end == totalsize == len */
    b_reset();
    b_begin("");
    end_at = b_end();
    b_endtok();
    b_finish();
    CHECK(I.size_str == 0u && I.off_str == I.len && I.off_str == I.off_st + I.size_st);
    EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + end_at);
    /* same with a child node: decision at the child's BEGIN_NODE */
    b_reset();
    b_begin("");
    child_at = b_begin("c");
    b_end();
    b_end();
    b_endtok();
    b_finish();
    CHECK(I.size_str == 0u && I.off_str == I.len);
    EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
    /* empty strings after a gap, starting exactly at totalsize == len */
    b_reset();
    B.str_gap = 8u;
    b_begin("");
    end_at = b_end();
    b_endtok();
    b_finish();
    CHECK(I.size_str == 0u && I.off_str == I.len && I.off_str > I.off_st + I.size_st);
    EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + end_at);
    /* empty strings starting exactly at `len`, with totalsize > len (padding
     * absent from the copy) */
    b_reset();
    b_begin("");
    end_at = b_end();
    b_endtok();
    b_finish();
    h_set(H_TOTAL, I.len + 64u);
    EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + end_at);
    b_reset();
    B.str_gap = 12u;
    b_begin("");
    child_at = b_begin("c");
    b_end();
    b_end();
    b_endtok();
    b_finish();
    h_set(H_TOTAL, I.len + 4096u);
    CHECK(I.off_str == I.len);
    EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
    /* empty strings at totalsize with totalsize < len is not exercised: the
     * glue never copies more than totalsize (copy_len = min(...)). */

    /* a PROP with an empty strings block: nameoff 0 >= size_dt_strings 0 ->
     * BAD_STRING, reached only if the layout was accepted */
    b_reset();
    b_begin("");
    child_at = b_prop_off(0u, "\0\0\0\2", 4u);
    b_end();
    b_endtok();
    b_finish();
    CHECK(I.size_str == 0u && I.off_str == I.len);
    EXPECT_BAD_NAMEOFF_AT(child_at);

    /* Struct side (Codex P2 round 2): a zero-size STRUCT block is layout-valid
     * and, starting exactly at `len`, is not TRUNCATED (it has no byte at/after
     * len). It can never parse: the walk needs at least BEGIN_NODE..END, so the
     * first token runs past the (empty) block -> BAD_LAYOUT, never TRUNCATED.
     * Here struct and strings are both empty and both start exactly at len,
     * with totalsize > len. (The rsvmap has no size field: its terminator is
     * always required, so it cannot be "zero-size".) */
    build_simple();
    {
        uint32_t at = I.off_rsv + 16u; /* rsvmap terminator end */
        h_set(H_OFF_ST, at);
        h_set(H_SZ_ST, 0u);
        h_set(H_OFF_STR, at);
        h_set(H_SZ_STR, 0u);
        want_(parse_img_len(at), JE_FDT_BAD_LAYOUT, __LINE__);
    }

    /* negative control: the empty block one word PAST totalsize is BAD_LAYOUT */
    b_reset();
    b_begin("");
    b_end();
    b_endtok();
    b_finish();
    h_set(H_OFF_STR, I.len + 4u);
    EXPECT(JE_FDT_BAD_LAYOUT);
}

static void test_layout(void)
{
    /* off_mem_rsvmap < 40 */
    build_simple();
    h_set(H_OFF_RSV, 32u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    build_simple();
    h_set(H_OFF_RSV, 0u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* off_mem_rsvmap not 8-aligned (rsvmap itself built at 44) */
    b_reset();
    B.rsv_gap = 4u;
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* off_dt_struct not 4-aligned */
    build_simple();
    h_set(H_OFF_ST, I.off_st + 2u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* size_dt_struct not a multiple of 4 */
    build_simple();
    h_set(H_SZ_ST, I.size_st - 2u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* struct end > strings start */
    build_simple();
    h_set(H_SZ_ST, I.size_st + 4u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* order: strings before struct */
    build_simple();
    h_set(H_OFF_STR, I.off_rsv);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* order: struct at rsvmap start / inside the terminator */
    build_simple();
    h_set(H_OFF_ST, I.off_rsv);
    EXPECT(JE_FDT_BAD_LAYOUT);
    build_simple();
    h_set(H_OFF_ST, I.off_rsv + 8u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* order: rsvmap after struct */
    build_simple();
    h_set(H_OFF_RSV, I.off_str + 8u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* block end beyond totalsize */
    build_simple();
    h_set(H_TOTAL, I.len - 1u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* totalsize < 40 (passes TOO_LARGE, not a valid layout) */
    build_simple();
    h_set(H_TOTAL, 39u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* u32 wrap-around must not pass (u64 arithmetic required) */
    build_simple();
    h_set(H_SZ_STR, 0xfffffff0u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    build_simple();
    h_set(H_SZ_STR, 0u - I.off_str);       /* off_str + size wraps to exactly 0 in u32 */
    EXPECT(JE_FDT_BAD_LAYOUT);
    build_simple();
    h_set(H_OFF_STR, 0xfffffff0u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    build_simple();
    h_set(H_SZ_ST, 0xfffffffcu);
    EXPECT(JE_FDT_BAD_LAYOUT);
    build_simple();
    h_set(H_OFF_ST, 0xfffffffcu);
    EXPECT(JE_FDT_BAD_LAYOUT);
    build_simple();
    h_set(H_OFF_RSV, 0xfffffff8u);
    EXPECT(JE_FDT_BAD_LAYOUT);

    /* rsvmap terminator missing before off_dt_struct */
    b_reset();
    B.rsv_noterm = true;
    b_rsv(RAM_B + 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_LAYOUT);
    b_reset();
    B.rsv_noterm = true; /* no entries at all: struct begins at off_mem_rsvmap */
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* terminator present but straddling off_dt_struct */
    build_simple();
    h_set(H_OFF_ST, I.off_rsv + 12u);
    EXPECT(JE_FDT_BAD_LAYOUT);

    /* struct block ends without END (token running past the block) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_end();
    b_finish();
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* PROP header running past the block */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    st_word(T_PROP);
    b_finish();
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* PROP value running past the block (strings/padding follow, so this is
     * not TRUNCATED) - and with a length that would wrap 32-bit arithmetic */
    {
        uint32_t reg_at;
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        reg_at = b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        st_patch(reg_at + 4u, I.size_st);
        EXPECT(JE_FDT_BAD_LAYOUT);
        st_patch(reg_at + 4u, 0xfffffffdu);
        EXPECT(JE_FDT_BAD_LAYOUT);
        st_patch(reg_at + 4u, 0xffffffffu);
        EXPECT(JE_FDT_BAD_LAYOUT);
    }
    /* END not the last word of the block */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_nop();
    b_finish();
    EXPECT(JE_FDT_BAD_LAYOUT);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    st_word(0u);
    b_finish();
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* struct block ends in the middle of the END token's region: size cut so
     * END is absent from the block although present in the bytes. */
    build_simple();
    h_set(H_SZ_ST, I.size_st - 4u);
    EXPECT(JE_FDT_BAD_LAYOUT);
    /* zero-size struct block: the first token runs past the block */
    build_simple();
    h_set(H_SZ_ST, 0u);
    EXPECT(JE_FDT_BAD_LAYOUT);

    test_zero_size_strings();
}

static void test_rsvmap(void)
{
    /* size == 0 && addr != 0 */
    b_reset();
    b_rsv(RAM_B + 0x1000u, 0x1000u);
    b_rsv(RAM_B + 0x2000u, 0u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    CHECK(EO >= I.off_rsv && EO < I.off_st);
    /* addr + size overflow (exactly 2^64, and beyond) */
    b_reset();
    b_rsv(0xfffffffffffff000ull, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    b_reset();
    b_rsv(0x8000000000000000ull, 0x8000000000000001ull);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    /* > JE_FDT_MAX_RSVMAP entries (all outside RAM, so only the count is wrong) */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSVMAP + 1u; i++)
        b_rsv(0x80000000u + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT(JE_FDT_RANGE_LIMIT);
    /* rsvmap RANGE_LIMIT precedes a struct defect */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSVMAP + 1u; i++)
        b_rsv(0x80000000u + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    st_word(0x77u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_RANGE_LIMIT);
}

/* ------------------------------------------------------------------------- */
/* 4. negative: struct tokens, strings, limits                                */

static void test_tokens(void)
{
    static const uint32_t bad[] = { 0u, 5u, 6u, 7u, 8u, 10u, 0x01000000u, 0x00000103u,
                                    0xffffffffu };
    for (uint32_t k = 0u; k < sizeof bad / sizeof bad[0]; k++) {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        at = st_word(bad[k]);
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
    }
    /* first token is not BEGIN_NODE */
    b_reset();
    b_u32("#address-cells", 2u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st);
    b_reset();
    b_nop();
    b_end();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + 4u);
    /* root with a non-empty, terminated name -> BAD_TOKEN at the root's
     * BEGIN_NODE (contract 5a3c14b), also after leading NOPs */
    {
        static const char *const rn[] = { "/", "a", "abcd" };
        for (uint32_t k = 0u; k < 3u; k++) {
            uint32_t at;
            b_reset();
            if (k == 2u)
                b_nop();
            at = b_begin(rn[k]);
            b_u32("#address-cells", 2u);
            b_u32("#size-cells", 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
        }
    }
    /* second root */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_end();
        at = b_begin("");
        b_end();
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
        /* with NOPs between */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_end();
        b_nop();
        at = b_begin("x");
        b_end();
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
    }
    /* PROP after a child node: in the root and in a depth-2 node */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        at = b_s("model", "x");
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_begin("sub");
        b_end();
        at = b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
    }
    /* END_NODE at depth 0 */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_end();
        at = b_end();
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
    }
    /* END with depth != 0 (depth 1 and depth 2) */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        at = b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        at = b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
    }
    /* trailing non-NOP/END tokens after the root's END_NODE */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_end();
        b_nop();
        at = b_s("x", "y");
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_end();
        at = st_word(0x42u);
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
    }
}

static void test_strings(void)
{
    /* node name > JE_FDT_MAX_NAME */
    {
        char name[JE_FDT_MAX_NAME + 2u];
        memset(name, 'a', JE_FDT_MAX_NAME + 1u);
        name[JE_FDT_MAX_NAME + 1u] = '\0';
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin(name);
        b_end();
        root_close();
        b_finish();
        EXPECT_IN_ST(JE_FDT_BAD_STRING);
    }
    /* nameoff == size_dt_strings, and far beyond (in a leaf node, so the PROP
     * is structurally legal) */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("leaf");
        at = b_prop_off(0u, NULL, 0u);
        b_end();
        root_close();
        b_finish();
        st_patch(at + 8u, I.size_str);
        EXPECT_BAD_NAMEOFF_AT(at);
        st_patch(at + 8u, 0xffffffffu);
        EXPECT_BAD_NAMEOFF_AT(at);
        st_patch(at + 8u, I.size_str - 1u); /* the final NUL: empty name, valid */
        EXPECT(JE_FDT_OK);
    }
    /* no NUL before the end of the strings block (a bad nameoff too: err_off
     * is the PROP whose name runs off the block; all earlier names are fine) */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("leaf");
        at = b_prop_off(b_str_new("zzunterminated"), NULL, 0u);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);                          /* control */
        I.buf[I.off_str + I.size_str - 1u] = 'z';  /* kill the final NUL */
        EXPECT_BAD_NAMEOFF_AT(at);
        /* ... even though a NUL exists in the bytes just after the block */
        b_reset();
        B.str_gap = 0u;
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("leaf");
        at = b_prop_off(b_str_new("zzunterminated"), NULL, 0u);
        b_end();
        root_close();
        b_finish();
        h_set(H_SZ_STR, I.size_str - 1u);          /* block excludes the NUL */
        EXPECT_BAD_NAMEOFF_AT(at);
    }
}

/* Node name with no NUL before the struct-block end. Contract update bdf446a
 * (W2 ambiguity A1) pins this to BAD_STRING; BAD_LAYOUT no longer covers names.
 * The strings block (which contains NULs) follows the struct block directly, so
 * a parser that searches for the NUL past the struct end is caught too. */
static void test_unterminated_name(void)
{
    /* short name, struct ends inside it */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    st_word(T_BEGIN);
    st_bytes("abcd", 4u);
    b_finish();
    EXPECT_IN_ST(JE_FDT_BAD_STRING);
    /* ... and with the strings block NOT adjacent (garbage gap without NULs) */
    b_reset();
    B.str_gap = 8u;
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    st_word(T_BEGIN);
    st_bytes("abcd", 4u);
    b_finish();
    EXPECT_IN_ST(JE_FDT_BAD_STRING);
    /* name both unterminated and longer than JE_FDT_MAX_NAME */
    {
        char name[JE_FDT_MAX_NAME + 41u];
        memset(name, 'a', sizeof name);
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        st_word(T_BEGIN);
        st_bytes(name, (uint32_t)sizeof name);
        b_finish();
        EXPECT_IN_ST(JE_FDT_BAD_STRING);
    }
    /* ROOT position (ambiguity A20, resolved by contract 5a3c14b: the name rules
     * are checked first -> BAD_STRING, not BAD_TOKEN): unterminated root name,
     * and a terminated root name > JE_FDT_MAX_NAME bytes */
    b_reset();
    st_word(T_BEGIN);
    st_bytes("abcd", 4u);
    b_finish();
    EXPECT_IN_ST(JE_FDT_BAD_STRING);
    {
        char name[JE_FDT_MAX_NAME + 2u];
        memset(name, 'r', JE_FDT_MAX_NAME + 1u);
        name[JE_FDT_MAX_NAME + 1u] = '\0';
        b_reset();
        b_begin(name);
        b_u32("#address-cells", 2u);
        b_u32("#size-cells", 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        root_close();
        b_finish();
        EXPECT_IN_ST(JE_FDT_BAD_STRING);
    }
}

static void nest(uint32_t extra_levels)
{
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    for (uint32_t i = 0u; i < extra_levels; i++)
        b_begin("n");
    for (uint32_t i = 0u; i < extra_levels; i++)
        b_end();
    root_close();
}

static void flat(uint32_t total_nodes)
{
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    for (uint32_t i = 2u; i < total_nodes; i++) {
        b_begin("n");
        b_end();
    }
    root_close();
}

static void props(uint32_t nprops)
{
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("p");
    for (uint32_t i = 0u; i < nprops; i++) {
        b_empty("x"); /* same non-consumed name: counts, never DUPLICATE */
        b_nop();      /* NOPs are not properties */
    }
    b_end();
    root_close();
}

static void test_limits(void)
{
    /* depth: root = 1, so MAX_DEPTH - 1 extra levels is the maximum */
    b_reset();
    nest(JE_FDT_MAX_DEPTH - 1u);
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.max_depth == JE_FDT_MAX_DEPTH);
    b_reset();
    nest(JE_FDT_MAX_DEPTH);
    b_finish();
    EXPECT_IN_ST(JE_FDT_DEPTH_LIMIT);

    /* nodes (root included) */
    b_reset();
    flat(JE_FDT_MAX_NODES);
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.node_count == JE_FDT_MAX_NODES);
    b_reset();
    flat(JE_FDT_MAX_NODES + 1u);
    b_finish();
    EXPECT_IN_ST(JE_FDT_NODE_LIMIT);

    /* props in one node */
    b_reset();
    props(JE_FDT_MAX_PROPS_PER_NODE);
    b_finish();
    EXPECT(JE_FDT_OK);
    b_reset();
    props(JE_FDT_MAX_PROPS_PER_NODE + 1u);
    b_finish();
    EXPECT_IN_ST(JE_FDT_PROP_LIMIT);
    /* consumed props count too: root with 2 cells + 63 others = 65 */
    b_reset();
    root_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_PROPS_PER_NODE - 1u; i++)
        b_empty("q");
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT_IN_ST(JE_FDT_PROP_LIMIT);

    /* first violation in file order wins */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    st_word(0x55u);                         /* BAD_TOKEN first ... */
    b_begin("p");
    for (uint32_t i = 0u; i < JE_FDT_MAX_PROPS_PER_NODE + 1u; i++)
        b_empty("x");                       /* ... PROP_LIMIT later */
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_TOKEN);
    b_reset();
    root_open(2u, 2u);
    b_begin("p");
    for (uint32_t i = 0u; i < JE_FDT_MAX_PROPS_PER_NODE + 1u; i++)
        b_empty("x");                       /* PROP_LIMIT first ... */
    b_end();
    st_word(0x55u);                         /* ... BAD_TOKEN later */
    root_close();
    b_finish();
    EXPECT(JE_FDT_PROP_LIMIT);
    /* struct violation beats post-walk NO_MEMORY */
    b_reset();
    root_open(2u, 2u);
    st_word(0x55u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_TOKEN);
}

/* ------------------------------------------------------------------------- */
/* 4. negative: semantics                                                     */

/* DUPLICATE (contract 5a3c14b): a CONSUMED name repeated among the DIRECT
 * properties of one TRACKED node (root, every depth-2 node, every depth-3
 * /reserved-memory child) -> DUPLICATE at the REPEATING PROP token as it is
 * encountered, regardless of status or classification; a second /chosen or
 * /reserved-memory -> DUPLICATE at its BEGIN_NODE. Every case pins err_off. */
static void test_duplicate(void)
{
    uint32_t at;

    /* root cells, repeated with a VALID value */
    b_reset();
    root_open(2u, 2u);
    at = b_u32("#address-cells", 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    at = b_u32("#size-cells", 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* root duplicate #address-cells is reported at the repeat even though a
     * LATER root property is bad (nameoff out of the strings block) */
    {
        uint32_t bad_at;
        b_reset();
        root_open(2u, 2u);
        at = b_u32("#address-cells", 2u);
        bad_at = b_prop_off(0u, NULL, 0u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        root_close();
        b_finish();
        st_patch(bad_at + 8u, 0xfffffff0u);
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    }

    /* memory node: reg, device_type, status; also reg twice before device_type */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    at = b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    at = b_reg1(2u, 2u, 0x80000000u, 0x1000u);
    b_s("device_type", "memory");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    at = b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    b_s("status", "okay");
    at = b_s("status", "okay");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* file order: reg, status, reg, status -> the SECOND reg (earliest repeat) */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_s("status", "okay");
    at = b_reg1(2u, 2u, RAM_B, RAM_S);
    b_s("status", "okay");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* first status "disabled", then a repeat: duplicates are tracked regardless
     * of status (the node would otherwise be skipped -> NO_MEMORY) */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    b_s("status", "disabled");
    at = b_s("status", "okay");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* ... and a disabled memory node repeating reg */
    b_reset();
    root_open(2u, 2u);
    mem("ok", 2u, 2u, 0x80000000u, 0x1000u);
    b_begin("m");
    b_s("status", "disabled");
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    at = b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* device_type "foo" then "memory": regardless of classification */
    b_reset();
    root_open(2u, 2u);
    mem("ok", 2u, 2u, 0x80000000u, 0x1000u);
    b_begin("m");
    b_s("device_type", "foo");
    at = b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* depth-2 nodes that are none of memory / chosen / reserved-memory are
     * still tracked: uart with reg twice, and with #address-cells twice */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("uart@9000000");
    b_u32("reg", 0x09000000u);
    b_s("compatible", "arm,pl011");
    at = b_u32("reg", 0x09000000u);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("soc");
    b_u32("#address-cells", 1u);
    at = b_u32("#address-cells", 1u);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* "chosen@0" (not /chosen) is still a depth-2 tracked node */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen@0");
    b_be("linux,initrd-start", 0x44000000u, 4u);
    at = b_be("linux,initrd-start", 0x44000000u, 4u);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);

    /* per-node scoping: a child repeating its parent's consumed names is NOT a
     * duplicate (root cells in soc / reserved-memory / a memory node; a rsv
     * child's status repeating /reserved-memory's) */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_u32("#address-cells", 2u);
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    b_begin("soc");
    b_u32("#address-cells", 1u);
    b_u32("#size-cells", 1u);
    b_s("status", "okay");
    b_begin("dev@0");
    b_s("status", "okay");
    b_u32("reg", 0u);
    b_end();
    b_end();
    rm_open(2u, 2u);
    b_s("status", "okay");
    b_begin("r");
    b_s("status", "okay");
    b_u32("#address-cells", 2u);
    b_empty("ranges");
    b_reg1(2u, 2u, RAM_B, 0x1000u);
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.ram_count == 1u && C.rsv_count == 1u);
    /* literal reading: a depth-3 node that is not a /reserved-memory child (and
     * a depth-4 node under one) is UNTRACKED, so a repeated consumed name there
     * is not a DUPLICATE */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("soc");
    b_u32("#address-cells", 1u);
    b_u32("#size-cells", 1u);
    b_begin("bus@0");
    b_u32("reg", 0x1u);
    b_u32("reg", 0x2u);
    b_s("status", "okay");
    b_s("status", "okay");
    b_end();
    b_end();
    rm_open(2u, 2u);
    b_begin("r");
    b_reg1(2u, 2u, RAM_B, 0x1000u);
    b_begin("sub");
    b_empty("no-map");
    b_empty("no-map");
    b_end();
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 1u && C.rsv_nomap[0] == 0u);

    /* /reserved-memory node: cells, ranges (repeat precedes the cells decision
     * at its END_NODE) */
    {
        static const char *const dup[] = { "#address-cells", "#size-cells", "ranges" };
        for (uint32_t k = 0u; k < 3u; k++) {
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            rm_open(2u, 2u);
            if (k == 2u)
                at = b_empty("ranges");
            else
                at = b_u32(dup[k], 2u);
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
        }
    }
    /* /reserved-memory child (depth 3, tracked): reg, no-map, size, status;
     * plus a DISABLED child repeating reg */
    for (uint32_t k = 0u; k < 5u; k++) {
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        if (k == 0u) {
            b_reg1(2u, 2u, RAM_B, 0x1000u);
            at = b_reg1(2u, 2u, RAM_B, 0x1000u);
        } else if (k == 1u) {
            b_reg1(2u, 2u, RAM_B, 0x1000u);
            b_empty("no-map");
            at = b_empty("no-map");
        } else if (k == 2u) {
            b_be("size", 0x1000u, 8u);
            at = b_be("size", 0x1000u, 8u);
        } else if (k == 3u) {
            b_s("status", "okay");
            b_reg1(2u, 2u, RAM_B, 0x1000u);
            at = b_s("status", "okay");
        } else {
            b_s("status", "disabled");
            b_reg1(2u, 2u, RAM_B, 0x1000u);
            at = b_reg1(2u, 2u, RAM_B, 0x1000u);
        }
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    }
    /* /chosen: initrd-start, initrd-end */
    for (uint32_t k = 0u; k < 2u; k++) {
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_be("linux,initrd-start", 0x44000000u, 4u);
        b_be("linux,initrd-end", 0x44100000u, 4u);
        if (k == 0u)
            at = b_be("linux,initrd-start", 0x44000000u, 4u);
        else
            at = b_be("linux,initrd-end", 0x44100000u, 4u);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    }
    /* second /chosen, second /reserved-memory (each individually valid): at the
     * second node's BEGIN_NODE */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_end();
    at = b_begin("chosen");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    b_end();
    at = b_begin("reserved-memory");
    b_u32("#address-cells", 2u);
    b_u32("#size-cells", 2u);
    b_empty("ranges");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* "chosen" at depth 3 and "chosen@0" do not make a second /chosen */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_end();
    b_begin("chosen@0");
    b_end();
    b_begin("soc");
    b_begin("chosen");
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
}

/* CONFLICT (contract 5a3c14b): /chosen or /reserved-memory with device_type
 * "memory" -> CONFLICT at that device_type PROP (fail closed). Names match
 * exactly, so "chosen@0" with device_type "memory" is an ordinary memory node. */
static void test_conflict(void)
{
    uint32_t at;

    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_s("bootargs", "x");
    at = b_s("device_type", "memory");
    b_reg1(2u, 2u, 0x80000000u, 0x1000u);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_CONFLICT, I.off_st + at);
    /* /chosen status is ignored: still CONFLICT when disabled */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_s("status", "disabled");
    at = b_s("device_type", "memory");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_CONFLICT, I.off_st + at);
    /* /reserved-memory with device_type "memory", first property */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("reserved-memory");
    at = b_s("device_type", "memory");
    b_u32("#address-cells", 2u);
    b_u32("#size-cells", 2u);
    b_empty("ranges");
    rm_child("r", 2u, 2u, RAM_B, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_CONFLICT, I.off_st + at);
    /* ... CONFLICT at the PROP precedes the cells decision (ranges absent would
     * be BAD_CELLS at the later END_NODE); and a disabled /reserved-memory too */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("reserved-memory");
    b_s("status", "disabled");
    b_u32("#address-cells", 2u);
    b_u32("#size-cells", 2u);
    at = b_s("device_type", "memory");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_CONFLICT, I.off_st + at);
    /* non-"memory" device_type in /chosen and /reserved-memory: no conflict */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_s("device_type", "chosen");
    b_end();
    rm_open(2u, 2u);
    b_s("device_type", "Memory");
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    /* exact names: "chosen@0" and "reserved-memory@0" with device_type memory
     * are ordinary memory nodes */
    b_reset();
    root_open(2u, 2u);
    b_begin("chosen@0");
    b_s("device_type", "memory");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    b_begin("reserved-memory@0");
    b_s("device_type", "memory");
    b_reg1(2u, 2u, 0x80000000u, 0x1000u);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.ram_count == 2u && has_ram(&C, RAM_B, RAM_S) && has_ram(&C, 0x80000000u, 0x1000u));
    CHECK(C.rsv_count == 0u && !C.has_initrd);
}

/* `status` scope and exact matching (contract 5a3c14b). status is okay iff the
 * value is exactly "okay\0" or "ok\0"; any other NUL-terminated value is
 * disabled; an empty value or one whose last byte is not NUL is BAD_STRING at
 * that PROP. The same malformed-value rule applies to device_type. status is
 * IGNORED on /chosen and /reserved-memory; it matters on memory nodes and on
 * /reserved-memory children (subtree skipped, still validated and counted). */
static void test_status_values(void)
{
    uint32_t at;

    /* exact okay values: "okay", "ok" */
    {
        static const char *const ok[] = { "okay", "ok" };
        for (uint32_t k = 0u; k < 2u; k++) {
            b_reset();
            root_open(2u, 2u);
            b_begin("m");
            b_s("status", ok[k]);
            b_s("device_type", "memory");
            b_reg1(2u, 2u, RAM_B, RAM_S);
            b_end();
            root_close();
            b_finish();
            EXPECT(JE_FDT_OK);
            CHECK(C.ram_count == 1u && has_ram(&C, RAM_B, RAM_S));
        }
    }
    /* disabled: "okay\0junk" (len 10, NUL-terminated), "OK", "okayx", "o",
     * "okay " -> the only memory node is skipped -> NO_MEMORY (err_off 0 is not
     * pinned: post-walk) */
    {
        static const struct { const char *v; uint32_t len; } dis[] = {
            { "okay\0junk", 10u }, { "OK", 3u }, { "okayx", 6u }, { "o", 2u },
            { "okay ", 6u }, { "ok\0", 4u },
        };
        for (uint32_t k = 0u; k < sizeof dis / sizeof dis[0]; k++) {
            b_reset();
            root_open(2u, 2u);
            b_begin("m");
            b_s("device_type", "memory");
            b_prop("status", dis[k].v, dis[k].len);
            b_reg1(2u, 2u, RAM_B, RAM_S);
            b_end();
            root_close();
            b_finish();
            EXPECT(JE_FDT_NO_MEMORY);
            /* the same value on a /reserved-memory child: not a candidate, so a
             * PARTIAL range there is not BAD_RANGE */
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            rm_open(2u, 2u);
            b_begin("r");
            b_prop("status", dis[k].v, dis[k].len);
            b_reg1(2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u);
            b_end();
            b_end();
            root_close();
            b_finish();
            EXPECT(JE_FDT_OK);
            CHECK(C.rsv_count == 0u && C.rsv_ignored == 0u);
        }
    }
    /* malformed status (empty; no trailing NUL) -> BAD_STRING at that PROP, on
     * a memory node (before and after device_type) and on a rsv child */
    {
        static const struct { const char *v; uint32_t len; } bad[] = {
            { "", 0u }, { "okay", 4u }, { "ok", 2u }, { "disabled", 8u },
        };
        for (uint32_t k = 0u; k < sizeof bad / sizeof bad[0]; k++) {
            for (uint32_t w = 0u; w < 3u; w++) {
                b_reset();
                root_open(2u, 2u);
                if (w == 2u) {
                    mem("m", 2u, 2u, RAM_B, RAM_S);
                    rm_open(2u, 2u);
                    b_begin("r");
                    b_reg1(2u, 2u, RAM_B, 0x1000u);
                    at = b_prop("status", bad[k].v, bad[k].len);
                    b_end();
                    b_end();
                } else {
                    b_begin("m");
                    if (w == 1u)
                        b_s("device_type", "memory");
                    at = b_prop("status", bad[k].v, bad[k].len);
                    if (w == 0u)
                        b_s("device_type", "memory");
                    b_reg1(2u, 2u, RAM_B, RAM_S);
                    b_end();
                }
                root_close();
                b_finish();
                EXPECT_AT(JE_FDT_BAD_STRING, I.off_st + at);
            }
        }
    }
    /* malformed device_type (empty; no trailing NUL, incl. "memory" len 6) ->
     * BAD_STRING at that PROP, in a depth-2 node */
    {
        static const struct { const char *v; uint32_t len; } bad[] = {
            { "", 0u }, { "memory", 6u }, { "memor", 5u },
        };
        for (uint32_t k = 0u; k < sizeof bad / sizeof bad[0]; k++) {
            b_reset();
            root_open(2u, 2u);
            mem("ok", 2u, 2u, 0x80000000u, 0x1000u);
            b_begin("m");
            at = b_prop("device_type", bad[k].v, bad[k].len);
            b_reg1(2u, 2u, RAM_B, RAM_S);
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_BAD_STRING, I.off_st + at);
        }
        /* device_type "memory\0junk" is NUL-terminated but not exactly
         * "memory": not a memory node */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, 0x80000000u, 0x1000u);
        b_begin("m");
        b_prop("device_type", "memory\0junk", 12u);
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.ram_count == 1u && has_ram(&C, 0x80000000u, 0x1000u));
    }

    /* /chosen status is IGNORED: a disabled /chosen still yields the initrd ... */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_s("status", "disabled");
    b_be("linux,initrd-start", 0x44000000u, 4u);
    b_be("linux,initrd-end", 0x44100000u, 4u);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.has_initrd && C.initrd.base == 0x44000000u && C.initrd.size == 0x100000u);
    /* ... and is still validated (half-present initrd -> BAD_RANGE) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_s("status", "fail");
    b_be("linux,initrd-start", 0x44000000u, 4u);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);

    /* /reserved-memory status is IGNORED: a disabled one still yields
     * reservations (kept, ignored and dynamic) ... */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("reserved-memory");
    b_s("status", "disabled");
    b_u32("#address-cells", 2u);
    b_u32("#size-cells", 2u);
    b_empty("ranges");
    rm_child("k", 2u, 2u, RAM_B + 0x2000u, 0x1000u, true);
    rm_child("o", 2u, 2u, 0x90000000u, 0x1000u, false);
    b_begin("d");
    b_be("size", 0x1000u, 8u);
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 1u && C.rsv[0].base == RAM_B + 0x2000u && C.rsv_nomap[0] == 1u);
    CHECK(C.rsv_ignored == 1u && C.rsv_dynamic == 1u);
    /* ... a partial child range is still BAD_RANGE ... */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("reserved-memory");
    b_u32("#address-cells", 2u);
    b_u32("#size-cells", 2u);
    b_empty("ranges");
    b_s("status", "disabled");
    rm_child("p", 2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    /* ... and it is still cells-checked (ranges absent -> BAD_CELLS at the
     * first child BEGIN_NODE) */
    {
        uint32_t child_at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("reserved-memory");
        b_s("status", "disabled");
        b_u32("#address-cells", 2u);
        b_u32("#size-cells", 2u);
        child_at = b_begin("r");
        b_reg1(2u, 2u, RAM_B, 0x1000u);
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
    }

    /* disabled /reserved-memory CHILD: subtree skipped for semantics but still
     * structurally validated (a bad token inside its grandchild is fatal) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    b_begin("off");
    b_s("status", "disabled");
    b_reg1(2u, 2u, RAM_B, 0x1000u);
    b_begin("g");
    b_reg1(2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u);
    b_end();
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 0u && C.rsv_ignored == 0u && C.node_count == 5u && C.max_depth == 4u);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    b_begin("off");
    b_s("status", "disabled");
    b_begin("g");
    at = st_word(0x66u);
    b_end();
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_BAD_TOKEN, I.off_st + at);
    /* disabled child with zero-size reg / size-only: skipped (no BAD_RANGE, no
     * rsv_dynamic) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    b_begin("z");
    b_reg1(2u, 2u, RAM_B, 0u);
    b_s("status", "disabled");
    b_end();
    b_begin("d");
    b_s("status", "disabled");
    b_be("size", 0x1000u, 8u);
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 0u && C.rsv_dynamic == 0u);
    /* 17 children, one DISABLED: its range is not a candidate -> OK with 16
     * counted (all outside RAM -> rsv_ignored == 16) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++) {
        if (i == 5u) {
            b_begin("off");
            b_reg1(2u, 2u, 0x80000000u + i * 0x1000u, 0x1000u);
            b_s("status", "disabled");
            b_end();
        } else {
            rm_child("r", 2u, 2u, 0x80000000u + i * 0x1000u, 0x1000u, false);
        }
    }
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 0u && C.rsv_ignored == JE_FDT_MAX_RSV);
}

static void test_bad_cells(void)
{
    /* root #address-cells / #size-cells absent: decided when the root's property
     * list ends, err_off = the root's first child BEGIN_NODE (contract bdf446a) */
    {
        uint32_t child_at;
        b_reset();
        b_begin("");
        b_u32("#size-cells", 2u);
        child_at = b_begin("m");
        b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
        b_reset();
        b_begin("");
        b_u32("#address-cells", 2u);
        child_at = b_begin("m");
        b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
        /* both absent, root has other properties; NOPs between the last root
         * property and the child do not move the decision point */
        b_reset();
        b_begin("");
        b_s("compatible", "x");
        b_nop();
        b_nop();
        child_at = b_begin("m");
        b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
        /* the decision precedes a defect later in that child (BAD_TOKEN) and
         * a later child's own semantics */
        b_reset();
        b_begin("");
        b_s("compatible", "x"); /* non-empty strings block: isolate this rule */
        child_at = b_begin("c");
        st_word(0x77u);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
        /* ... and precedes a memory child whose reg could only be decoded with
         * cells (the classification happens at that child's END_NODE) */
        b_reset();
        b_begin("");
        b_u32("#address-cells", 2u);
        child_at = b_begin("m");
        b_s("device_type", "memory");
        b_prop("reg", "\0\0\0\1", 4u); /* would be BAD_CELLS at the child too */
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + child_at);
    }
    /* childless root: err_off = the root's END_NODE. Walk-time BAD_CELLS also
     * precedes post-walk NO_MEMORY. */
    {
        uint32_t end_at;
        /* (no properties at all -> also an empty strings block) */
        b_reset();
        b_begin("");
        end_at = b_end();
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + end_at);
        b_reset();
        b_begin("");
        b_u32("#address-cells", 2u);
        b_nop();
        end_at = b_end();
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + end_at);
        b_reset();
        b_begin("");
        b_u32("#size-cells", 1u);
        end_at = b_end();
        b_nop();
        b_endtok();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + end_at);
    }
    /* positive control: cells present, childless root -> walk OK, NO_MEMORY */
    b_reset();
    root_open(2u, 2u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_NO_MEMORY);
    /* bad length / value */
    {
        static const uint32_t vals[] = { 0u, 3u, 4u, 0xffffffffu };
        for (uint32_t w = 0u; w < 2u; w++) {
            for (uint32_t k = 0u; k < sizeof vals / sizeof vals[0]; k++) {
                b_reset();
                b_begin("");
                b_u32("#address-cells", w == 0u ? vals[k] : 2u);
                b_u32("#size-cells", w == 1u ? vals[k] : 2u);
                mem("m", 2u, 2u, RAM_B, RAM_S);
                root_close();
                b_finish();
                EXPECT(JE_FDT_BAD_CELLS);
            }
            static const uint8_t eight[8] = { 0, 0, 0, 0, 0, 0, 0, 2 };
            b_reset();
            b_begin("");
            if (w == 0u) {
                b_prop("#address-cells", eight, 8u);
                b_u32("#size-cells", 2u);
            } else {
                b_u32("#address-cells", 2u);
                b_prop("#size-cells", eight, 8u);
            }
            mem("m", 2u, 2u, RAM_B, RAM_S);
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_CELLS);
            b_reset();
            b_begin("");
            if (w == 0u) {
                b_empty("#address-cells");
                b_u32("#size-cells", 2u);
            } else {
                b_u32("#address-cells", 2u);
                b_empty("#size-cells");
            }
            mem("m", 2u, 2u, RAM_B, RAM_S);
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_CELLS);
        }
    }
    /* memory reg: zero length, not a multiple of (ac+sc)*4 */
    {
        static const uint32_t lens[] = { 0u, 4u, 8u, 12u, 20u, 24u };
        static const uint8_t z[24];
        for (uint32_t k = 0u; k < sizeof lens / sizeof lens[0]; k++) {
            b_reset();
            root_open(2u, 2u);
            b_begin("m");
            b_s("device_type", "memory");
            b_prop("reg", z, lens[k]);
            b_end();
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_CELLS);
        }
        b_reset();
        root_open(1u, 1u);
        b_begin("m");
        b_s("device_type", "memory");
        b_prop("reg", z, 12u);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_BAD_CELLS);
        /* reserved-memory child reg: zero length / not a multiple */
        for (uint32_t k = 0u; k < 2u; k++) {
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            rm_open(2u, 2u);
            b_begin("r");
            b_prop("reg", z, k == 0u ? 0u : 12u);
            b_end();
            b_end();
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_CELLS);
        }
    }
    /* /reserved-memory: cells absent, != root's, ranges absent / non-empty.
     * Decided at its first child BEGIN_NODE, or at its END_NODE when childless
     * (contract 5a3c14b), err_off = that token. w == 0: childless; w == 1: one
     * valid child; w == 2: the first child has a MALFORMED reg (len 12), which
     * must NOT be reported first (the cells decision precedes any child use). */
    for (uint32_t w = 0u; w < 3u; w++) {
        for (uint32_t k = 0u; k < 7u; k++) {
            static const uint8_t rng[12] = { 0 };
            static const uint8_t z12[12] = { 0 };
            uint32_t at = 0u;
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            b_begin("reserved-memory");
            if (k != 0u)
                b_u32("#address-cells", k == 2u ? 1u : 2u);
            if (k != 1u)
                b_u32("#size-cells", k == 3u ? 1u : 2u);
            if (k == 5u)
                b_prop("ranges", rng, 12u);
            else if (k == 6u)
                b_prop("ranges", rng, 4u);
            else if (k != 4u)
                b_empty("ranges");
            if (w == 0u) {
                at = b_end();
            } else {
                at = b_begin("r");
                if (w == 1u)
                    b_reg1(2u, 2u, RAM_B, 0x1000u);
                else
                    b_prop("reg", z12, 12u);
                b_end();
                b_end();
            }
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
        }
    }
    /* /reserved-memory 1/1 under a 2/2 root (cells must equal root's), with a
     * child whose reg is valid FOR 1/1 cells */
    {
        uint32_t at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(1u, 1u);
        at = b_begin("r");
        b_reg1(1u, 1u, RAM_B, 0x1000u);
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(1u, 1u);
        at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
        /* NOPs before the first child do not move the decision point; a bad
         * token AFTER the child BEGIN_NODE (inside the child) is not reached */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("reserved-memory");
        b_u32("#address-cells", 2u);
        b_u32("#size-cells", 2u);
        b_nop();
        at = b_begin("r");
        st_word(0x66u);
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
    }
    /* round-6: a cells property of length 8 whose FIRST word is a valid value
     * (equal to root's) is still BAD_CELLS (len must be exactly 4). Kills a
     * `len == 4` -> `len >= 4` mutant that would read only the first word.
     * The second word is 0 or 2, so neither "first word" nor "last word"
     * readings of an over-long value can pass. */
    {
        static const uint8_t two_lo[8] = { 0, 0, 0, 2, 0, 0, 0, 0 };
        static const uint8_t two_hi[8] = { 0, 0, 0, 2, 0, 0, 0, 2 };
        for (uint32_t v = 0u; v < 2u; v++) {
            const uint8_t *eight = v == 0u ? two_lo : two_hi;
            /* root: rejected at the offending PROP token itself */
            for (uint32_t w = 0u; w < 2u; w++) {
                uint32_t at;
                b_reset();
                b_begin("");
                if (w == 0u) {
                    at = b_prop("#address-cells", eight, 8u);
                    b_u32("#size-cells", 2u);
                } else {
                    b_u32("#address-cells", 2u);
                    at = b_prop("#size-cells", eight, 8u);
                }
                mem("m", 2u, 2u, RAM_B, RAM_S);
                root_close();
                b_finish();
                EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
            }
            /* /reserved-memory under a 2/2 root: w selects which cell property
             * is over-long; c == 0: childless (err_off = its END_NODE),
             * c == 1: one child valid for 2/2 (err_off = child BEGIN_NODE) */
            for (uint32_t w = 0u; w < 2u; w++) {
                for (uint32_t c = 0u; c < 2u; c++) {
                    uint32_t at;
                    b_reset();
                    root_open(2u, 2u);
                    mem("m", 2u, 2u, RAM_B, RAM_S);
                    b_begin("reserved-memory");
                    if (w == 0u) {
                        b_prop("#address-cells", eight, 8u);
                        b_u32("#size-cells", 2u);
                    } else {
                        b_u32("#address-cells", 2u);
                        b_prop("#size-cells", eight, 8u);
                    }
                    b_empty("ranges");
                    if (c == 0u) {
                        at = b_end();
                    } else {
                        at = b_begin("r");
                        b_reg1(2u, 2u, RAM_B, 0x1000u);
                        b_end();
                        b_end();
                    }
                    root_close();
                    b_finish();
                    EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
                }
            }
        }
        /* positive control: the same trees with len-4 <2> cells parse OK */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        rm_child("r", 2u, 2u, RAM_B, 0x1000u, false);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
    }
}

/* ------------------------------------------------------------------------- */
/* 4b. round-4 discriminating cases (contract 3a6e1f1 + 44187ac)              */

/* A PROP whose nameoff is patched (after b_finish) to lie outside the strings
 * block -> BAD_STRING. Returns the PROP's struct-relative offset; the caller
 * must call bad_nameoff_patch() on it after b_finish(). */
static uint32_t bad_nameoff_prop(void)
{
    return b_prop_off(0u, NULL, 0u);
}

static void bad_nameoff_patch(uint32_t at)
{
    st_patch(at + 8u, 0xfffffff0u);
}

/* Codex test review (2): DUPLICATE is decided by NAME before the repeat's value
 * is examined. Each repeat carries a value that would otherwise be its own
 * error (BAD_CELLS, BAD_STRING, CONFLICT), so a value-first parser fails. */
static void test_dup_before_value(void)
{
    uint32_t at;

    /* root #address-cells / #size-cells repeated with len 0, value 3, len 8 */
    for (uint32_t w = 0u; w < 2u; w++) {
        for (uint32_t k = 0u; k < 3u; k++) {
            static const uint8_t eight[8] = { 0, 0, 0, 0, 0, 0, 0, 2 };
            const char *name = w == 0u ? "#address-cells" : "#size-cells";
            b_reset();
            root_open(2u, 2u);
            if (k == 0u)
                at = b_empty(name);
            else if (k == 1u)
                at = b_u32(name, 3u);
            else
                at = b_prop(name, eight, 8u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
        }
    }
    /* memory status "okay" repeated with an empty / non-NUL-terminated value,
     * with status before and after device_type */
    {
        static const struct { const char *v; uint32_t len; } bad[] = {
            { "", 0u }, { "okay", 4u }, { "disabled", 8u },
        };
        for (uint32_t k = 0u; k < sizeof bad / sizeof bad[0]; k++) {
            for (uint32_t w = 0u; w < 2u; w++) {
                b_reset();
                root_open(2u, 2u);
                b_begin("m");
                if (w == 1u)
                    b_s("device_type", "memory");
                b_s("status", "okay");
                at = b_prop("status", bad[k].v, bad[k].len);
                if (w == 0u)
                    b_s("device_type", "memory");
                b_reg1(2u, 2u, RAM_B, RAM_S);
                b_end();
                root_close();
                b_finish();
                EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
            }
        }
        /* first status itself malformed (empty), then a repeat: the memory
         * node's status value is decided at END_NODE, the repeat precedes it */
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_empty("status");
        at = b_s("status", "okay");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    }
    /* rsv child status repeated with an empty value (status is checked at the
     * PROP there, but DUPLICATE comes first) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    b_begin("r");
    b_s("status", "okay");
    at = b_empty("status");
    b_reg1(2u, 2u, RAM_B, 0x1000u);
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* device_type repeated with an empty value on a depth-2 node */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    at = b_empty("device_type");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* /chosen and /reserved-memory: device_type "foo" then "memory" ->
     * DUPLICATE at the repeat, NOT CONFLICT */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("chosen");
    b_s("device_type", "foo");
    at = b_s("device_type", "memory");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    b_s("device_type", "foo");
    at = b_s("device_type", "memory");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    /* /reserved-memory #address-cells=<3> then a repeat: the bad VALUE is only
     * recorded at the PROP (decided at the first child / END_NODE), so the
     * repeat is DUPLICATE (Claude parser review probe 2a); also len 0 */
    for (uint32_t k = 0u; k < 2u; k++) {
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("reserved-memory");
        if (k == 0u)
            b_u32("#address-cells", 3u);
        else
            b_empty("#address-cells");
        b_u32("#size-cells", 2u);
        b_empty("ranges");
        at = b_u32("#address-cells", 2u);
        rm_child("r", 2u, 2u, RAM_B, 0x1000u, false);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    }
}

/* Claude parser review probes 1a-1g: a malformed status / device_type VALUE is
 * validated only where it is consumed (device_type on depth-2 nodes; status on
 * memory nodes and /reserved-memory children). Everywhere else -> OK. */
static void test_unconsumed_strings(void)
{
    static const uint8_t nonul[4] = { 'o', 'k', 'a', 'y' };

    for (uint32_t k = 0u; k < 2u; k++) {
        const uint8_t *v = k == 0u ? NULL : nonul;
        uint32_t vl = k == 0u ? 0u : 4u;
        /* 1b root status; 1g root device_type */
        b_reset();
        root_open(2u, 2u);
        b_prop("status", v, vl);
        b_prop("device_type", v, vl);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.ram_count == 1u && has_ram(&C, RAM_B, RAM_S));
        /* 1c /chosen status (still yields the initrd) */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_prop("status", v, vl);
        b_be("linux,initrd-start", 0x44000000u, 4u);
        b_be("linux,initrd-end", 0x44100000u, 4u);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.has_initrd && C.initrd.base == 0x44000000u);
        /* 1d /reserved-memory status (still yields its reservation) */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_prop("status", v, vl);
        rm_child("r", 2u, 2u, RAM_B + 0x1000u, 0x1000u, false);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.rsv_count == 1u && C.rsv[0].base == RAM_B + 0x1000u);
        /* 1e non-memory depth-2 nodes: no device_type, and device_type "foo"
         * (on either side of the status) */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("uart@9000000");
        b_prop("status", v, vl);
        b_u32("reg", 0x09000000u);
        b_end();
        b_begin("dev1");
        b_prop("status", v, vl);
        b_s("device_type", "foo");
        b_end();
        b_begin("dev2");
        b_s("device_type", "foo");
        b_prop("status", v, vl);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.ram_count == 1u);
        /* 1a/1f depth-3 untracked nodes (under soc and under a memory node);
         * depth-3 rsv child device_type (tracked, but device_type is consumed
         * only at depth 2); depth-4 node under a rsv child */
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_begin("sub");
        b_prop("status", v, vl);
        b_prop("device_type", v, vl);
        b_end();
        b_end();
        b_begin("soc");
        b_begin("dev@0");
        b_prop("status", v, vl);
        b_prop("device_type", v, vl);
        b_end();
        b_end();
        rm_open(2u, 2u);
        b_begin("r");
        b_prop("device_type", v, vl);
        b_reg1(2u, 2u, RAM_B + 0x1000u, 0x1000u);
        b_begin("g");
        b_prop("status", v, vl);
        b_prop("device_type", v, vl);
        b_end();
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.ram_count == 1u && C.rsv_count == 1u);
    }
}

/* Memory-node status VALUE timing (contract 44187ac): checked at the node's
 * END_NODE (first of the END_NODE checks), err_off = the status PROP. A
 * violation before that END_NODE wins; END_NODE-time semantics lose. */
static void test_memory_status_timing(void)
{
    uint32_t at, st_at;

    for (uint32_t w = 0u; w < 2u; w++) { /* w==0: status before device_type */
        /* control: BAD_STRING at the status PROP */
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        if (w == 1u)
            b_s("device_type", "memory");
        st_at = b_empty("status");
        if (w == 0u)
            b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_STRING, I.off_st + st_at);
        /* a repeated reg AFTER the empty status (before END_NODE) -> DUPLICATE
         * at the second reg */
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        if (w == 1u)
            b_s("device_type", "memory");
        b_empty("status");
        if (w == 0u)
            b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        at = b_reg1(2u, 2u, RAM_B, RAM_S);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
        /* a depth-3 child of the memory node with a bad nameoff -> BAD_STRING
         * at the child's PROP, not at the status PROP */
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        if (w == 1u)
            b_s("device_type", "memory");
        st_at = b_empty("status");
        if (w == 0u)
            b_s("device_type", "memory");
        b_reg1(2u, 2u, RAM_B, RAM_S);
        b_begin("sub");
        at = bad_nameoff_prop();
        b_end();
        b_end();
        root_close();
        b_finish();
        bad_nameoff_patch(at);
        EXPECT_BAD_NAMEOFF_AT(at);
        CHECK(EO != I.off_st + st_at);
        /* status is the FIRST END_NODE check: it beats reg absent (BAD_RANGE)
         * and a malformed reg (BAD_CELLS) */
        for (uint32_t k = 0u; k < 2u; k++) {
            static const uint8_t z12[12];
            b_reset();
            root_open(2u, 2u);
            mem("ok", 2u, 2u, 0x80000000u, 0x1000u);
            b_begin("m");
            if (w == 1u)
                b_s("device_type", "memory");
            st_at = b_prop("status", "okay", 4u);
            if (w == 0u)
                b_s("device_type", "memory");
            if (k == 1u)
                b_prop("reg", z12, 12u);
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_BAD_STRING, I.off_st + st_at);
        }
    }
}

/* /reserved-memory cell VALUES are recorded at the PROP and decided with
 * absence at the first child BEGIN_NODE / END_NODE (contract 44187ac; Claude
 * parser review probes 2b, 2c). (2a is in test_dup_before_value.) */
static void test_rsv_cells_deferral(void)
{
    static const uint8_t eight[8] = { 0, 0, 0, 0, 0, 0, 0, 2 };
    uint32_t at;

    /* bad value / len 0 / len 8 for either cell, with a child -> at the child
     * BEGIN_NODE; childless -> at the END_NODE */
    for (uint32_t w = 0u; w < 2u; w++) {
        for (uint32_t k = 0u; k < 4u; k++) {
            for (uint32_t c = 0u; c < 2u; c++) {
                const char *bad = w == 0u ? "#address-cells" : "#size-cells";
                const char *good = w == 0u ? "#size-cells" : "#address-cells";
                b_reset();
                root_open(2u, 2u);
                mem("m", 2u, 2u, RAM_B, RAM_S);
                b_begin("reserved-memory");
                if (k == 0u)
                    b_u32(bad, 3u);
                else if (k == 1u)
                    b_u32(bad, 0u);
                else if (k == 2u)
                    b_empty(bad);
                else
                    b_prop(bad, eight, 8u);
                b_u32(good, 2u);
                b_empty("ranges");
                if (c == 0u) {
                    at = b_begin("r");
                    b_reg1(2u, 2u, RAM_B, 0x1000u);
                    b_end();
                    b_end();
                } else {
                    at = b_end();
                }
                root_close();
                b_finish();
                EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
            }
        }
    }
    /* a non-cell defect between the bad cell PROP and the decision point wins:
     * repeated ranges -> DUPLICATE at the repeat; bad nameoff -> BAD_STRING */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("reserved-memory");
    b_u32("#address-cells", 3u);
    b_u32("#size-cells", 2u);
    b_empty("ranges");
    at = b_empty("ranges");
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_begin("reserved-memory");
    b_prop("#size-cells", eight, 8u);
    b_u32("#address-cells", 2u);
    b_empty("ranges");
    at = bad_nameoff_prop();
    b_end();
    root_close();
    b_finish();
    bad_nameoff_patch(at);
    EXPECT_BAD_NAMEOFF_AT(at);
}

/* /reserved-memory child `reg` is decoded at the child's END_NODE, err_off the
 * reg PROP (contract 44187ac; Claude parser review probe 3); its status VALUE
 * is checked at the PROP. Plus Codex test review (3): a DISABLED memory node /
 * rsv child's reg is not decoded at all. */
static void test_reg_timing(void)
{
    static const uint8_t z12[12];
    uint32_t at, reg_at;

    /* rsv child zero-size reg, then a grandchild with a bad nameoff ->
     * BAD_STRING at the grandchild; same with a 12-byte reg (BAD_CELLS lost) */
    for (uint32_t k = 0u; k < 2u; k++) {
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        if (k == 0u)
            reg_at = b_reg1(2u, 2u, RAM_B, 0u);
        else
            reg_at = b_prop("reg", z12, 12u);
        b_begin("g");
        at = bad_nameoff_prop();
        b_end();
        b_end();
        b_end();
        root_close();
        b_finish();
        bad_nameoff_patch(at);
        EXPECT_BAD_NAMEOFF_AT(at);
        CHECK(EO != I.off_st + reg_at);
        /* ... a later repeated no-map / malformed status in the same child */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        if (k == 0u)
            b_reg1(2u, 2u, RAM_B, 0u);
        else
            b_prop("reg", z12, 12u);
        b_empty("no-map");
        at = b_empty("no-map");
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        if (k == 0u)
            b_reg1(2u, 2u, RAM_B, 0u);
        else
            b_prop("reg", z12, 12u);
        at = b_empty("status");
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_STRING, I.off_st + at);
    }

    /* Codex test review (4): err_off = the reg PROP for walk-time reg errors.
     * memory node BAD_CELLS (zero length, not a multiple; reg before and after
     * device_type) and BAD_RANGE (zero size, overflow, 2nd pair zero-size) */
    {
        static const uint32_t lens[] = { 0u, 4u, 12u, 20u };
        for (uint32_t k = 0u; k < sizeof lens / sizeof lens[0]; k++) {
            for (uint32_t w = 0u; w < 2u; w++) {
                static const uint8_t z[20];
                b_reset();
                root_open(2u, 2u);
                b_begin("m");
                if (w == 0u)
                    b_s("device_type", "memory");
                at = b_prop("reg", z, lens[k]);
                if (w == 1u)
                    b_s("device_type", "memory");
                b_s("compatible", "x"); /* the reg PROP is not the last token */
                b_end();
                root_close();
                b_finish();
                EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
            }
        }
        b_reset();
        root_open(1u, 1u);
        b_begin("m");
        b_s("device_type", "memory");
        at = b_prop("reg", z12, 12u);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_CELLS, I.off_st + at);
    }
    {
        static const uint64_t cases[][4] = {
            { RAM_B, 0u, 0u, 0u },
            { 0xfffffffffffff000ull, 0x1000u, 0u, 0u },
            { RAM_B, RAM_S, 0x80000000u, 0u },
        };
        for (uint32_t k = 0u; k < 3u; k++) {
            b_reset();
            root_open(2u, 2u);
            b_begin("m");
            at = b_reg(2u, 2u, k == 2u ? 2u : 1u, cases[k]);
            b_s("device_type", "memory");
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + at);
        }
        b_reset();
        root_open(1u, 1u);
        b_begin("m");
        b_s("device_type", "memory");
        at = b_reg1(1u, 1u, RAM_B, 0u);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + at);
    }
    /* rsv child BAD_CELLS (zero length, 12 bytes; reg before another prop) and
     * BAD_RANGE (zero size, overflow, 2nd pair zero-size) at the reg PROP */
    for (uint32_t k = 0u; k < 5u; k++) {
        static const uint64_t p2[4] = { RAM_B, 0x1000u, RAM_B + 0x2000u, 0u };
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        if (k == 0u)
            at = b_prop("reg", z12, 0u);
        else if (k == 1u)
            at = b_prop("reg", z12, 12u);
        else if (k == 2u)
            at = b_reg1(2u, 2u, RAM_B, 0u);
        else if (k == 3u)
            at = b_reg1(2u, 2u, 0xfffffffffffff000ull, 0x1000u);
        else
            at = b_reg(2u, 2u, 2u, p2);
        b_empty("no-map");
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(k < 2u ? JE_FDT_BAD_CELLS : JE_FDT_BAD_RANGE, I.off_st + at);
    }

    /* Codex test review (3): disabled memory node and disabled rsv child with
     * a 12-byte reg under 2/2 cells -> OK (status on either side of reg) */
    for (uint32_t w = 0u; w < 2u; w++) {
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("m");
        b_s("device_type", "memory");
        if (w == 0u)
            b_s("status", "disabled");
        b_prop("reg", z12, 12u);
        if (w == 1u)
            b_s("status", "disabled");
        b_end();
        rm_open(2u, 2u);
        b_begin("r");
        if (w == 0u)
            b_s("status", "disabled");
        b_prop("reg", z12, 12u);
        if (w == 1u)
            b_s("status", "disabled");
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.ram_count == 1u && has_ram(&C, RAM_B, RAM_S));
        CHECK(C.rsv_count == 0u && C.rsv_ignored == 0u && C.rsv_dynamic == 0u);
        CHECK(C.node_count == 5u);
    }
}

/* Round-4 err_off pins and memory END_NODE order (contract a4712a8):
 *  - memory node without `reg` -> BAD_RANGE at ITS END_NODE;
 *  - exactly one initrd property -> BAD_RANGE at /chosen's END_NODE;
 *  - memory END_NODE order: status value (BAD_STRING) -> disabled skip ->
 *    `reg` absent -> `reg` decode;
 *  - device_type on a depth-3 node: duplicate-tracked if tracked, value not
 *    inspected.
 * Each case is built so the plausible wrong answers (the child's END_NODE, the
 * PROP, a post-walk status, a later violation) are distinguishable. */
static void test_end_node_pins(void)
{
    static const uint8_t z12[12];
    uint32_t at, end_at, st_at;

    /* --- memory without reg: END_NODE of the memory node itself --- */
    for (uint32_t k = 0u; k < 4u; k++) {
        /* k0: plain; k1: valid status "okay" first; k2: a child subtree
         * (err_off is the memory node's END_NODE, not the child's); k3: "ok"
         * after device_type, then an unconsumed property */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("m");
        if (k == 1u)
            b_s("status", "okay");
        b_s("device_type", "memory");
        if (k == 3u) {
            b_s("status", "ok");
            b_s("compatible", "x");
        }
        if (k == 2u) {
            b_begin("sub");
            b_s("compatible", "x");
            b_end();
        }
        end_at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
    }
    /* ... it is a WALK-time error: it beats a later sibling's bad nameoff and
     * post-walk NO_MEMORY (it is the only memory node) */
    b_reset();
    root_open(2u, 2u);
    mem("ok", 2u, 2u, RAM_B, RAM_S);
    b_begin("m");
    b_s("device_type", "memory");
    end_at = b_end();
    b_begin("leaf");
    at = bad_nameoff_prop();
    b_end();
    root_close();
    b_finish();
    bad_nameoff_patch(at);
    EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    end_at = b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
    /* ... but an earlier violation inside its subtree wins */
    b_reset();
    root_open(2u, 2u);
    mem("ok", 2u, 2u, RAM_B, RAM_S);
    b_begin("m");
    b_s("device_type", "memory");
    b_begin("sub");
    at = bad_nameoff_prop();
    b_end();
    b_end();
    root_close();
    b_finish();
    bad_nameoff_patch(at);
    EXPECT_BAD_NAMEOFF_AT(at);

    /* --- exactly one initrd property: /chosen's END_NODE --- */
    for (uint32_t w = 0u; w < 2u; w++) {
        const char *one = w == 0u ? "linux,initrd-start" : "linux,initrd-end";
        /* trailing property and a child subtree: err_off is /chosen's own
         * END_NODE, not the PROP and not the child's END_NODE */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_be(one, 0x44000000u, w == 0u ? 4u : 8u);
        b_s("bootargs", "x");
        b_begin("sub");
        b_s("compatible", "x");
        b_end();
        end_at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
        /* walk-time: beats a later sibling's bad nameoff */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_be(one, 0x44000000u, 4u);
        end_at = b_end();
        b_begin("leaf");
        at = bad_nameoff_prop();
        b_end();
        root_close();
        b_finish();
        bad_nameoff_patch(at);
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
        /* walk-time: beats post-walk NO_MEMORY (no memory node at all, and a
         * memory node AFTER /chosen whose RAM would not contain the value) */
        b_reset();
        root_open(2u, 2u);
        b_begin("chosen");
        b_be(one, 0x44000000u, 4u);
        end_at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
        b_reset();
        root_open(2u, 2u);
        b_begin("chosen");
        b_be(one, 0x10000000u, 4u);
        end_at = b_end();
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
        /* an earlier violation inside /chosen's subtree wins */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_be(one, 0x44000000u, 4u);
        b_begin("sub");
        at = bad_nameoff_prop();
        b_end();
        b_end();
        root_close();
        b_finish();
        bad_nameoff_patch(at);
        EXPECT_BAD_NAMEOFF_AT(at);
        /* ... and a repeat of the one property in /chosen (DUPLICATE at it) */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_be(one, 0x44000000u, 4u);
        at = b_be(one, 0x44000000u, 4u);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
    }

    /* --- memory END_NODE order --- */
    for (uint32_t w = 0u; w < 2u; w++) { /* w==1: device_type first */
        /* (1) status value first: malformed status beats a zero-size reg, an
         * overflowing reg and a 12-byte reg (would be BAD_RANGE / BAD_CELLS at
         * the reg PROP), and a missing reg (BAD_RANGE at END_NODE) */
        for (uint32_t k = 0u; k < 4u; k++) {
            b_reset();
            root_open(2u, 2u);
            mem("ok", 2u, 2u, RAM_B, RAM_S);
            b_begin("m");
            if (k == 0u)
                b_reg1(2u, 2u, 0x80000000u, 0u);
            else if (k == 1u)
                b_reg1(2u, 2u, 0xfffffffffffff000ull, 0x1000u);
            else if (k == 2u)
                b_prop("reg", z12, 12u);
            if (w == 1u)
                b_s("device_type", "memory");
            st_at = b_empty("status");
            if (w == 0u)
                b_s("device_type", "memory");
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_BAD_STRING, I.off_st + st_at);
        }
        /* (2) disabled -> skip before reg absent / reg decode: the only memory
         * node, disabled, with no reg / zero-size / 12-byte reg -> post-walk
         * NO_MEMORY (not BAD_RANGE/BAD_CELLS); with another memory node -> OK */
        for (uint32_t k = 0u; k < 3u; k++) {
            for (uint32_t other = 0u; other < 2u; other++) {
                b_reset();
                root_open(2u, 2u);
                if (other == 1u)
                    mem("ok", 2u, 2u, RAM_B, RAM_S);
                b_begin("m");
                if (w == 1u)
                    b_s("device_type", "memory");
                if (k == 1u)
                    b_reg1(2u, 2u, 0x80000000u, 0u);
                else if (k == 2u)
                    b_prop("reg", z12, 12u);
                b_s("status", "disabled");
                if (w == 0u)
                    b_s("device_type", "memory");
                b_end();
                root_close();
                b_finish();
                if (other == 0u) {
                    EXPECT(JE_FDT_NO_MEMORY);
                } else {
                    EXPECT(JE_FDT_OK);
                    CHECK(C.ram_count == 1u && has_ram(&C, RAM_B, RAM_S));
                }
            }
        }
        /* (3) reg absent after a VALID okay status -> END_NODE, not status */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("m");
        if (w == 1u)
            b_s("device_type", "memory");
        st_at = b_s("status", "okay");
        if (w == 0u)
            b_s("device_type", "memory");
        end_at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
        CHECK(EO != I.off_st + st_at);
        /* (4) reg decode is at END_NODE (err_off still the reg PROP): an
         * earlier violation inside the node's subtree, or a later repeat in
         * the node, beats a zero-size / 12-byte reg */
        for (uint32_t k = 0u; k < 2u; k++) {
            uint32_t reg_at;
            b_reset();
            root_open(2u, 2u);
            mem("ok", 2u, 2u, RAM_B, RAM_S);
            b_begin("m");
            if (w == 1u)
                b_s("device_type", "memory");
            reg_at = k == 0u ? b_reg1(2u, 2u, 0x80000000u, 0u) : b_prop("reg", z12, 12u);
            if (w == 0u)
                b_s("device_type", "memory");
            b_begin("sub");
            at = bad_nameoff_prop();
            b_end();
            b_end();
            root_close();
            b_finish();
            bad_nameoff_patch(at);
            EXPECT_BAD_NAMEOFF_AT(at);
            CHECK(EO != I.off_st + reg_at);
            b_reset();
            root_open(2u, 2u);
            mem("ok", 2u, 2u, RAM_B, RAM_S);
            b_begin("m");
            b_s("device_type", "memory");
            if (k == 0u)
                b_reg1(2u, 2u, 0x80000000u, 0u);
            else
                b_prop("reg", z12, 12u);
            b_s("status", "okay");
            at = b_s(w == 0u ? "device_type" : "status", w == 0u ? "memory" : "okay");
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
            /* control: the same reg alone fails at its PROP */
            b_reset();
            root_open(2u, 2u);
            mem("ok", 2u, 2u, RAM_B, RAM_S);
            b_begin("m");
            b_s("device_type", "memory");
            reg_at = k == 0u ? b_reg1(2u, 2u, 0x80000000u, 0u) : b_prop("reg", z12, 12u);
            b_s("status", "okay");
            b_begin("sub");
            b_end();
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(k == 0u ? JE_FDT_BAD_RANGE : JE_FDT_BAD_CELLS, I.off_st + reg_at);
        }
    }

    /* --- depth-3 device_type: not inspected; duplicate-tracked if tracked --- */
    {
        static const uint8_t nonul[6] = { 'm', 'e', 'm', 'o', 'r', 'y' };
        for (uint32_t k = 0u; k < 3u; k++) {
            /* empty / non-NUL / "memory" device_type on a rsv child -> OK, the
             * child stays a reservation (not memory, not CONFLICT) */
            b_reset();
            root_open(2u, 2u);
            mem("ok", 2u, 2u, RAM_B, RAM_S);
            rm_open(2u, 2u);
            b_begin("r");
            if (k == 0u)
                b_empty("device_type");
            else if (k == 1u)
                b_prop("device_type", nonul, 6u);
            else
                b_s("device_type", "memory");
            b_reg1(2u, 2u, RAM_B + 0x1000u, 0x1000u);
            b_end();
            b_end();
            root_close();
            b_finish();
            EXPECT(JE_FDT_OK);
            CHECK(C.ram_count == 1u && C.rsv_count == 1u &&
                  C.rsv[0].base == RAM_B + 0x1000u && C.rsv[0].size == 0x1000u);
        }
        /* tracked rsv child: a repeated device_type (empty values) -> DUPLICATE */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        b_empty("device_type");
        at = b_empty("device_type");
        b_reg1(2u, 2u, RAM_B + 0x1000u, 0x1000u);
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_DUPLICATE, I.off_st + at);
        /* untracked depth-3 node (under a non-rsv node): repeat is OK */
        b_reset();
        root_open(2u, 2u);
        mem("ok", 2u, 2u, RAM_B, RAM_S);
        b_begin("soc");
        b_begin("dev");
        b_empty("device_type");
        b_empty("device_type");
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
    }
}

static void test_no_memory(void)
{
    /* no memory node at all */
    b_reset();
    root_open(2u, 2u);
    b_begin("chosen");
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_NO_MEMORY);
    /* only disabled memory */
    b_reset();
    root_open(2u, 2u);
    b_begin("m");
    b_s("device_type", "memory");
    b_s("status", "disabled");
    b_reg1(2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_NO_MEMORY);
    /* memory only at depth 3 */
    b_reset();
    root_open(2u, 2u);
    b_begin("soc");
    b_u32("#address-cells", 2u);
    b_u32("#size-cells", 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_NO_MEMORY);
    /* NO_MEMORY precedes reservation/initrd checks */
    b_reset();
    b_rsv(RAM_B, 0x1000u);
    root_open(2u, 2u);
    chosen_initrd(0x44000000u, 4u, 0x44100000u, 4u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_NO_MEMORY);
}

/* /reserved-memory `reg` CANDIDATES are bounded during the walk (contract update
 * bdf446a): the (JE_FDT_MAX_RSV+1)-th candidate RANGE -> RANGE_LIMIT at once,
 * err_off = that `reg` PROP, before any post-walk RAM classification. Literal
 * reading: a candidate is one {base,size} range of an enabled depth-3 child's
 * `reg`; memreserve entries, status-disabled children and size-only (dynamic)
 * children are not candidates. */
static void test_rsv_candidates(void)
{
    uint32_t reg_at = 0u;

    /* 17 children, all inside RAM */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++)
        reg_at = rm_child("r", 2u, 2u, 0x48000000u + i * 0x1000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
    /* 17 candidates ALL outside RAM: candidate bound, not kept bound */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++)
        reg_at = rm_child("r", 2u, 2u, 0x80000000u + i * 0x1000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
    /* ... and 17 PARTIAL candidates: walk-time RANGE_LIMIT beats post-walk
     * BAD_RANGE */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++)
        reg_at = rm_child("r", 2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
    /* the 17th candidate precedes a later struct defect (first violation in
     * file order) */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++)
        reg_at = rm_child("r", 2u, 2u, 0x80000000u + i * 0x1000u, 0x1000u, false);
    b_end();
    st_word(0x77u);
    root_close();
    b_finish();
    EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
    /* exactly 16 candidates outside RAM -> OK, all ignored */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        rm_child("r", 2u, 2u, 0x80000000u + i * 0x1000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 0u && C.rsv_ignored == JE_FDT_MAX_RSV);
    /* candidates are counted per RANGE: one child with 17 pairs */
    {
        uint64_t p[2u * (JE_FDT_MAX_RSV + 1u)];
        for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++) {
            p[2u * i] = 0x80000000u + (uint64_t)i * 0x1000u;
            p[2u * i + 1u] = 0x1000u;
        }
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        reg_at = b_reg(2u, 2u, JE_FDT_MAX_RSV + 1u, p);
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
        /* 16 pairs in one child + 1 more child: the 17th is the second reg */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        b_reg(2u, 2u, JE_FDT_MAX_RSV, p);
        b_end();
        reg_at = rm_child("s", 2u, 2u, 0x90000000u, 0x1000u, false);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
        /* 16 pairs in one child -> OK */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        b_begin("r");
        b_reg(2u, 2u, JE_FDT_MAX_RSV, p);
        b_end();
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.rsv_count == 0u && C.rsv_ignored == JE_FDT_MAX_RSV);
        /* Candidate ORDER within one reg (contract 5a3c14b): validity first,
         * then the count; only VALID ranges count. An invalid 17th range ->
         * BAD_RANGE (not RANGE_LIMIT); an invalid range among the first 16 ->
         * BAD_RANGE (also with a valid 17th after it). err_off is pinned to the
         * reg PROP token (the token that supplied the range; ambiguity noted in
         * WORKER_NOTES.md). Invalid = zero size, or base+size overflow. */
        {
            static const uint32_t bad_idx[] = { JE_FDT_MAX_RSV, JE_FDT_MAX_RSV, 3u, 0u };
            static const uint64_t bad_rng[][2] = {
                { 0x90000000u, 0u },
                { 0xfffffffffffff000ull, 0x1000u },
                { 0x90000000u, 0u },
                { 0xffffffffffffffffull, 1u },
            };
            for (uint32_t k = 0u; k < 4u; k++) {
                uint64_t q[2u * (JE_FDT_MAX_RSV + 1u)];
                for (uint32_t i = 0u; i < 2u * (JE_FDT_MAX_RSV + 1u); i++)
                    q[i] = p[i];
                q[2u * bad_idx[k]] = bad_rng[k][0];
                q[2u * bad_idx[k] + 1u] = bad_rng[k][1];
                b_reset();
                root_open(2u, 2u);
                mem("m", 2u, 2u, RAM_B, RAM_S);
                rm_open(2u, 2u);
                b_begin("r");
                reg_at = b_reg(2u, 2u, JE_FDT_MAX_RSV + 1u, q);
                b_end();
                b_end();
                root_close();
                b_finish();
                EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + reg_at);
            }
            /* 18 ranges in one reg: invalid 4th, then 17 VALID ones. In-order
             * checking reports the invalid range before the 17th valid one
             * could reach the count -> BAD_RANGE, not RANGE_LIMIT. */
            {
                uint64_t q[2u * (JE_FDT_MAX_RSV + 2u)];
                for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 2u; i++) {
                    q[2u * i] = 0x80000000u + (uint64_t)i * 0x1000u;
                    q[2u * i + 1u] = (i == 3u) ? 0u : 0x1000u;
                }
                b_reset();
                root_open(2u, 2u);
                mem("m", 2u, 2u, RAM_B, RAM_S);
                rm_open(2u, 2u);
                b_begin("r");
                reg_at = b_reg(2u, 2u, JE_FDT_MAX_RSV + 2u, q);
                b_end();
                b_end();
                root_close();
                b_finish();
                EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + reg_at);
            }
            /* an invalid range in an EARLIER child among the first 16 */
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            rm_open(2u, 2u);
            for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++) {
                uint32_t r = rm_child("r", 2u, 2u, 0x80000000u + i * 0x1000u,
                                      i == 7u ? 0u : 0x1000u, false);
                if (i == 7u)
                    reg_at = r;
            }
            b_end();
            root_close();
            b_finish();
            EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + reg_at);
        }
    }
    /* not candidates: memreserve entries, disabled and size-only children.
     * 16 memreserve (outside RAM) + 16 candidates (outside) + 1 disabled + 1
     * dynamic -> OK */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSVMAP; i++)
        b_rsv(0x90000000u + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        rm_child("r", 2u, 2u, 0x80000000u + i * 0x1000u, 0x1000u, false);
    b_begin("off");
    b_s("status", "disabled");
    b_reg1(2u, 2u, 0x88000000u, 0x1000u);
    b_end();
    b_begin("dyn");
    b_be("size", 0x1000u, 8u);
    b_end();
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == 0u && C.rsv_ignored == JE_FDT_MAX_RSVMAP + JE_FDT_MAX_RSV &&
          C.rsv_dynamic == 1u);
    /* 16 candidates with a mix of kept / ignored -> OK; the kept ones are below
     * the kept bound */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        rm_child("r", 2u, 2u, ((i & 1u) ? 0x80000000u : RAM_B) + i * 0x1000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_OK);
    CHECK(C.rsv_count == JE_FDT_MAX_RSV / 2u && C.rsv_ignored == JE_FDT_MAX_RSV / 2u);
}

static void test_range_limit(void)
{
    /* > MAX_RAM ranges: one node with 9 pairs, and 9 nodes */
    {
        uint64_t p[2u * (JE_FDT_MAX_RAM + 1u)];
        for (uint32_t i = 0u; i < JE_FDT_MAX_RAM + 1u; i++) {
            p[2u * i] = 0x40000000u + (uint64_t)i * 0x10000000u;
            p[2u * i + 1u] = 0x1000u;
        }
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_reg(2u, 2u, JE_FDT_MAX_RAM, p);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OK);
        CHECK(C.ram_count == JE_FDT_MAX_RAM);
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_reg(2u, 2u, JE_FDT_MAX_RAM + 1u, p);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_RANGE_LIMIT);
        b_reset();
        root_open(2u, 2u);
        for (uint32_t i = 0u; i < JE_FDT_MAX_RAM + 1u; i++)
            mem("m", 2u, 2u, p[2u * i], p[2u * i + 1u]);
        root_close();
        b_finish();
        EXPECT(JE_FDT_RANGE_LIMIT);
    }
    /* > MAX_RSV kept: 16 memreserve + 1 /reserved-memory child */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        b_rsv(RAM_B + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("r", 2u, 2u, 0x48000000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_RANGE_LIMIT);
    test_rsv_candidates();
}

static void test_bad_range(void)
{
    /* memory: zero size (2/2 and 1/1), overflow, exactly 2^64, no reg */
    {
        static const uint64_t cases[][2] = {
            { RAM_B, 0u },
            { 0xfffffffffffff000ull, 0x1000u },
            { 0xffffffffffffffffull, 0x1u },
            { 0x8000000000000000ull, 0x8000000000000000ull },
        };
        for (uint32_t k = 0u; k < sizeof cases / sizeof cases[0]; k++) {
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, cases[k][0], cases[k][1]);
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_RANGE);
        }
    }
    b_reset();
    root_open(1u, 1u);
    mem("m", 1u, 1u, RAM_B, 0u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    {
        /* memory node without reg -> BAD_RANGE at ITS END_NODE (round-4 pin) */
        uint32_t end_at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("m2");
        b_s("device_type", "memory");
        end_at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
    }
    /* second pair in one reg zero-size */
    {
        const uint64_t p[4] = { RAM_B, RAM_S, 0x80000000u, 0u };
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_reg(2u, 2u, 2u, p);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_BAD_RANGE);
    }

    /* reservation PARTIALLY inside RAM: memreserve below/above/straddling, and
     * spanning two adjacent RAM ranges (inside neither ONE range) */
    {
        static const uint64_t part[][2] = {
            { RAM_B - 0x1000u, 0x2000u },
            { RAM_B + RAM_S - 0x1000u, 0x2000u },
            { RAM_B - 0x1000u, RAM_S + 0x2000u },
        };
        for (uint32_t k = 0u; k < 3u; k++) {
            b_reset();
            b_rsv(part[k][0], part[k][1]);
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_RANGE);
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            rm_open(2u, 2u);
            rm_child("r", 2u, 2u, part[k][0], part[k][1], true);
            b_end();
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_RANGE);
        }
        b_reset();
        b_rsv(RAM_B + RAM_S - 0x1000u, 0x2000u);
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        mem("m2", 2u, 2u, RAM_B + RAM_S, RAM_S);
        root_close();
        b_finish();
        EXPECT(JE_FDT_BAD_RANGE);
    }
    /* /reserved-memory child: zero size, overflow */
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("r", 2u, 2u, RAM_B, 0u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    b_reset();
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("r", 2u, 2u, 0xfffffffffffff000ull, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);

    /* initrd: half-present, end <= start, bad lengths, outside / partial / spanning.
     * Exactly one present -> BAD_RANGE at /chosen's END_NODE (round-4 pin). */
    {
        uint32_t end_at;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_be("linux,initrd-start", 0x44000000u, 4u);
        end_at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        b_begin("chosen");
        b_be("linux,initrd-end", 0x44000000u, 8u);
        end_at = b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_BAD_RANGE, I.off_st + end_at);
    }
    {
        static const uint64_t se[][2] = {
            { 0x44000000u, 0x44000000u },               /* end == start */
            { 0x44100000u, 0x44000000u },               /* end < start */
            { 0x30000000u, 0x30100000u },               /* outside RAM */
            { RAM_B + RAM_S - 0x1000u, RAM_B + RAM_S + 0x1000u }, /* partial */
            { RAM_B - 0x1000u, RAM_B + 0x1000u },       /* partial below */
        };
        for (uint32_t k = 0u; k < sizeof se / sizeof se[0]; k++) {
            b_reset();
            root_open(2u, 2u);
            mem("m", 2u, 2u, RAM_B, RAM_S);
            chosen_initrd(se[k][0], 8u, se[k][1], 4u);
            root_close();
            b_finish();
            EXPECT(JE_FDT_BAD_RANGE);
        }
        /* spanning two adjacent RAM ranges */
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        mem("m2", 2u, 2u, RAM_B + RAM_S, RAM_S);
        chosen_initrd(RAM_B + RAM_S - 0x1000u, 4u, RAM_B + RAM_S + 0x1000u, 4u);
        root_close();
        b_finish();
        EXPECT(JE_FDT_BAD_RANGE);
    }
    {
        static const uint32_t lens[] = { 0u, 1u, 2u, 3u, 5u, 6u, 7u, 12u, 16u };
        static const uint8_t v[16] = { 0, 0, 0, 0, 0x44, 0x10, 0, 0, 0, 0, 0, 0, 0x44, 0x10, 0, 0 };
        for (uint32_t w = 0u; w < 2u; w++) {
            for (uint32_t k = 0u; k < sizeof lens / sizeof lens[0]; k++) {
                b_reset();
                root_open(2u, 2u);
                mem("m", 2u, 2u, RAM_B, RAM_S);
                b_begin("chosen");
                if (w == 0u) {
                    b_prop("linux,initrd-start", v, lens[k]);
                    b_be("linux,initrd-end", 0x48000000u, 4u);
                } else {
                    b_be("linux,initrd-start", 0x40000000u, 4u);
                    b_prop("linux,initrd-end", v, lens[k]);
                }
                b_end();
                root_close();
                b_finish();
                EXPECT(JE_FDT_BAD_RANGE);
            }
        }
    }
}

static void test_overlap(void)
{
    static const uint64_t r2[][2] = {
        { RAM_B, RAM_S },                          /* identical */
        { RAM_B + RAM_S - 1u, 0x1000u },           /* one byte */
        { RAM_B + 0x1000u, 0x1000u },              /* contained */
        { RAM_B - 0x1000u, RAM_S + 0x2000u },      /* containing */
    };
    for (uint32_t k = 0u; k < sizeof r2 / sizeof r2[0]; k++) {
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        mem("m2", 2u, 2u, r2[k][0], r2[k][1]);
        root_close();
        b_finish();
        EXPECT(JE_FDT_OVERLAP);
    }
    /* within a single reg, non-adjacent order */
    {
        const uint64_t p[6] = { 0x80000000u, 0x1000u, RAM_B, RAM_S, RAM_B + 0x10u, 0x10u };
        b_reset();
        root_open(2u, 2u);
        b_begin("m");
        b_s("device_type", "memory");
        b_reg(2u, 2u, 3u, p);
        b_end();
        root_close();
        b_finish();
        EXPECT(JE_FDT_OVERLAP);
    }
}

/* Post-walk ORDER: NO_MEMORY -> OVERLAP -> reservations (memreserve first, then
 * /reserved-memory in walk order) -> initrd. */
static void test_postwalk_order(void)
{
    /* OVERLAP before a partial reservation and a bad initrd */
    b_reset();
    b_rsv(RAM_B - 0x1000u, 0x2000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    mem("m2", 2u, 2u, RAM_B, RAM_S);
    chosen_initrd(0x30000000u, 4u, 0x30100000u, 4u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_OVERLAP);

    /* 17th kept reservation found before a later partial one -> RANGE_LIMIT */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        b_rsv(RAM_B + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("k", 2u, 2u, 0x48000000u, 0x1000u, false);
    rm_child("p", 2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_RANGE_LIMIT);
    /* ... partial one first in walk order -> BAD_RANGE */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        b_rsv(RAM_B + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("p", 2u, 2u, RAM_B + RAM_S - 0x1000u, 0x2000u, false);
    rm_child("k", 2u, 2u, 0x48000000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    /* Updated for contract bdf446a: 17 /reserved-memory reg CANDIDATES are a
     * WALK-time RANGE_LIMIT (err_off = the 17th reg PROP), so they beat a
     * partial memreserve entry whose BAD_RANGE is only decided post-walk.
     * (Round 1 expected BAD_RANGE here, from the post-walk-only bound.) */
    {
        uint32_t reg_at = 0u;
        b_reset();
        b_rsv(RAM_B - 0x1000u, 0x2000u);
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++)
            reg_at = rm_child("r", 2u, 2u, 0x48000000u + i * 0x1000u, 0x1000u, false);
        b_end();
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
    }
    /* memreserve entries are still processed BEFORE /reserved-memory post-walk:
     * with only 16 candidates (16 kept children, under the walk bound) a partial
     * memreserve entry is BAD_RANGE ... */
    b_reset();
    b_rsv(RAM_B - 0x1000u, 0x2000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        rm_child("r", 2u, 2u, 0x48000000u + i * 0x1000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    /* ... 1 kept memreserve + 16 kept children = 17 kept -> post-walk
     * kept-bound RANGE_LIMIT; a partial memreserve entry placed before the kept
     * one is classified first and still wins (BAD_RANGE). */
    b_reset();
    b_rsv(RAM_B, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        rm_child("r", 2u, 2u, 0x48000000u + i * 0x1000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_RANGE_LIMIT);
    b_reset();
    b_rsv(RAM_B - 0x1000u, 0x2000u);
    b_rsv(RAM_B, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        rm_child("r", 2u, 2u, 0x48000000u + i * 0x1000u, 0x1000u, false);
    b_end();
    root_close();
    b_finish();
    EXPECT(JE_FDT_BAD_RANGE);
    /* post-walk kept-bound RANGE_LIMIT (16 memreserve + 1 child, all kept)
     * precedes the initrd-in-RAM check */
    b_reset();
    for (uint32_t i = 0u; i < JE_FDT_MAX_RSV; i++)
        b_rsv(RAM_B + i * 0x1000u, 0x1000u);
    root_open(2u, 2u);
    mem("m", 2u, 2u, RAM_B, RAM_S);
    rm_open(2u, 2u);
    rm_child("r", 2u, 2u, 0x48000000u, 0x1000u, false);
    b_end();
    chosen_initrd(0x30000000u, 4u, 0x30100000u, 4u);
    root_close();
    b_finish();
    EXPECT(JE_FDT_RANGE_LIMIT);
    /* walk-time candidate RANGE_LIMIT also precedes it */
    {
        uint32_t reg_at = 0u;
        b_reset();
        root_open(2u, 2u);
        mem("m", 2u, 2u, RAM_B, RAM_S);
        rm_open(2u, 2u);
        for (uint32_t i = 0u; i < JE_FDT_MAX_RSV + 1u; i++)
            reg_at = rm_child("r", 2u, 2u, 0x48000000u + i * 0x1000u, 0x1000u, false);
        b_end();
        chosen_initrd(0x30000000u, 4u, 0x30100000u, 4u);
        root_close();
        b_finish();
        EXPECT_AT(JE_FDT_RANGE_LIMIT, I.off_st + reg_at);
    }
}

/* ------------------------------------------------------------------------- */
/* 5. je_fdt_copy_plan                                                        */

static je_fdt_status plan(const uint8_t *hdr40, uint32_t *cl, uint32_t *ts)
{
    uint8_t *h = malloc(JE_FDT_HEADER_SIZE);
    CHECK(h != NULL);
    memcpy(h, hdr40, JE_FDT_HEADER_SIZE);
    *cl = 0xdeadbeefu;
    *ts = 0xdeadbeefu;
    je_fdt_status s = je_fdt_copy_plan(h, cl, ts);
    free(h);
    CHECK((unsigned)s < (unsigned)JE_FDT_STATUS_COUNT);
    return s;
}

static void test_copy_plan(void)
{
    uint8_t h[JE_FDT_HEADER_SIZE];
    uint32_t cl, ts;

    memcpy(h, qemu_virt82_dtb, sizeof h);
    want_(plan(h, &cl, &ts), JE_FDT_OK, __LINE__);
    CHECK(cl == 65536u && cl == JE_FDT_MAX_COPY && ts == 1048576u);

    static const uint32_t sizes[][2] = {
        { JE_FDT_MAX_TOTAL, JE_FDT_MAX_COPY },
        { JE_FDT_MAX_COPY + 1u, JE_FDT_MAX_COPY },
        { JE_FDT_MAX_COPY, JE_FDT_MAX_COPY },
        { JE_FDT_MAX_COPY - 1u, JE_FDT_MAX_COPY - 1u },
        { 7426u, 7426u },
        { 40u, 40u },
    };
    for (uint32_t k = 0u; k < sizeof sizes / sizeof sizes[0]; k++) {
        be32(h + H_TOTAL, sizes[k][0]);
        want_(plan(h, &cl, &ts), JE_FDT_OK, __LINE__);
        CHECK(cl == sizes[k][1] && ts == sizes[k][0]);
    }
    be32(h + H_TOTAL, JE_FDT_MAX_TOTAL + 1u);
    want_(plan(h, &cl, &ts), JE_FDT_TOO_LARGE, __LINE__);
    be32(h + H_TOTAL, 0xffffffffu);
    want_(plan(h, &cl, &ts), JE_FDT_TOO_LARGE, __LINE__);
    be32(h + H_TOTAL, 39u);
    want_(plan(h, &cl, &ts), JE_FDT_BAD_LAYOUT, __LINE__);
    be32(h + H_TOTAL, 0u);
    want_(plan(h, &cl, &ts), JE_FDT_BAD_LAYOUT, __LINE__);
    /* magic checked before size */
    be32(h + H_MAGIC, 0xedfe0dd0u);
    want_(plan(h, &cl, &ts), JE_FDT_BAD_MAGIC, __LINE__);
    be32(h + H_TOTAL, JE_FDT_MAX_TOTAL + 1u);
    want_(plan(h, &cl, &ts), JE_FDT_BAD_MAGIC, __LINE__);
    /* plan reads only the header: other fields (version etc.) are irrelevant */
    memcpy(h, qemu_virt82_dtb, sizeof h);
    be32(h + H_VER, 1u);
    be32(h + H_OFF_ST, 3u);
    want_(plan(h, &cl, &ts), JE_FDT_OK, __LINE__);
    CHECK(je_fdt_copy_plan(NULL, &cl, &ts) == JE_FDT_BAD_ARG);
}

/* ------------------------------------------------------------------------- */
/* 6. je_boot_ctx_check (hand-built contexts: independent of parse())         */

static je_boot_ctx K;
static je_boot_facts F;

static void check_base(void)
{
    memset(&K, 0, sizeof K);
    K.totalsize = 0x100000u;
    K.used = 0x2000u;
    K.addr_cells = 2u;
    K.size_cells = 2u;
    K.node_count = 4u;
    K.max_depth = 3u;
    K.ram_count = 1u;
    K.ram[0] = (je_range){ RAM_B, RAM_S };
    K.rsv_count = 3u;
    K.rsv[0] = (je_range){ 0x4f800000u, 0x1000u };
    K.rsv[1] = (je_range){ 0x48000000u, 0x10000u };
    K.rsv[2] = (je_range){ 0x48008000u, 0x10000u };
    K.has_initrd = true;
    K.initrd = (je_range){ 0x44000000u, 0x100000u };
    memset(&F, 0, sizeof F);
    F.kernel_image = (je_range){ 0x40080000u, 0x400000u }; /* ends 0x40480000 */
    F.dtb_src = (je_range){ 0x4f000000u, 0x100000u };
    F.pa_bits = 40u;
}

static void want_chk_(je_fdt_status want, uint32_t want_idx, bool idx, int line)
{
    uint32_t e = 0x5a5a5a5au;
    je_fdt_status s = je_boot_ctx_check(&K, &F, &e);
    if (s != want || (idx && e != want_idx)) {
        fprintf(stderr, "fdt: FAIL %s:%d: check want %s/0x%x got %s/0x%x\n", __FILE__,
                line, nm(want), want_idx, nm(s), e);
        abort();
    }
}
/* check_base() with EVERY ram[] / rsv[] slot a valid, accepted entry, so an
 * unclamped count walks past the last slot instead of failing early on a
 * zeroed (invalid) entry. ram[1..7] are disjoint 4 KiB ranges above RAM;
 * rsv[3..15] lie inside RAM, clear of the kernel, DTB and initrd. */
static void check_full(void)
{
    check_base();
    K.ram_count = JE_FDT_MAX_RAM;
    for (uint32_t i = 1u; i < JE_FDT_MAX_RAM; i++)
        K.ram[i] = (je_range){ 0x80000000u + (uint64_t)i * 0x100000u, 0x1000u };
    K.rsv_count = JE_FDT_MAX_RSV;
    for (uint32_t i = 3u; i < JE_FDT_MAX_RSV; i++)
        K.rsv[i] = (je_range){ 0x4e000000u + (uint64_t)i * 0x1000u, 0x1000u };
}

#define CHK(st, idx) want_chk_((st), (idx), true, __LINE__)
#define CHK_ST(st) want_chk_((st), 0u, false, __LINE__)

static void test_check(void)
{
    uint32_t e;

    check_base();
    CHK(JE_FDT_OK, 0u);
    CHECK(je_boot_ctx_check(NULL, &F, &e) == JE_FDT_BAD_ARG);
    CHECK(je_boot_ctx_check(&K, NULL, &e) == JE_FDT_BAD_ARG);
    CHECK(je_boot_ctx_check(&K, &F, NULL) == JE_FDT_BAD_ARG);
    /* NULL ctx / facts with a non-NULL err_idx -> err_idx FACTS (contract
     * 44187ac; Claude parser review nit) */
    e = 0x5a5a5a5au;
    CHECK(je_boot_ctx_check(NULL, &F, &e) == JE_FDT_BAD_ARG && e == JE_BOOT_ERR_FACTS);
    e = 0x5a5a5a5au;
    CHECK(je_boot_ctx_check(&K, NULL, &e) == JE_FDT_BAD_ARG && e == JE_BOOT_ERR_FACTS);
    e = 0x5a5a5a5au;
    CHECK(je_boot_ctx_check(NULL, NULL, &e) == JE_FDT_BAD_ARG && e == JE_BOOT_ERR_FACTS);

    /* count clamp (contract 5a3c14b): ram_count > MAX_RAM or rsv_count >
     * MAX_RSV -> BAD_ARG, err_idx FACTS, before the pa_bits/facts checks and
     * WITHOUT indexing past the arrays. The context is an exact-size heap copy
     * so ASan flags any read past the struct (count 0xffffffff); a read of
     * ram[JE_FDT_MAX_RAM] / rsv[JE_FDT_MAX_RSV] stays inside the struct, which
     * ASan cannot see — UBSan -fsanitize=bounds (in -fsanitize=undefined)
     * catches that only when the parser indexes the declared arrays directly. */
    {
        static const uint32_t rc[] = { JE_FDT_MAX_RAM + 1u, 0xffffffffu };
        static const uint32_t sc[] = { JE_FDT_MAX_RSV + 1u, 0xffffffffu };
        for (uint32_t w = 0u; w < 2u; w++) {
            for (uint32_t k = 0u; k < 3u; k++) {
                je_boot_ctx *h = malloc(sizeof *h);
                CHECK(h != NULL);
                check_full();
                *h = K;
                if (w == 0u)
                    h->ram_count = rc[k & 1u];
                else
                    h->rsv_count = sc[k & 1u];
                if (k == 2u)
                    F.pa_bits = 0u;   /* BAD_ARG precedes the facts BAD_RANGE */
                e = 0x5a5a5a5au;
                je_fdt_status st = je_boot_ctx_check(h, &F, &e);
                free(h);
                if (st != JE_FDT_BAD_ARG || e != JE_BOOT_ERR_FACTS)
                    FAILF("count clamp w=%u k=%u: got %s/0x%x, want BAD_ARG/FACTS", w, k,
                          nm(st), e);
            }
        }
        /* boundary: exactly MAX_RAM / MAX_RSV (all slots valid) is accepted */
        check_full();
        CHK(JE_FDT_OK, 0u);
    }

    /* pa_bits bounds */
    static const uint32_t badpa[] = { 0u, 31u, 53u, 64u, 0xffffffffu };
    for (uint32_t k = 0u; k < sizeof badpa / sizeof badpa[0]; k++) {
        check_base();
        F.pa_bits = badpa[k];
        CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_FACTS);
    }
    check_base();
    F.pa_bits = 32u;
    CHK(JE_FDT_OK, 0u);
    F.pa_bits = 52u;
    CHK(JE_FDT_OK, 0u);
    /* invalid facts ranges */
    check_base();
    F.kernel_image.size = 0u;
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_FACTS);
    check_base();
    F.kernel_image = (je_range){ 0xfffffffffffff000ull, 0x1000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_FACTS);
    check_base();
    F.dtb_src.size = 0u;
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_FACTS);
    check_base();
    F.dtb_src = (je_range){ 0xffffffffffffff00ull, 0x200u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_FACTS);
    /* FACTS precedes KERNEL */
    check_base();
    F.pa_bits = 31u;
    F.kernel_image = (je_range){ 0x10000000u, 0x1000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_FACTS);

    /* ends above 2^pa_bits (end == 2^pa_bits is fine: exclusive) */
    check_base();
    K.ram_count = 2u;
    K.ram[1] = (je_range){ 0xf0000000u, 0x10000000u }; /* ends exactly 2^32 */
    F.pa_bits = 32u;
    CHK(JE_FDT_OK, 0u);
    K.ram[1].size = 0x10001000u;                        /* ends above 2^32 */
    CHK_ST(JE_FDT_BAD_RANGE);
    check_base();
    K.ram_count = 2u;
    K.ram[1] = (je_range){ (1ull << 52) - 0x10000000u, 0x10000000u };
    F.pa_bits = 52u;
    CHK(JE_FDT_OK, 0u);
    F.pa_bits = 51u;
    CHK_ST(JE_FDT_BAD_RANGE);
    check_base();                                      /* kept reservation above */
    K.ram_count = 2u;
    K.ram[1] = (je_range){ 0x200000000ull, 0x10000000u };
    K.rsv[2] = (je_range){ 0x200000000ull, 0x1000u };
    F.pa_bits = 34u;
    CHK(JE_FDT_OK, 0u);
    F.pa_bits = 33u;
    CHK_ST(JE_FDT_BAD_RANGE);

    /* kernel not fully inside one RAM range */
    check_base();
    F.kernel_image = (je_range){ 0x30000000u, 0x1000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_KERNEL);
    check_base();
    F.kernel_image = (je_range){ RAM_B + RAM_S - 0x1000u, 0x2000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_KERNEL);
    check_base();
    K.ram_count = 2u;
    K.ram[1] = (je_range){ RAM_B + RAM_S, RAM_S };     /* adjacent */
    F.kernel_image = (je_range){ RAM_B + RAM_S - 0x1000u, 0x2000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_KERNEL);
    /* KERNEL precedes DTB */
    check_base();
    F.kernel_image = (je_range){ 0x30000000u, 0x1000u };
    F.dtb_src = (je_range){ 0x31000000u, 0x1000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_KERNEL);
    /* dtb not fully inside one RAM range */
    check_base();
    F.dtb_src = (je_range){ 0x31000000u, 0x1000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_DTB);
    check_base();
    F.dtb_src = (je_range){ RAM_B + RAM_S - 0x800u, 0x1000u };
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_DTB);
    /* DTB containment precedes overlaps */
    check_base();
    F.dtb_src = (je_range){ RAM_B + RAM_S - 0x800u, 0x1000u };
    F.kernel_image = (je_range){ 0x44000000u, 0x1000u }; /* overlaps initrd */
    CHK(JE_FDT_BAD_RANGE, JE_BOOT_ERR_DTB);

    /* kernel overlaps dtb */
    check_base();
    F.dtb_src = (je_range){ 0x40100000u, 0x1000u };
    CHK(JE_FDT_OVERLAP, JE_BOOT_ERR_DTB);
    check_base();
    F.dtb_src = (je_range){ 0x40000000u, 0x80001u };     /* last byte overlaps */
    CHK(JE_FDT_OVERLAP, JE_BOOT_ERR_DTB);
    check_base();
    F.dtb_src = (je_range){ 0x40000000u, 0x80000u };     /* adjacent below */
    CHK(JE_FDT_OK, 0u);
    F.dtb_src = (je_range){ 0x40480000u, 0x1000u };      /* adjacent above */
    CHK(JE_FDT_OK, 0u);
    /* kernel overlaps dtb precedes kernel overlaps initrd */
    check_base();
    F.dtb_src = (je_range){ 0x40100000u, 0x1000u };
    K.initrd = (je_range){ 0x40200000u, 0x1000u };
    CHK(JE_FDT_OVERLAP, JE_BOOT_ERR_DTB);

    /* kernel overlaps initrd */
    check_base();
    K.initrd = (je_range){ 0x40400000u, 0x100000u };
    CHK(JE_FDT_OVERLAP, JE_BOOT_ERR_INITRD);
    check_base();
    K.initrd = (je_range){ 0x40480000u, 0x1000u };       /* adjacent */
    CHK(JE_FDT_OK, 0u);
    /* has_initrd false: the (zero) initrd is not considered */
    check_base();
    K.has_initrd = false;
    K.initrd = (je_range){ 0u, 0u };
    CHK(JE_FDT_OK, 0u);
    /* kernel overlaps initrd precedes kernel overlaps reservation */
    check_base();
    K.initrd = (je_range){ 0x40400000u, 0x1000u };
    K.rsv[0] = (je_range){ 0x40100000u, 0x1000u };
    CHK(JE_FDT_OVERLAP, JE_BOOT_ERR_INITRD);

    /* kernel overlaps kept reservation i (lowest i) */
    check_base();
    K.rsv[2] = (je_range){ 0x40100000u, 0x1000u };
    CHK(JE_FDT_OVERLAP, 2u);
    K.rsv[1] = (je_range){ 0x40200000u, 0x1000u };
    CHK(JE_FDT_OVERLAP, 1u);
    K.rsv[0] = (je_range){ 0x4047f000u, 0x2000u };
    CHK(JE_FDT_OVERLAP, 0u);
    /* only kept (rsv_count) reservations are examined */
    check_base();
    K.rsv_count = 2u;
    K.rsv[2] = (je_range){ 0x40100000u, 0x1000u };
    CHK(JE_FDT_OK, 0u);
    /* kernel overlaps reservation precedes dtb overlaps initrd */
    check_base();
    K.rsv[1] = (je_range){ 0x40100000u, 0x1000u };
    F.dtb_src = (je_range){ 0x44080000u, 0x100000u };
    CHK(JE_FDT_OVERLAP, 1u);

    /* dtb overlaps initrd */
    check_base();
    F.dtb_src = (je_range){ 0x44080000u, 0x100000u };
    CHK(JE_FDT_OVERLAP, JE_BOOT_ERR_INITRD);
    /* allowed overlaps: dtb overlaps reservation, initrd overlaps reservation, rsv overlaps rsv */
    check_base();
    F.dtb_src = (je_range){ 0x4f800000u, 0x100000u };
    K.rsv[1] = (je_range){ 0x44000000u, 0x10000u };
    CHK(JE_FDT_OK, 0u);

    /* round trip from a real parse */
    want_(parse_bytes(qemu_virt82_dtb, QEMU_VIRT82_DTB_LEN, &K, &e), JE_FDT_OK, __LINE__);
    memset(&F, 0, sizeof F);
    F.kernel_image = (je_range){ 0x40200000u, 0x800000u };
    F.dtb_src = (je_range){ 0x40000000u, QEMU_VIRT82_DTB_TOTALSIZE };
    F.pa_bits = 40u;
    CHK(JE_FDT_OK, 0u);
    F.dtb_src = (je_range){ 0x40180000u, QEMU_VIRT82_DTB_TOTALSIZE };
    CHK(JE_FDT_OVERLAP, JE_BOOT_ERR_DTB);
}

/* ------------------------------------------------------------------------- */
/* 7. robustness                                                              */

static void test_truncation_sweep(void)
{
    for (uint32_t len = 0u; len < QEMU_VIRT82_DTB_LEN; len++) {
        je_fdt_status s = parse_bytes(qemu_virt82_dtb, len, &C, &EO);
        if (s != JE_FDT_TRUNCATED)
            FAILF("truncation len %u: got %s, want TRUNCATED", len, nm(s));
        if (len < JE_FDT_HEADER_SIZE && EO != 0u)
            FAILF("truncation len %u: err_off %u, want 0", len, EO);
    }
}

static uint32_t g_lcg = 0x52f00d15u;

static uint32_t rnd(void)
{
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg >> 8;
}

static bool valid_range(je_range r)
{
    return r.size != 0u && r.base <= UINT64_MAX - r.size;
}

static bool inside(je_range in, je_range out)
{
    return in.base >= out.base && in.base + in.size <= out.base + out.size;
}

static bool in_one_ram(const je_boot_ctx *c, je_range r)
{
    for (uint32_t i = 0u; i < c->ram_count; i++)
        if (inside(r, c->ram[i]))
            return true;
    return false;
}

/* Contract-derived post-conditions of any successful parse. */
static void ok_invariants(const je_boot_ctx *c, size_t len)
{
    CHECK(c->addr_cells == 1u || c->addr_cells == 2u);
    CHECK(c->size_cells == 1u || c->size_cells == 2u);
    CHECK(c->totalsize >= JE_FDT_HEADER_SIZE && c->totalsize <= JE_FDT_MAX_TOTAL);
    CHECK(c->used <= len && c->used <= c->totalsize && c->used >= JE_FDT_HEADER_SIZE);
    CHECK(c->node_count >= 1u && c->node_count <= JE_FDT_MAX_NODES);
    CHECK(c->max_depth >= 1u && c->max_depth <= JE_FDT_MAX_DEPTH);
    CHECK(c->ram_count >= 1u && c->ram_count <= JE_FDT_MAX_RAM);
    for (uint32_t i = 0u; i < c->ram_count; i++) {
        CHECK(valid_range(c->ram[i]));
        for (uint32_t j = i + 1u; j < c->ram_count; j++) {
            je_range a = c->ram[i], b = c->ram[j];
            CHECK(a.base + a.size <= b.base || b.base + b.size <= a.base);
        }
    }
    CHECK(c->rsv_count <= JE_FDT_MAX_RSV);
    for (uint32_t i = 0u; i < c->rsv_count; i++) {
        CHECK(valid_range(c->rsv[i]) && in_one_ram(c, c->rsv[i]));
        CHECK(c->rsv_nomap[i] <= 1u);
    }
    const unsigned char hb = *(const unsigned char *)&c->has_initrd;
    CHECK(hb <= 1u);
    if (hb != 0u)
        CHECK(valid_range(c->initrd) && in_one_ram(c, c->initrd));
}

static void build_rich(void)
{
    b_reset();
    b_rsv(RAM_B + 0x1000u, 0x1000u);
    b_rsv(0x90000000u, 0x1000u);
    root_open(2u, 2u);
    b_s("compatible", "linux,dummy-virt");
    mem("memory@40000000", 2u, 2u, RAM_B, RAM_S);
    mem("memory@50000000", 2u, 2u, RAM_B + RAM_S, RAM_S);
    rm_open(2u, 2u);
    rm_child("a", 2u, 2u, 0x48000000u, 0x100000u, true);
    rm_child("b", 2u, 2u, 0x10000000u, 0x1000u, false);
    b_begin("d");
    b_be("size", 0x1000u, 8u);
    b_end();
    b_end();
    b_begin("soc");
    b_u32("#address-cells", 1u);
    b_u32("#size-cells", 1u);
    b_begin("uart@9000000");
    b_reg1(1u, 1u, 0x09000000u, 0x1000u);
    b_s("status", "okay");
    b_end();
    b_end();
    chosen_initrd(0x44000000u, 4u, 0x44100000u, 8u);
    root_close();
    b_finish();
}

/* Mutation statistics, reported honestly in the PASS line. */
typedef struct {
    uint32_t iters;      /* iterations run */
    uint32_t mutations;  /* individual mutations applied, each a real byte change */
    uint32_t truncated;  /* iterations that also parsed a strict prefix */
    uint32_t ok;         /* iterations whose (changed) input still parsed OK */
} mut_stats;

/* Every mutation changes at least one byte (asserted), and every iteration's
 * parsed input differs from the seed: either its bytes differ within the parsed
 * length, or it is a strict prefix (asserted). Mutations that would cancel
 * (e.g. the same bit flipped twice) are followed by one guaranteed flip. */
static void mutate_loop(const uint8_t *seed, uint32_t len, uint32_t iters, mut_stats *st)
{
    static uint8_t m[IMG_CAP];
    static je_boot_ctx c2;
    static const uint32_t words[] = { 0u, 1u, 2u, 3u, 4u, 9u, 16u, 17u, 40u,
                                      0x7fffffffu, 0x80000000u, 0xfffffffcu,
                                      0xffffffffu, JE_FDT_MAGIC };
    CHECK(len >= 4u && len <= IMG_CAP);
    for (uint32_t it = 0u; it < iters; it++) {
        memcpy(m, seed, len);
        size_t plen = len;
        if (rnd() % 16u == 0u)
            plen = rnd() % len; /* strict prefix: 0 .. len-1 */
        /* mutate only bytes that are parsed, so a change is observable (for
         * plen == 0 nothing is parsed; the prefix itself is the change) */
        uint32_t span = (plen != 0u) ? (uint32_t)plen : len;
        uint32_t nmut = 1u + rnd() % 4u;
        for (uint32_t k = 0u; k < nmut; k++) {
            uint32_t mode = rnd() % 4u;
            /* bias a quarter of mutations into the header */
            uint32_t pos = (rnd() % 4u == 0u && span > JE_FDT_HEADER_SIZE)
                               ? rnd() % JE_FDT_HEADER_SIZE
                               : rnd() % span;
            uint32_t wp = pos & ~3u;
            if (mode >= 2u && wp + 4u <= span) {
                uint32_t old = rd32(&m[wp]);
                uint32_t v = words[rnd() % (sizeof words / sizeof words[0])];
                if (v == old)
                    v = ~old; /* never a no-op word write */
                be32(&m[wp], v);
                CHECK(rd32(&m[wp]) != old);
            } else if (mode == 0u) {
                uint8_t old = m[pos];
                uint8_t v = (uint8_t)rnd();
                if (v == old)
                    v = (uint8_t)~old; /* never a no-op byte write */
                m[pos] = v;
                CHECK(m[pos] != old);
            } else {
                /* mode 1, or a word write that would not fit: one bit flip */
                uint8_t old = m[pos];
                m[pos] ^= (uint8_t)(1u << (rnd() % 8u));
                CHECK(m[pos] != old);
            }
            st->mutations++;
        }
        if (plen == len && memcmp(m, seed, len) == 0) {
            m[rnd() % len] ^= 0x01u; /* mutations cancelled: force a change */
            st->mutations++;
        }
        CHECK(plen < len || memcmp(m, seed, len) != 0);
        if (plen < len)
            st->truncated++;
        st->iters++;
        uint32_t eo1, eo2;
        je_fdt_status s1 = parse_bytes(m, plen, &C, &eo1);
        je_fdt_status s2 = parse_bytes(m, plen, &c2, &eo2);
        if (s1 != s2 || eo1 != eo2 || !ctx_eq(&C, &c2))
            FAILF("nondeterministic parse at iteration %u (%s/%u vs %s/%u)", it, nm(s1),
                  eo1, nm(s2), eo2);
        if (s1 == JE_FDT_OK) {
            ok_invariants(&C, plen);
            st->ok++;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 8. names and CRC                                                           */

static void test_names_crc(void)
{
    for (unsigned s = 0u; s < (unsigned)JE_FDT_STATUS_COUNT; s++) {
        const char *n = je_fdt_status_name((je_fdt_status)s);
        CHECK(n != NULL);
        if (strcmp(n, k_names[s]) != 0)
            FAILF("status_name(%u) = \"%s\", want \"%s\"", s, n, k_names[s]);
        CHECK(strcmp(n, "UNKNOWN") != 0);
        for (unsigned t = 0u; t < s; t++)
            CHECK(strcmp(n, je_fdt_status_name((je_fdt_status)t)) != 0);
    }
    CHECK(strcmp(je_fdt_status_name(JE_FDT_OK), "OK") == 0);
    CHECK(strcmp(je_fdt_status_name(JE_FDT_STATUS_COUNT), "UNKNOWN") == 0);
    CHECK(strcmp(je_fdt_status_name((je_fdt_status)999), "UNKNOWN") == 0);

    {
        uint8_t *p = malloc(9u);
        CHECK(p != NULL);
        memcpy(p, "123456789", 9u);
        CHECK(je_fdt_crc32(p, 9u) == 0xCBF43926u);
        CHECK(je_fdt_crc32(p, 0u) == 0u);
        CHECK(je_fdt_crc32(p, 1u) == 0x83DCEFB7u); /* crc32("1") */
        free(p);
    }
    {
        uint8_t *p = malloc(QEMU_VIRT82_DTB_LEN);
        CHECK(p != NULL);
        memcpy(p, qemu_virt82_dtb, QEMU_VIRT82_DTB_LEN);
        CHECK(je_fdt_crc32(p, QEMU_VIRT82_DTB_LEN) == QEMU_VIRT82_DTB_CRC32);
        free(p);
    }
}

int main(void)
{
    mut_stats mf = { 0u, 0u, 0u, 0u }, mr = { 0u, 0u, 0u, 0u };

    test_names_crc();
    test_fixture();
    test_positive_cells();
    test_positive_memory();
    test_positive_layout();
    test_positive_rsv();
    test_positive_initrd();
    test_positive_literal_scope();
    test_bad_arg_parse();
    test_header();
    test_layout();
    test_rsvmap();
    test_tokens();
    test_strings();
    test_unterminated_name();
    test_limits();
    test_duplicate();
    test_conflict();
    test_status_values();
    test_bad_cells();
    test_dup_before_value();
    test_unconsumed_strings();
    test_memory_status_timing();
    test_rsv_cells_deferral();
    test_reg_timing();
    test_end_node_pins();
    test_no_memory();
    test_range_limit();
    test_bad_range();
    test_overlap();
    test_postwalk_order();
    test_copy_plan();
    test_check();
    test_truncation_sweep();
    mutate_loop(qemu_virt82_dtb, QEMU_VIRT82_DTB_LEN, 20000u, &mf);
    build_rich();
    EXPECT(JE_FDT_OK);
    {
        static uint8_t rich[IMG_CAP];
        uint32_t rl = I.len;
        memcpy(rich, I.buf, rl);
        mutate_loop(rich, rl, 10000u, &mr);
    }
    CHECK(mf.iters == 20000u && mr.iters == 10000u);
    printf("fdt: PASS (fixture, builder positive/negative per status, copy_plan, "
           "boot_ctx_check, %u-len truncation sweep; mutation: fixture %u iters "
           "(%u byte-changing mutations, %u also truncated, %u still OK), rich %u iters "
           "(%u mutations, %u truncated, %u OK); every iteration's input differs "
           "from its seed; each parsed twice for determinism)\n",
           QEMU_VIRT82_DTB_LEN, mf.iters, mf.mutations, mf.truncated, mf.ok, mr.iters,
           mr.mutations, mr.truncated, mr.ok);
    return 0;
}
