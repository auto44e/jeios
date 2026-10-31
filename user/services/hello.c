/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>

static volatile unsigned char hello_bss[32u];
static uint32_t hello_data = UINT32_C(0x13579bdf);
static const char hello_text[] = "JeiOS M4e ring-3 C image";

/*
 * Failure codes are intentionally distinct so a PROC_STATUS exit code identifies
 * which image-boundary check failed:
 * 11 startup version, 12 grant count, 13 BSS, 14 DATA initializer/write,
 * 15 RODATA, 16 CLOCK_TICKS, 17 READY send.
 */
int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    static const uint32_t data_initial = UINT32_C(0x13579bdf);
    static const uint32_t data_written = UINT32_C(0x2468ace0);
    static const char text_expected[] = "JeiOS M4e ring-3 C image";
    uint64_t ticks = 0u;
    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };

    if (version != 1u) return 11;
    if (n != 1u || h1 != 0u || h2 != 0u || h3 != 0u) return 12;
    for (unsigned long i = 0ul; i < sizeof hello_bss; ++i)
        if (hello_bss[i] != 0u) return 13;
    if (hello_data != data_initial) return 14;
    hello_data = data_written;
    if (hello_data != data_written) return 14;
    if (memcmp(hello_text, text_expected, sizeof hello_text) != 0) return 15;
    if (je_clock_ticks(&ticks) != JE_ABI_OK) return 16;
    if (je_try_send(h0, &ready, sizeof ready) != JE_ABI_OK) return 17;
    return 0;
}
