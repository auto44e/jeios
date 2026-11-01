/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>

static const char restart_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS restart";
static volatile uint32_t restart_data __attribute__((used, section(".data")))
    = UINT32_C(0x13579bdf);

/* The restart child needs RECV as well as SEND: its one endpoint is also the
 * supervisor's bounded CONFIG control path. No second grant is available in
 * the fixed D11 scenario. */
int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    (void)restart_rodata;
    (void)restart_data;
    je_svc_config config = {0};
    uint64_t meta = 0u;
    if (version != JE_ABI_STARTUP_V1 || n != 1u
        || h1 != 0u || h2 != 0u || h3 != 0u)
        return 31;
    for (;;) {
        uint64_t status = je_try_recv(h0, &config, sizeof config, 100u, &meta);
        if (status == JE_ABI_H_TIMED_OUT) continue;
        if (status != JE_ABI_OK || (meta & UINT64_C(0xffffffff)) != JE_SVC_CONFIG_LEN
            || (meta >> 32) != 0u || config.op != JE_SVC_OP_CONFIG
            || config.reserved != 0u)
            return 32;
        if (config.incarnation < config.crash_before)
            __asm__ volatile("ud2");
        break;
    }
    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };
    if (je_try_send(h0, &ready, sizeof ready) != JE_ABI_OK) return 33;
    for (;;) (void)je_yield();
}
