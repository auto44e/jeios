/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bounded flattened-devicetree (FDT/DTB) boot-context parser — issue #52 (MP1
 * completion). Arch-neutral, pure, hostable: no allocation, no arch includes,
 * bounded loops, all multi-byte reads are big-endian BYTE loads (safe under
 * -mstrict-align on pre-MMU Device memory). ARM64 uses it now; RISC-V later.
 *
 * THIS HEADER IS THE FROZEN CONTRACT that the parser (kernel/core/fdt.c), the
 * hosted tests (tests/test_fdt.c) and the ARM glue are written against by
 * different authors. Change it only by an explicit, reviewed contract update.
 * Decision record + both peer reviews: docs/adr/debates/mp1-dtb.md.
 *
 * Model (ADR 0002 D3, frozen): the kernel COPIES the DTB into its own reserved
 * buffer before the MMU, and parses ONLY the copy (TOCTOU-free):
 *   1. glue reads the 40-byte header from firmware memory and calls
 *      je_fdt_copy_plan() to get copy_len = min(totalsize, JE_FDT_MAX_COPY);
 *   2. glue copies copy_len bytes once into its buffer;
 *   3. je_fdt_parse(copy, copy_len, ...) re-derives EVERYTHING from the copy
 *      (the header values read in step 1 only sized the copy);
 *   4. je_boot_ctx_check() validates the result against arch facts.
 */
#ifndef JEIOS_FDT_H
#define JEIOS_FDT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- policy bounds (compile-time; raise only via contract update) ---- */
#define JE_FDT_HEADER_SIZE        40u
#define JE_FDT_MAGIC              0xd00dfeedu
#define JE_FDT_MAX_TOTAL          (2u * 1024u * 1024u) /* header totalsize cap (project policy) */
#define JE_FDT_MAX_COPY           (64u * 1024u)        /* copy window; QEMU virt-8.2 live extent ~7.4 KiB */
#define JE_FDT_MAX_DEPTH          16u                  /* root node is depth 1 */
#define JE_FDT_MAX_NODES          1024u
#define JE_FDT_MAX_PROPS_PER_NODE 64u                  /* counts EVERY property, consumed or not */
#define JE_FDT_MAX_NAME           255u                 /* node-name bytes, excluding NUL */
#define JE_FDT_MAX_RAM            8u
#define JE_FDT_MAX_RSV            16u                  /* kept reservations (memreserve + reserved-memory) */
#define JE_FDT_MAX_RSVMAP         16u                  /* non-terminator memreserve entries scanned */

/* ---- status: one deterministic value per failure class ---- */
typedef enum {
    JE_FDT_OK = 0,
    JE_FDT_BAD_ARG,      /* NULL pointer / impossible argument */
    JE_FDT_TRUNCATED,    /* len < 40, or a required byte lies at/after len (but the header
                          * says it exists). Glue: TRUNCATED with len == JE_FDT_MAX_COPY and
                          * totalsize > len means "live data exceeds the copy window". */
    JE_FDT_BAD_MAGIC,
    JE_FDT_TOO_LARGE,    /* totalsize > JE_FDT_MAX_TOTAL */
    JE_FDT_BAD_VERSION,  /* version < 17 or last_comp_version > 17 */
    JE_FDT_BAD_LAYOUT,   /* header block offsets/sizes/alignment/order/overflow, rsvmap
                          * terminator missing before off_dt_struct, a token or PROP
                          * value running past the struct block, END not the last word.
                          * (A node NAME running past the block is BAD_STRING.) */
    JE_FDT_BAD_TOKEN,    /* unknown token; first node not an empty-named root; second
                          * root; PROP after a child node; END_NODE at depth 0; END with
                          * depth != 0; non-NOP/END token after the root's END_NODE */
    JE_FDT_BAD_STRING,   /* node name with no NUL before the struct-block end, or >
                          * JE_FDT_MAX_NAME bytes; nameoff >=
                          * size_dt_strings or no NUL inside the strings block */
    JE_FDT_DEPTH_LIMIT,
    JE_FDT_NODE_LIMIT,
    JE_FDT_PROP_LIMIT,
    JE_FDT_DUPLICATE,    /* a CONSUMED property repeated in one node, or a second
                          * /chosen or /reserved-memory node */
    JE_FDT_BAD_CELLS,    /* root #address-cells/#size-cells absent, len != 4, or not in
                          * {1,2}; /reserved-memory cells absent or != root's, or its
                          * `ranges` absent or non-empty; a `reg` length that is zero or
                          * not a multiple of (ac+sc)*4 */
    JE_FDT_NO_MEMORY,    /* no usable RAM range after the walk */
    JE_FDT_RANGE_LIMIT,  /* > JE_FDT_MAX_RAM RAM ranges, > JE_FDT_MAX_RSV kept
                          * reservations, > JE_FDT_MAX_RSVMAP memreserve entries, or
                          * > JE_FDT_MAX_RSV /reserved-memory `reg` CANDIDATES during the
                          * walk (see SEMANTICS) */
    JE_FDT_BAD_RANGE,    /* zero size, base+size overflows u64, memreserve entry with
                          * size==0 && addr!=0, memory node without reg, reservation
                          * PARTIALLY inside RAM, initrd half-present / end<=start / not
                          * fully inside one RAM range / bad length; check(): an
                          * arch-fact range invalid or above 2^pa_bits */
    JE_FDT_OVERLAP,      /* two RAM ranges overlap; check(): kernel image overlaps the
                          * DTB source, the initrd or any kept reservation; DTB source
                          * overlaps the initrd */
    JE_FDT_STATUS_COUNT
} je_fdt_status;

/* Stable short name ("OK", "BAD_MAGIC", ...); "UNKNOWN" if out of range. */
const char *je_fdt_status_name(je_fdt_status s);

/* Half-open physical range [base, base+size). Valid iff size > 0 and
 * base + size does not overflow (base + size <= UINT64_MAX, exclusive end). */
typedef struct {
    uint64_t base;
    uint64_t size;
} je_range;

/* Extracted boot context. Fixed size, no pointers into the blob. Zeroed on any
 * failure. *_off are byte offsets in the blob of the token/entry that supplied
 * each range (diagnostics only). */
typedef struct {
    uint32_t totalsize;            /* header totalsize (may exceed parsed len) */
    uint32_t used;                 /* live extent = max(rsvmap end, struct end, strings end) */
    uint32_t addr_cells;           /* root #address-cells (1|2) */
    uint32_t size_cells;           /* root #size-cells (1|2) */
    uint32_t node_count;
    uint32_t max_depth;

    uint32_t ram_count;
    je_range ram[JE_FDT_MAX_RAM];
    uint32_t ram_off[JE_FDT_MAX_RAM];

    uint32_t rsv_count;            /* kept: fully inside one RAM range */
    je_range rsv[JE_FDT_MAX_RSV];
    uint32_t rsv_off[JE_FDT_MAX_RSV];
    uint8_t  rsv_nomap[JE_FDT_MAX_RSV]; /* 1 if /reserved-memory child had no-map */
    uint32_t rsv_ignored;          /* reservations fully OUTSIDE all RAM (tolerated, counted) */
    uint32_t rsv_dynamic;          /* /reserved-memory children with `size` but no `reg`
                                    * (MP1 allocates nothing; MP2 allocator must honor) */

    bool     has_initrd;
    je_range initrd;               /* [linux,initrd-start, linux,initrd-end) */
} je_boot_ctx;

/*
 * Copy planning (pure; reads only the 40 header bytes). Checks, in order:
 * hdr != NULL (BAD_ARG), magic (BAD_MAGIC), totalsize > JE_FDT_MAX_TOTAL
 * (TOO_LARGE), totalsize < 40 (BAD_LAYOUT). On OK sets *totalsize and
 * *copy_len = min(totalsize, JE_FDT_MAX_COPY).
 */
je_fdt_status je_fdt_copy_plan(const uint8_t hdr[JE_FDT_HEADER_SIZE],
                               uint32_t *copy_len, uint32_t *totalsize);

/*
 * Parse a COPIED blob of `len` bytes (len may be < totalsize; trailing padding
 * beyond `used` need not be present). Zeroes *out first; on failure leaves it
 * zeroed and sets *err_off to the offset of the first violation (0 for header
 * checks before any offset is meaningful); on success *err_off = 0.
 *
 * Normative rules (both the parser and the tests implement exactly these):
 * HEADER, in this order: blob/out/err_off NULL -> BAD_ARG; len < 40 ->
 *   TRUNCATED; magic -> BAD_MAGIC; totalsize > MAX_TOTAL -> TOO_LARGE;
 *   version < 17 || last_comp_version > 17 -> BAD_VERSION; then layout ->
 *   BAD_LAYOUT unless all hold (overflow-checked, u64 arithmetic):
 *   off_mem_rsvmap >= 40 and 8-aligned; off_dt_struct 4-aligned and
 *   size_dt_struct a multiple of 4; canonical ORDER rsvmap < struct < strings
 *   with struct start >= rsvmap terminator end and strings start >= struct end;
 *   every block end <= totalsize. Zero-size blocks are valid, including one that
 *   starts exactly at totalsize. Any block byte at/after len -> TRUNCATED.
 * RSVMAP: 16-byte big-endian {u64 addr, u64 size} entries from off_mem_rsvmap,
 *   terminated by addr==0 && size==0. > JE_FDT_MAX_RSVMAP entries before the
 *   terminator -> RANGE_LIMIT; terminator not ending <= off_dt_struct ->
 *   BAD_LAYOUT; size==0 && addr!=0 -> BAD_RANGE; addr+size overflow -> BAD_RANGE.
 *   Always 64-bit, independent of cells.
 * STRUCT (single pass, file order, first violation wins): tokens BEGIN_NODE=1,
 *   END_NODE=2, PROP=3, NOP=4, END=9; anything else -> BAD_TOKEN. Leading NOPs
 *   allowed; the first node must be the root with an empty name. Node name: NUL
 *   inside the struct block, <= MAX_NAME bytes, padded to 4. PROP: {u32 len,
 *   u32 nameoff}, value padded to 4, computed in 64-bit. Depth > MAX_DEPTH,
 *   nodes > MAX_NODES, props-in-one-node > MAX_PROPS_PER_NODE -> the *_LIMIT.
 *   After the root's END_NODE only NOPs then END; END must end exactly at
 *   off_dt_struct + size_dt_struct (else BAD_LAYOUT). nameoff may point into the
 *   middle of a string (dtc suffix sharing): require only nameoff <
 *   size_dt_strings and a NUL before the strings-block end.
 * SEMANTICS (cells are NOT inherited; each node's cells govern its children):
 *   - root (depth 1): #address-cells and #size-cells required, len 4, value 1|2.
 *     Properties precede children, so absence is decided when the root's
 *     property list ends: at its first child BEGIN_NODE (or its END_NODE if it
 *     has no children) -> BAD_CELLS, err_off = that token. This precedes any
 *     use of the cells by a child.
 *   - memory: any depth-2 node whose device_type is exactly "memory" (name not
 *     used); skipped if `status` present and not "okay"/"ok"; `reg` required
 *     (absent -> BAD_RANGE) using ROOT cells. A node is classified at its
 *     END_NODE (reg may precede device_type). Multiple memory nodes aggregate.
 *   - /reserved-memory: the depth-2 node named exactly "reserved-memory"; must
 *     have its own #address-cells/#size-cells equal to root's and an empty
 *     `ranges` (else BAD_CELLS). Depth-3 children: `reg` (using those cells)
 *     yields reservation CANDIDATES, collected in walk order; the
 *     (JE_FDT_MAX_RSV+1)-th candidate range -> RANGE_LIMIT immediately (walk-time,
 *     err_off = that `reg` PROP), before any post-walk RAM classification — this
 *     bounds parser scratch without a second pass. Status-disabled children
 *     skipped; `no-map` recorded; `size` without `reg` -> rsv_dynamic++.
 *   - /chosen: the depth-2 node named exactly "chosen". linux,initrd-start/-end
 *     each 4 or 8 bytes (independently); exactly one present, end <= start, or a
 *     bad length -> BAD_RANGE.
 *   - consumed properties: #address-cells, #size-cells, reg, device_type,
 *     status, ranges, no-map, size, linux,initrd-start, linux,initrd-end in the
 *     nodes above; a repeat in the same node -> DUPLICATE. A second "chosen" or
 *     "reserved-memory" depth-2 node -> DUPLICATE.
 * POST-WALK, in this order: ram_count == 0 -> NO_MEMORY; any two RAM ranges
 *   overlapping (adjacent is fine) -> OVERLAP; then each reservation (memreserve
 *   entries first, then /reserved-memory in walk order): fully inside ONE RAM
 *   range -> kept (> MAX_RSV kept -> RANGE_LIMIT); fully disjoint from all RAM ->
 *   rsv_ignored++; otherwise -> BAD_RANGE. Reservations may overlap each other,
 *   the initrd and the DTB (bootloaders reserve those). Then initrd must lie
 *   fully inside one RAM range (else BAD_RANGE).
 * Status-disabled nodes are fully skipped for semantics but still structurally
 * validated and counted.
 */
je_fdt_status je_fdt_parse(const uint8_t *blob, size_t len,
                           je_boot_ctx *out, uint32_t *err_off);

/* Arch facts supplied by the platform glue. */
typedef struct {
    je_range kernel_image;  /* the loaded kernel extent (incl. .bss/stacks/copy buffer) */
    je_range dtb_src;       /* FIRMWARE DTB: [x0, x0 + header totalsize) — conservative full range */
    uint32_t pa_bits;       /* physical address width, 32..52 */
} je_boot_facts;

/* err_idx encoding for je_boot_ctx_check */
#define JE_BOOT_ERR_FACTS     0xffffffffu /* the facts themselves are invalid */
#define JE_BOOT_ERR_KERNEL    0xfffffffeu
#define JE_BOOT_ERR_DTB       0xfffffffdu
#define JE_BOOT_ERR_INITRD    0xfffffffcu
#define JE_BOOT_ERR_RAM(i)    (0x00010000u + (uint32_t)(i))
/* a plain value 0..JE_FDT_MAX_RSV-1 names the conflicting kept reservation */

/*
 * Validate a successfully parsed context against arch facts. In this order:
 * NULL -> BAD_ARG; pa_bits outside 32..52, or kernel_image/dtb_src invalid ->
 * BAD_RANGE (err_idx FACTS); any RAM/kept-reservation/initrd/kernel/dtb end
 * above 2^pa_bits -> BAD_RANGE; kernel_image not fully inside one RAM range ->
 * BAD_RANGE (KERNEL); dtb_src not fully inside one RAM range -> BAD_RANGE (DTB);
 * kernel ∩ dtb_src -> OVERLAP (DTB); kernel ∩ initrd -> OVERLAP (INITRD); kernel
 * ∩ kept reservation i -> OVERLAP (i, lowest i); dtb_src ∩ initrd -> OVERLAP
 * (INITRD). On OK *err_idx = 0.
 */
je_fdt_status je_boot_ctx_check(const je_boot_ctx *ctx, const je_boot_facts *facts,
                                uint32_t *err_idx);

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF) — lets
 * the boot prove the copied DTB matches the committed fixture. */
uint32_t je_fdt_crc32(const uint8_t *data, size_t len);

#endif /* JEIOS_FDT_H */
