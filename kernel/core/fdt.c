/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bounded, freestanding flattened-devicetree parser.
 *
 * The parser deliberately does not use the host C library. Every DTB
 * multi-byte field is read one byte at a time because the ARM64 boot copy is
 * initially Device memory and is compiled with -mstrict-align.
 */
#include <jeios/fdt.h>

enum {
    FDT_BEGIN_NODE = 1u,
    FDT_END_NODE = 2u,
    FDT_PROP = 3u,
    FDT_NOP = 4u,
    FDT_END = 9u
};

enum {
    PROP_ADDR_CELLS = 0u,
    PROP_SIZE_CELLS = 1u,
    PROP_REG = 2u,
    PROP_DEVICE_TYPE = 3u,
    PROP_STATUS = 4u,
    PROP_RANGES = 5u,
    PROP_NO_MAP = 6u,
    PROP_SIZE = 7u,
    PROP_INITRD_START = 8u,
    PROP_INITRD_END = 9u,
    PROP_COUNT = 10u
};

typedef struct {
    uint64_t name_off;
    uint32_t name_len;
    uint32_t prop_count;
    uint16_t seen;
    bool had_child;
    bool tracked;
    bool is_memory;
    bool is_reserved_memory;
    bool is_chosen;
    bool status_enabled;
    bool has_reg;
    uint64_t reg_off;
    uint32_t reg_prop_off;
    uint32_t reg_len;
    bool has_addr_cells;
    bool has_size_cells;
    uint32_t addr_cells;
    uint32_t size_cells;
    bool has_ranges;
    uint32_t ranges_len;
    bool has_size;
    bool has_no_map;
    bool has_initrd_start;
    bool has_initrd_end;
    uint64_t initrd_start;
    uint64_t initrd_end;
    uint32_t initrd_start_prop_off;
    uint32_t initrd_end_prop_off;
} fdt_node_state;

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t off;
    uint8_t nomap;
} fdt_reservation;

static void fdt_zero_node(fdt_node_state *node)
{
    node->name_off = 0u;
    node->name_len = 0u;
    node->prop_count = 0u;
    node->seen = 0u;
    node->had_child = false;
    node->tracked = false;
    node->is_memory = false;
    node->is_reserved_memory = false;
    node->is_chosen = false;
    node->status_enabled = false;
    node->has_reg = false;
    node->reg_off = 0u;
    node->reg_prop_off = 0u;
    node->reg_len = 0u;
    node->has_addr_cells = false;
    node->has_size_cells = false;
    node->addr_cells = 0u;
    node->size_cells = 0u;
    node->has_ranges = false;
    node->ranges_len = 0u;
    node->has_size = false;
    node->has_no_map = false;
    node->has_initrd_start = false;
    node->has_initrd_end = false;
    node->initrd_start = 0u;
    node->initrd_end = 0u;
    node->initrd_start_prop_off = 0u;
    node->initrd_end_prop_off = 0u;
}

static void fdt_zero_context(je_boot_ctx *out)
{
    uint32_t i;

    out->totalsize = 0u;
    out->used = 0u;
    out->addr_cells = 0u;
    out->size_cells = 0u;
    out->node_count = 0u;
    out->max_depth = 0u;
    out->ram_count = 0u;
    out->rsv_count = 0u;
    out->rsv_ignored = 0u;
    out->rsv_dynamic = 0u;
    out->has_initrd = false;
    out->initrd.base = 0u;
    out->initrd.size = 0u;
    for (i = 0u; i < JE_FDT_MAX_RAM; ++i) {
        out->ram[i].base = 0u;
        out->ram[i].size = 0u;
        out->ram_off[i] = 0u;
    }
    for (i = 0u; i < JE_FDT_MAX_RSV; ++i) {
        out->rsv[i].base = 0u;
        out->rsv[i].size = 0u;
        out->rsv_off[i] = 0u;
        out->rsv_nomap[i] = 0u;
    }
}

static uint32_t fdt_be32(const uint8_t *blob, uint64_t off)
{
    return ((uint32_t)blob[(size_t)off] << 24)
         | ((uint32_t)blob[(size_t)(off + 1u)] << 16)
         | ((uint32_t)blob[(size_t)(off + 2u)] << 8)
         | (uint32_t)blob[(size_t)(off + 3u)];
}

static uint64_t fdt_be64(const uint8_t *blob, uint64_t off)
{
    uint64_t value = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        value = (value << 8) | (uint64_t)blob[(size_t)(off + (uint64_t)i)];
    }
    return value;
}

static bool fdt_add_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (left > UINT64_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool fdt_align4(uint64_t value, uint64_t *result)
{
    uint64_t adjusted;

    if (!fdt_add_u64(value, 3u, &adjusted)) return false;
    *result = adjusted & ~UINT64_C(3);
    return true;
}

static bool fdt_range_valid(uint64_t base, uint64_t size, uint64_t *end)
{
    if (size == 0u) return false;
    return fdt_add_u64(base, size, end);
}

static bool fdt_range_contains(je_range outer, je_range inner)
{
    uint64_t outer_end;
    uint64_t inner_end;

    if (!fdt_range_valid(outer.base, outer.size, &outer_end)
        || !fdt_range_valid(inner.base, inner.size, &inner_end)) {
        return false;
    }
    return inner.base >= outer.base && inner_end <= outer_end;
}

static bool fdt_range_overlaps(je_range left, je_range right)
{
    uint64_t left_end;
    uint64_t right_end;

    if (!fdt_range_valid(left.base, left.size, &left_end)
        || !fdt_range_valid(right.base, right.size, &right_end)) {
        return false;
    }
    return left.base < right_end && right.base < left_end;
}

static bool fdt_literal_equal(const uint8_t *blob, uint64_t off,
                              uint32_t length, const char *literal,
                              uint32_t literal_length, bool nul_terminated)
{
    uint32_t i;
    uint32_t expected = literal_length + (nul_terminated ? 1u : 0u);

    if (length != expected) return false;
    for (i = 0u; i < literal_length; ++i) {
        if (blob[(size_t)(off + (uint64_t)i)] != (uint8_t)literal[i]) {
            return false;
        }
    }
    if (nul_terminated && blob[(size_t)(off + (uint64_t)literal_length)] != 0u) {
        return false;
    }
    return true;
}

static uint32_t fdt_property_kind(const uint8_t *blob, uint64_t off,
                                  uint32_t length)
{
    if (fdt_literal_equal(blob, off, length, "#address-cells", 14u, false)) {
        return PROP_ADDR_CELLS;
    }
    if (fdt_literal_equal(blob, off, length, "#size-cells", 11u, false)) {
        return PROP_SIZE_CELLS;
    }
    if (fdt_literal_equal(blob, off, length, "reg", 3u, false)) {
        return PROP_REG;
    }
    if (fdt_literal_equal(blob, off, length, "device_type", 11u, false)) {
        return PROP_DEVICE_TYPE;
    }
    if (fdt_literal_equal(blob, off, length, "status", 6u, false)) {
        return PROP_STATUS;
    }
    if (fdt_literal_equal(blob, off, length, "ranges", 6u, false)) {
        return PROP_RANGES;
    }
    if (fdt_literal_equal(blob, off, length, "no-map", 6u, false)) {
        return PROP_NO_MAP;
    }
    if (fdt_literal_equal(blob, off, length, "size", 4u, false)) {
        return PROP_SIZE;
    }
    if (fdt_literal_equal(blob, off, length, "linux,initrd-start", 18u, false)) {
        return PROP_INITRD_START;
    }
    if (fdt_literal_equal(blob, off, length, "linux,initrd-end", 16u, false)) {
        return PROP_INITRD_END;
    }
    return PROP_COUNT;
}

static bool fdt_node_name_equal(const uint8_t *blob, const fdt_node_state *node,
                                const char *literal, uint32_t literal_length)
{
    return fdt_literal_equal(blob, node->name_off, node->name_len,
                             literal, literal_length, false);
}

static bool fdt_status_ok(const uint8_t *blob, uint64_t off, uint32_t length)
{
    return fdt_literal_equal(blob, off, length, "okay", 4u, true)
        || fdt_literal_equal(blob, off, length, "ok", 2u, true);
}

static bool fdt_consumed_string_valid(const uint8_t *blob, uint64_t off,
                                      uint32_t length)
{
    return length != 0u && blob[(size_t)(off + (uint64_t)length - 1u)] == 0u;
}

static je_fdt_status fdt_validate_cells(const fdt_node_state *node,
                                        uint32_t expected_addr_cells,
                                        uint32_t expected_size_cells,
                                        bool compare_root, uint32_t off,
                                        uint32_t *err)
{
    if (!node->has_addr_cells || !node->has_size_cells
        || (compare_root && node->addr_cells != expected_addr_cells)
        || (compare_root && node->size_cells != expected_size_cells)
        || (node->is_reserved_memory
            && (!node->has_ranges || node->ranges_len != 0u))) {
        *err = off;
        return JE_FDT_BAD_CELLS;
    }
    return JE_FDT_OK;
}

static bool fdt_value_u64(const uint8_t *blob, uint64_t off, uint32_t length,
                          uint64_t *value)
{
    if (length == 4u) {
        *value = (uint64_t)fdt_be32(blob, off);
        return true;
    }
    if (length == 8u) {
        *value = fdt_be64(blob, off);
        return true;
    }
    return false;
}

static bool fdt_string_at(const uint8_t *blob, uint64_t strings_off,
                          uint32_t strings_size, uint32_t nameoff,
                          uint64_t *string_off, uint32_t *string_length)
{
    uint64_t relative = (uint64_t)nameoff;
    uint64_t end = (uint64_t)strings_size;
    uint64_t i;

    if (relative >= end) return false;
    for (i = relative; i < end; ++i) {
        if (blob[(size_t)(strings_off + i)] == 0u) {
            *string_off = strings_off + relative;
            *string_length = (uint32_t)(i - relative);
            return true;
        }
    }
    return false;
}

static je_fdt_status fdt_decode_reg(const uint8_t *blob, uint64_t value_off,
                                    uint32_t value_len, uint32_t addr_cells,
                                    uint32_t size_cells, uint32_t prop_off,
                                    je_boot_ctx *out)
{
    uint64_t tuple_cells = (uint64_t)addr_cells + (uint64_t)size_cells;
    uint64_t tuple_bytes = tuple_cells * 4u;
    uint64_t cursor = value_off;
    uint32_t tuple_count;
    uint32_t tuple;

    if (value_len == 0u || tuple_bytes == 0u
        || (uint64_t)value_len % tuple_bytes != 0u) {
        return JE_FDT_BAD_CELLS;
    }
    tuple_count = (uint32_t)((uint64_t)value_len / tuple_bytes);
    for (tuple = 0u; tuple < tuple_count; ++tuple) {
        uint64_t base = 0u;
        uint64_t size = 0u;
        uint32_t cell;
        uint64_t end;

        for (cell = 0u; cell < addr_cells; ++cell) {
            base = (base << 32) | (uint64_t)fdt_be32(blob, cursor);
            cursor += 4u;
        }
        for (cell = 0u; cell < size_cells; ++cell) {
            size = (size << 32) | (uint64_t)fdt_be32(blob, cursor);
            cursor += 4u;
        }
        if (!fdt_range_valid(base, size, &end)) return JE_FDT_BAD_RANGE;
        if (out->ram_count >= JE_FDT_MAX_RAM) return JE_FDT_RANGE_LIMIT;
        out->ram[out->ram_count].base = base;
        out->ram[out->ram_count].size = size;
        out->ram_off[out->ram_count] = prop_off;
        out->ram_count++;
    }
    return JE_FDT_OK;
}

static je_fdt_status fdt_decode_reservation(const fdt_reservation *reservation,
                                            je_boot_ctx *out)
{
    uint32_t i;
    bool inside = false;

    for (i = 0u; i < out->ram_count; ++i) {
        je_range candidate;
        candidate.base = reservation->base;
        candidate.size = reservation->size;
        if (fdt_range_contains(out->ram[i], candidate)) {
            inside = true;
            break;
        }
        if (fdt_range_overlaps(out->ram[i], candidate)) {
            return JE_FDT_BAD_RANGE;
        }
    }
    if (!inside) {
        out->rsv_ignored++;
        return JE_FDT_OK;
    }
    if (out->rsv_count >= JE_FDT_MAX_RSV) return JE_FDT_RANGE_LIMIT;
    out->rsv[out->rsv_count].base = reservation->base;
    out->rsv[out->rsv_count].size = reservation->size;
    out->rsv_off[out->rsv_count] = reservation->off;
    out->rsv_nomap[out->rsv_count] = reservation->nomap;
    out->rsv_count++;
    return JE_FDT_OK;
}

static je_fdt_status fdt_finish_node(const uint8_t *blob,
                                     fdt_node_state *stack,
                                     uint32_t depth,
                                     uint32_t root_addr_cells,
                                     uint32_t root_size_cells,
                                     je_boot_ctx *out,
                                     fdt_reservation *child_rsv,
                                     uint32_t *child_rsv_count,
                                     uint32_t node_off,
                                     uint32_t *err,
                                     uint32_t *initrd_error)
{
    fdt_node_state *node = &stack[depth - 1u];
    if (depth == 1u) {
        return JE_FDT_OK;
    }

    if (depth == 2u && node->is_reserved_memory) {
        return JE_FDT_OK;
    }

    if (depth == 2u && node->is_chosen) {
        /* Missing halves are structural chosen-node errors; value errors
         * point at the offending initrd property, including post-walk
         * containment failures. */
        if (node->has_initrd_start != node->has_initrd_end) {
            *err = node_off;
            *initrd_error = node_off;
            return JE_FDT_BAD_RANGE;
        }
        if (node->has_initrd_start) {
            if (node->initrd_end <= node->initrd_start) {
                *err = node->initrd_end_prop_off;
                *initrd_error = node->initrd_end_prop_off;
                return JE_FDT_BAD_RANGE;
            }
            out->has_initrd = true;
            out->initrd.base = node->initrd_start;
            out->initrd.size = node->initrd_end - node->initrd_start;
            *initrd_error = node->initrd_end_prop_off;
        }
        return JE_FDT_OK;
    }

    if (depth == 2u && node->is_memory) {
        if (!node->status_enabled) return JE_FDT_OK;
        if (!node->has_reg) {
            *err = node_off;
            return JE_FDT_BAD_RANGE;
        }
        je_fdt_status status = fdt_decode_reg(blob, node->reg_off, node->reg_len,
                                              root_addr_cells, root_size_cells,
                                              node->reg_prop_off, out);
        if (status != JE_FDT_OK) *err = node->reg_prop_off;
        return status;
    }

    if (depth == 3u && stack[depth - 2u].is_reserved_memory) {
        if (!node->status_enabled) return JE_FDT_OK;
        if (!node->has_reg) {
            if (node->has_size) out->rsv_dynamic++;
            return JE_FDT_OK;
        }
        {
            fdt_node_state *parent = &stack[depth - 2u];
            uint64_t tuple_cells = (uint64_t)parent->addr_cells
                                 + (uint64_t)parent->size_cells;
            uint64_t tuple_bytes = tuple_cells * 4u;
            uint64_t cursor = node->reg_off;
            uint32_t tuple_count;
            uint32_t tuple;
            uint32_t cell;

            if (node->reg_len == 0u || tuple_bytes == 0u
                || (uint64_t)node->reg_len % tuple_bytes != 0u) {
                *err = node->reg_prop_off;
                return JE_FDT_BAD_CELLS;
            }
            tuple_count = (uint32_t)((uint64_t)node->reg_len / tuple_bytes);
            for (tuple = 0u; tuple < tuple_count; ++tuple) {
                uint64_t base = 0u;
                uint64_t size = 0u;
                uint64_t end;

                for (cell = 0u; cell < parent->addr_cells; ++cell) {
                    base = (base << 32) | (uint64_t)fdt_be32(blob, cursor);
                    cursor += 4u;
                }
                for (cell = 0u; cell < parent->size_cells; ++cell) {
                    size = (size << 32) | (uint64_t)fdt_be32(blob, cursor);
                    cursor += 4u;
                }
                if (!fdt_range_valid(base, size, &end)) {
                    *err = node->reg_prop_off;
                    return JE_FDT_BAD_RANGE;
                }
                if (*child_rsv_count >= JE_FDT_MAX_RSV) {
                    *err = node->reg_prop_off;
                    return JE_FDT_RANGE_LIMIT;
                }
                child_rsv[*child_rsv_count].base = base;
                child_rsv[*child_rsv_count].size = size;
                child_rsv[*child_rsv_count].off = node->reg_prop_off;
                child_rsv[*child_rsv_count].nomap =
                    node->has_no_map ? 1u : 0u;
                (*child_rsv_count)++;
            }
        }
    }
    return JE_FDT_OK;
}

const char *je_fdt_status_name(je_fdt_status s)
{
    switch (s) {
    case JE_FDT_OK: return "OK";
    case JE_FDT_BAD_ARG: return "BAD_ARG";
    case JE_FDT_TRUNCATED: return "TRUNCATED";
    case JE_FDT_BAD_MAGIC: return "BAD_MAGIC";
    case JE_FDT_TOO_LARGE: return "TOO_LARGE";
    case JE_FDT_BAD_VERSION: return "BAD_VERSION";
    case JE_FDT_BAD_LAYOUT: return "BAD_LAYOUT";
    case JE_FDT_BAD_TOKEN: return "BAD_TOKEN";
    case JE_FDT_BAD_STRING: return "BAD_STRING";
    case JE_FDT_DEPTH_LIMIT: return "DEPTH_LIMIT";
    case JE_FDT_NODE_LIMIT: return "NODE_LIMIT";
    case JE_FDT_PROP_LIMIT: return "PROP_LIMIT";
    case JE_FDT_DUPLICATE: return "DUPLICATE";
    case JE_FDT_BAD_CELLS: return "BAD_CELLS";
    case JE_FDT_NO_MEMORY: return "NO_MEMORY";
    case JE_FDT_RANGE_LIMIT: return "RANGE_LIMIT";
    case JE_FDT_BAD_RANGE: return "BAD_RANGE";
    case JE_FDT_OVERLAP: return "OVERLAP";
    case JE_FDT_CONFLICT: return "CONFLICT";
    case JE_FDT_STATUS_COUNT: return "UNKNOWN";
    default: return "UNKNOWN";
    }
}

je_fdt_status je_fdt_copy_plan(const uint8_t hdr[JE_FDT_HEADER_SIZE],
                               uint32_t *copy_len, uint32_t *totalsize)
{
    uint32_t total;

    if (hdr == NULL || copy_len == NULL || totalsize == NULL) {
        return JE_FDT_BAD_ARG;
    }
    if (fdt_be32(hdr, 0u) != JE_FDT_MAGIC) return JE_FDT_BAD_MAGIC;
    total = fdt_be32(hdr, 4u);
    if (total > JE_FDT_MAX_TOTAL) return JE_FDT_TOO_LARGE;
    if (total < JE_FDT_HEADER_SIZE) return JE_FDT_BAD_LAYOUT;
    *totalsize = total;
    *copy_len = total < JE_FDT_MAX_COPY ? total : JE_FDT_MAX_COPY;
    return JE_FDT_OK;
}

je_fdt_status je_fdt_parse(const uint8_t *blob, size_t len,
                           je_boot_ctx *out, uint32_t *err_off)
{
    uint64_t blob_len;
    uint32_t total;
    uint32_t off_struct32;
    uint32_t off_strings32;
    uint32_t off_rsv32;
    uint32_t version;
    uint32_t last_comp;
    uint32_t strings_size;
    uint32_t struct_size;
    uint64_t off_struct;
    uint64_t off_strings;
    uint64_t off_rsv;
    uint64_t struct_end;
    uint64_t strings_end;
    uint64_t rsv_pos;
    uint64_t rsv_end;
    uint64_t used;
    uint32_t rsv_count = 0u;
    fdt_reservation mem_rsv[JE_FDT_MAX_RSVMAP];
    fdt_reservation child_rsv[JE_FDT_MAX_RSV];
    uint32_t child_rsv_count = 0u;
    fdt_node_state stack[JE_FDT_MAX_DEPTH];
    uint32_t depth = 0u;
    uint32_t node_count = 0u;
    uint32_t max_depth = 0u;
    uint32_t root_addr_cells = 0u;
    uint32_t root_size_cells = 0u;
    bool root_seen = false;
    bool root_closed = false;
    bool chosen_seen = false;
    bool reserved_memory_seen = false;
    bool ended = false;
    uint32_t initrd_error_off = 0u;
    uint64_t pos;
    je_fdt_status status = JE_FDT_OK;
    uint32_t error = 0u;
    uint32_t i;

    if (out != NULL) fdt_zero_context(out);
    if (err_off != NULL) *err_off = 0u;
    if (out == NULL || err_off == NULL || blob == NULL) return JE_FDT_BAD_ARG;
    fdt_zero_context(out);
    *err_off = 0u;
    blob_len = (uint64_t)len;

    if (len < JE_FDT_HEADER_SIZE) return JE_FDT_TRUNCATED;
    if (fdt_be32(blob, 0u) != JE_FDT_MAGIC) return JE_FDT_BAD_MAGIC;
    total = fdt_be32(blob, 4u);
    if (total > JE_FDT_MAX_TOTAL) return JE_FDT_TOO_LARGE;
    version = fdt_be32(blob, 20u);
    last_comp = fdt_be32(blob, 24u);
    if (version < 17u || last_comp > 17u) return JE_FDT_BAD_VERSION;

    off_struct32 = fdt_be32(blob, 8u);
    off_strings32 = fdt_be32(blob, 12u);
    off_rsv32 = fdt_be32(blob, 16u);
    strings_size = fdt_be32(blob, 32u);
    struct_size = fdt_be32(blob, 36u);
    off_struct = (uint64_t)off_struct32;
    off_strings = (uint64_t)off_strings32;
    off_rsv = (uint64_t)off_rsv32;
    if (off_rsv < JE_FDT_HEADER_SIZE || (off_rsv & 7u) != 0u
        || (off_struct & 3u) != 0u || (struct_size & 3u) != 0u
        || off_rsv >= off_struct || off_struct > off_strings
        || !fdt_add_u64(off_struct, (uint64_t)struct_size, &struct_end)
        || !fdt_add_u64(off_strings, (uint64_t)strings_size, &strings_end)
        || struct_end > off_strings || struct_end > (uint64_t)total
        || strings_end > (uint64_t)total) {
        return JE_FDT_BAD_LAYOUT;
    }
    if (off_rsv >= blob_len
        || (struct_size != 0u && struct_end > blob_len)
        || (strings_size != 0u && strings_end > blob_len)) {
        error = (uint32_t)blob_len;
        if (off_rsv >= blob_len) error = off_rsv32;
        else if (struct_size != 0u && struct_end > blob_len) error = off_struct32;
        else if (strings_size != 0u && strings_end > blob_len) error = off_strings32;
        *err_off = error;
        return JE_FDT_TRUNCATED;
    }

    rsv_pos = off_rsv;
    rsv_end = off_rsv;
    for (;;) {
        uint64_t next;
        uint64_t address;
        uint64_t size;

        if (!fdt_add_u64(rsv_pos, 16u, &next) || next > off_struct) {
            *err_off = (uint32_t)rsv_pos;
            return JE_FDT_BAD_LAYOUT;
        }
        if (next > blob_len) {
            *err_off = (uint32_t)rsv_pos;
            return JE_FDT_TRUNCATED;
        }
        address = fdt_be64(blob, rsv_pos);
        size = fdt_be64(blob, rsv_pos + 8u);
        rsv_end = next;
        rsv_pos = next;
        if (address == 0u && size == 0u) break;
        if (rsv_count >= JE_FDT_MAX_RSVMAP) {
            *err_off = (uint32_t)(rsv_pos - 16u);
            return JE_FDT_RANGE_LIMIT;
        }
        if (size == 0u || !fdt_range_valid(address, size, &next)) {
            *err_off = (uint32_t)(rsv_pos - 16u);
            return JE_FDT_BAD_RANGE;
        }
        mem_rsv[rsv_count].base = address;
        mem_rsv[rsv_count].size = size;
        mem_rsv[rsv_count].off = (uint32_t)(rsv_pos - 16u);
        mem_rsv[rsv_count].nomap = 0u;
        rsv_count++;
    }
    if (rsv_end > off_struct) {
        *err_off = (uint32_t)rsv_end;
        return JE_FDT_BAD_LAYOUT;
    }

    used = rsv_end;
    if (struct_end > used) used = struct_end;
    if (strings_end > used) used = strings_end;
    out->totalsize = total;
    out->used = (uint32_t)used;
    pos = off_struct;
    while (pos < struct_end) {
        uint32_t token;
        uint64_t next;

        if (!fdt_add_u64(pos, 4u, &next) || next > struct_end) {
            error = (uint32_t)pos;
            status = JE_FDT_BAD_LAYOUT;
            goto fail;
        }
        token = fdt_be32(blob, pos);
        if (token == FDT_BEGIN_NODE) {
            uint64_t name_start;
            uint64_t name_padded;
            uint64_t scan;
            fdt_node_state *node;

            if (root_closed) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_TOKEN;
                goto fail;
            }
            if (depth == 1u && !stack[0].had_child) {
                status = fdt_validate_cells(&stack[0], 0u, 0u, false,
                                            (uint32_t)pos, &error);
                if (status != JE_FDT_OK) goto fail;
            }
            if (depth == 2u && stack[1].is_reserved_memory
                && !stack[1].had_child) {
                status = fdt_validate_cells(&stack[1], root_addr_cells,
                                            root_size_cells, true,
                                            (uint32_t)pos, &error);
                if (status != JE_FDT_OK) goto fail;
            }
            if (depth >= JE_FDT_MAX_DEPTH) {
                error = (uint32_t)pos;
                status = JE_FDT_DEPTH_LIMIT;
                goto fail;
            }
            if (node_count >= JE_FDT_MAX_NODES) {
                error = (uint32_t)pos;
                status = JE_FDT_NODE_LIMIT;
                goto fail;
            }
            if (depth != 0u) stack[depth - 1u].had_child = true;
            fdt_zero_node(&stack[depth]);
            node = &stack[depth];
            name_start = next;
            scan = name_start;
            while (scan < struct_end && blob[(size_t)scan] != 0u
                   && scan - name_start <= JE_FDT_MAX_NAME) {
                scan++;
            }
            if (scan >= struct_end || blob[(size_t)scan] != 0u
                || scan - name_start > JE_FDT_MAX_NAME) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_STRING;
                goto fail;
            }
            if (!fdt_align4(scan + 1u, &name_padded)
                || name_padded > struct_end) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_LAYOUT;
                goto fail;
            }
            node->name_off = name_start;
            node->name_len = (uint32_t)(scan - name_start);
            node->tracked = depth == 0u || depth == 1u
                         || (depth == 2u && stack[depth - 1u].is_reserved_memory);
            node->status_enabled = true;
            if (!root_seen) {
                if (node->name_len != 0u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_TOKEN;
                    goto fail;
                }
                root_seen = true;
            }
            if (depth == 1u) {
                node->is_reserved_memory =
                    fdt_node_name_equal(blob, node, "reserved-memory", 15u);
                node->is_chosen = fdt_node_name_equal(blob, node, "chosen", 6u);
                if (node->is_reserved_memory) {
                    if (reserved_memory_seen) {
                        error = (uint32_t)pos;
                        status = JE_FDT_DUPLICATE;
                        goto fail;
                    }
                    reserved_memory_seen = true;
                }
                if (node->is_chosen) {
                    if (chosen_seen) {
                        error = (uint32_t)pos;
                        status = JE_FDT_DUPLICATE;
                        goto fail;
                    }
                    chosen_seen = true;
                }
            }
            depth++;
            node_count++;
            if (depth > max_depth) max_depth = depth;
            pos = name_padded;
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0u) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_TOKEN;
                goto fail;
            }
            if (depth == 1u && !stack[0].had_child) {
                status = fdt_validate_cells(&stack[0], 0u, 0u, false,
                                            (uint32_t)pos, &error);
                if (status != JE_FDT_OK) goto fail;
            }
            if (depth == 2u && stack[1].is_reserved_memory
                && !stack[1].had_child) {
                status = fdt_validate_cells(&stack[1], root_addr_cells,
                                            root_size_cells, true,
                                            (uint32_t)pos, &error);
                if (status != JE_FDT_OK) goto fail;
            }
            status = fdt_finish_node(blob, stack, depth, root_addr_cells,
                                     root_size_cells, out, child_rsv,
                                     &child_rsv_count, (uint32_t)pos, &error,
                                     &initrd_error_off);
            if (status != JE_FDT_OK) goto fail;
            depth--;
            if (depth == 0u) root_closed = true;
            pos = next;
            continue;
        }
        if (token == FDT_PROP) {
            uint32_t value_len;
            uint32_t nameoff;
            uint64_t value_off;
            uint64_t value_end;
            uint64_t property_end;
            uint64_t name_off;
            uint32_t name_len;
            uint32_t kind;
            bool first_kind = false;
            fdt_node_state *node;

            if (depth == 0u || root_closed) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_TOKEN;
                goto fail;
            }
            node = &stack[depth - 1u];
            if (node->had_child) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_TOKEN;
                goto fail;
            }
            if (!fdt_add_u64(pos, 12u, &value_off)
                || value_off > struct_end) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_LAYOUT;
                goto fail;
            }
            value_len = fdt_be32(blob, pos + 4u);
            nameoff = fdt_be32(blob, pos + 8u);
            if (!fdt_add_u64(value_off, (uint64_t)value_len, &value_end)
                || !fdt_align4(value_end, &property_end)
                || property_end > struct_end) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_LAYOUT;
                goto fail;
            }
            if (!fdt_string_at(blob, off_strings, strings_size, nameoff,
                               &name_off, &name_len)) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_STRING;
                goto fail;
            }
            if (node->prop_count >= JE_FDT_MAX_PROPS_PER_NODE) {
                error = (uint32_t)pos;
                status = JE_FDT_PROP_LIMIT;
                goto fail;
            }
            node->prop_count++;
            kind = fdt_property_kind(blob, name_off, name_len);
            if (kind < PROP_COUNT) {
                uint16_t bit = (uint16_t)(1u << kind);
                first_kind = true;
                if (node->tracked) {
                    if ((node->seen & bit) != 0u) {
                        error = (uint32_t)pos;
                        status = JE_FDT_DUPLICATE;
                        goto fail;
                    }
                    node->seen |= bit;
                }
            }
            if ((kind == PROP_STATUS || kind == PROP_DEVICE_TYPE)
                && !fdt_consumed_string_valid(blob, value_off, value_len)) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_STRING;
                goto fail;
            }
            if (kind == PROP_ADDR_CELLS && depth == 1u && first_kind) {
                if (value_len != 4u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
                node->has_addr_cells = true;
                node->addr_cells = fdt_be32(blob, value_off);
                root_addr_cells = node->addr_cells;
                if (node->addr_cells < 1u || node->addr_cells > 2u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
            } else if (kind == PROP_SIZE_CELLS && depth == 1u && first_kind) {
                if (value_len != 4u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
                node->has_size_cells = true;
                node->size_cells = fdt_be32(blob, value_off);
                root_size_cells = node->size_cells;
                if (node->size_cells < 1u || node->size_cells > 2u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
            } else if (kind == PROP_ADDR_CELLS && depth == 2u
                       && node->is_reserved_memory && first_kind) {
                if (value_len != 4u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
                node->has_addr_cells = true;
                node->addr_cells = fdt_be32(blob, value_off);
                if (node->addr_cells < 1u || node->addr_cells > 2u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
            } else if (kind == PROP_SIZE_CELLS && depth == 2u
                       && node->is_reserved_memory && first_kind) {
                if (value_len != 4u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
                node->has_size_cells = true;
                node->size_cells = fdt_be32(blob, value_off);
                if (node->size_cells < 1u || node->size_cells > 2u) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_CELLS;
                    goto fail;
                }
            } else if (kind == PROP_DEVICE_TYPE && depth == 2u
                       && first_kind) {
                if ((node->is_chosen || node->is_reserved_memory)
                    && fdt_literal_equal(blob, value_off, value_len,
                                         "memory", 6u, true)) {
                    error = (uint32_t)pos;
                    status = JE_FDT_CONFLICT;
                    goto fail;
                }
                node->is_memory =
                    fdt_literal_equal(blob, value_off, value_len,
                                      "memory", 6u, true);
            } else if (kind == PROP_STATUS && first_kind) {
                node->status_enabled = fdt_status_ok(blob, value_off, value_len);
            } else if (kind == PROP_REG && first_kind) {
                node->has_reg = true;
                node->reg_off = value_off;
                node->reg_prop_off = (uint32_t)pos;
                node->reg_len = value_len;
            } else if (kind == PROP_RANGES && depth == 2u
                       && node->is_reserved_memory && first_kind) {
                node->has_ranges = true;
                node->ranges_len = value_len;
            } else if (kind == PROP_NO_MAP && depth == 3u
                       && stack[depth - 2u].is_reserved_memory) {
                node->has_no_map = true;
            } else if (kind == PROP_SIZE && depth == 3u
                       && stack[depth - 2u].is_reserved_memory) {
                node->has_size = true;
            } else if (kind == PROP_INITRD_START && depth == 2u
                       && node->is_chosen && first_kind) {
                if (!fdt_value_u64(blob, value_off, value_len,
                                   &node->initrd_start)) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_RANGE;
                    goto fail;
                }
                node->has_initrd_start = true;
                node->initrd_start_prop_off = (uint32_t)pos;
            } else if (kind == PROP_INITRD_END && depth == 2u
                       && node->is_chosen && first_kind) {
                if (!fdt_value_u64(blob, value_off, value_len,
                                   &node->initrd_end)) {
                    error = (uint32_t)pos;
                    status = JE_FDT_BAD_RANGE;
                    goto fail;
                }
                node->has_initrd_end = true;
                node->initrd_end_prop_off = (uint32_t)pos;
            }
            pos = property_end;
            continue;
        }
        if (token == FDT_NOP) {
            pos = next;
            continue;
        }
        if (token == FDT_END) {
            if (depth != 0u || !root_closed) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_TOKEN;
                goto fail;
            }
            if (next != struct_end) {
                error = (uint32_t)pos;
                status = JE_FDT_BAD_LAYOUT;
                goto fail;
            }
            ended = true;
            pos = next;
            continue;
        }
        error = (uint32_t)pos;
        status = JE_FDT_BAD_TOKEN;
        goto fail;
    }
    if (!ended) {
        error = (uint32_t)pos;
        status = JE_FDT_BAD_LAYOUT;
        goto fail;
    }
    if (depth != 0u || !root_seen || !root_closed) {
        error = (uint32_t)pos;
        status = JE_FDT_BAD_TOKEN;
        goto fail;
    }
    out->node_count = node_count;
    out->max_depth = max_depth;
    out->addr_cells = root_addr_cells;
    out->size_cells = root_size_cells;
    if (out->ram_count == 0u) {
        error = (uint32_t)off_struct;
        status = JE_FDT_NO_MEMORY;
        goto fail;
    }
    for (i = 0u; i < out->ram_count; ++i) {
        uint32_t j;
        for (j = i + 1u; j < out->ram_count; ++j) {
            if (fdt_range_overlaps(out->ram[i], out->ram[j])) {
                error = out->ram_off[j];
                status = JE_FDT_OVERLAP;
                goto fail;
            }
        }
    }
    for (i = 0u; i < rsv_count; ++i) {
        status = fdt_decode_reservation(&mem_rsv[i], out);
        if (status != JE_FDT_OK) {
            error = mem_rsv[i].off;
            goto fail;
        }
    }
    for (i = 0u; i < child_rsv_count; ++i) {
        status = fdt_decode_reservation(&child_rsv[i], out);
        if (status != JE_FDT_OK) {
            error = child_rsv[i].off;
            goto fail;
        }
    }
    if (out->has_initrd) {
        if (!fdt_range_valid(out->initrd.base, out->initrd.size, &struct_end)) {
            error = initrd_error_off;
            status = JE_FDT_BAD_RANGE;
            goto fail;
        }
        {
            bool contained = false;
            for (i = 0u; i < out->ram_count; ++i) {
                if (fdt_range_contains(out->ram[i], out->initrd)) {
                    contained = true;
                    break;
                }
            }
            if (!contained) {
                error = initrd_error_off;
                status = JE_FDT_BAD_RANGE;
                goto fail;
            }
        }
    }
    *err_off = 0u;
    return JE_FDT_OK;

fail:
    fdt_zero_context(out);
    *err_off = error;
    return status;
}

je_fdt_status je_boot_ctx_check(const je_boot_ctx *ctx, const je_boot_facts *facts,
                                uint32_t *err_idx)
{
    uint64_t limit;
    uint64_t end;
    uint32_t i;

    if (ctx == NULL || facts == NULL || err_idx == NULL) return JE_FDT_BAD_ARG;
    *err_idx = 0u;
    if (ctx->ram_count > JE_FDT_MAX_RAM
        || ctx->rsv_count > JE_FDT_MAX_RSV) {
        *err_idx = JE_BOOT_ERR_FACTS;
        return JE_FDT_BAD_ARG;
    }
    if (facts->pa_bits < 32u || facts->pa_bits > 52u
        || !fdt_range_valid(facts->kernel_image.base,
                            facts->kernel_image.size, &end)
        || !fdt_range_valid(facts->dtb_src.base, facts->dtb_src.size, &end)) {
        *err_idx = JE_BOOT_ERR_FACTS;
        return JE_FDT_BAD_RANGE;
    }
    limit = UINT64_C(1) << facts->pa_bits;
    for (i = 0u; i < ctx->ram_count; ++i) {
        if (ctx->ram[i].size == 0u
            || !fdt_range_valid(ctx->ram[i].base, ctx->ram[i].size, &end)
            || end > limit) {
            *err_idx = JE_BOOT_ERR_RAM(i);
            return JE_FDT_BAD_RANGE;
        }
    }
    for (i = 0u; i < ctx->rsv_count; ++i) {
        if (!fdt_range_valid(ctx->rsv[i].base, ctx->rsv[i].size, &end)
            || end > limit) {
            *err_idx = i;
            return JE_FDT_BAD_RANGE;
        }
    }
    if (ctx->has_initrd
        && (!fdt_range_valid(ctx->initrd.base, ctx->initrd.size, &end)
            || end > limit)) {
        *err_idx = JE_BOOT_ERR_INITRD;
        return JE_FDT_BAD_RANGE;
    }
    if (!fdt_range_valid(facts->kernel_image.base, facts->kernel_image.size, &end)
        || end > limit) {
        *err_idx = JE_BOOT_ERR_KERNEL;
        return JE_FDT_BAD_RANGE;
    }
    if (!fdt_range_valid(facts->dtb_src.base, facts->dtb_src.size, &end)
        || end > limit) {
        *err_idx = JE_BOOT_ERR_DTB;
        return JE_FDT_BAD_RANGE;
    }
    for (i = 0u; i < ctx->ram_count; ++i) {
        if (fdt_range_contains(ctx->ram[i], facts->kernel_image)) break;
    }
    if (i == ctx->ram_count) {
        *err_idx = JE_BOOT_ERR_KERNEL;
        return JE_FDT_BAD_RANGE;
    }
    for (i = 0u; i < ctx->ram_count; ++i) {
        if (fdt_range_contains(ctx->ram[i], facts->dtb_src)) break;
    }
    if (i == ctx->ram_count) {
        *err_idx = JE_BOOT_ERR_DTB;
        return JE_FDT_BAD_RANGE;
    }
    if (fdt_range_overlaps(facts->kernel_image, facts->dtb_src)) {
        *err_idx = JE_BOOT_ERR_DTB;
        return JE_FDT_OVERLAP;
    }
    if (ctx->has_initrd
        && fdt_range_overlaps(facts->kernel_image, ctx->initrd)) {
        *err_idx = JE_BOOT_ERR_INITRD;
        return JE_FDT_OVERLAP;
    }
    for (i = 0u; i < ctx->rsv_count; ++i) {
        if (fdt_range_overlaps(facts->kernel_image, ctx->rsv[i])) {
            *err_idx = i;
            return JE_FDT_OVERLAP;
        }
    }
    if (ctx->has_initrd
        && fdt_range_overlaps(facts->dtb_src, ctx->initrd)) {
        *err_idx = JE_BOOT_ERR_INITRD;
        return JE_FDT_OVERLAP;
    }
    *err_idx = 0u;
    return JE_FDT_OK;
}

uint32_t je_fdt_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    size_t i;

    if (data == NULL && len != 0u) return 0u;
    for (i = 0u; i < len; ++i) {
        uint32_t bit;
        crc ^= (uint32_t)data[i];
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 1u) != 0u) crc = (crc >> 1) ^ UINT32_C(0xedb88320);
            else crc >>= 1;
        }
    }
    return crc ^ UINT32_MAX;
}
