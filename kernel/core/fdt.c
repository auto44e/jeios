/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PRE-WIRING STUB (issue #52). Contract: include/jeios/fdt.h. The real parser is
 * written by a separate worker against that header; this stub only lets the
 * build, the hosted test target and the ARM link exist before it lands. Every
 * entry point fails closed.
 */
#include <jeios/fdt.h>

const char *je_fdt_status_name(je_fdt_status s)
{
    (void)s;
    return "UNKNOWN";
}

je_fdt_status je_fdt_copy_plan(const uint8_t hdr[JE_FDT_HEADER_SIZE],
                               uint32_t *copy_len, uint32_t *totalsize)
{
    (void)hdr; (void)copy_len; (void)totalsize;
    return JE_FDT_BAD_ARG;
}

je_fdt_status je_fdt_parse(const uint8_t *blob, size_t len,
                           je_boot_ctx *out, uint32_t *err_off)
{
    (void)blob; (void)len; (void)out; (void)err_off;
    return JE_FDT_BAD_ARG;
}

je_fdt_status je_boot_ctx_check(const je_boot_ctx *ctx, const je_boot_facts *facts,
                                uint32_t *err_idx)
{
    (void)ctx; (void)facts; (void)err_idx;
    return JE_FDT_BAD_ARG;
}

uint32_t je_fdt_crc32(const uint8_t *data, size_t len)
{
    (void)data; (void)len;
    return 0u;
}
