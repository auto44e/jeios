/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>

static const char survivor_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS survivor";
static volatile uint32_t survivor_data __attribute__((used, section(".data")))
    = UINT32_C(0x2468ace0);

int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    (void)survivor_rodata;
    (void)survivor_data;
    uint32_t sequence = 0u;
    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };
    je_svc_heartbeat heartbeat = { JE_SVC_OP_HEARTBEAT, 0u };
    if (version != JE_ABI_STARTUP_V1 || n != 1u
        || h1 != 0u || h2 != 0u || h3 != 0u)
        return 41;
    if (je_try_send(h0, &ready, sizeof ready) != JE_ABI_OK) return 42;
    for (;;) {
        heartbeat.seq = ++sequence;
        uint64_t status = je_try_send(h0, &heartbeat, sizeof heartbeat);
        if (status != JE_ABI_OK && status != JE_ABI_H_QUEUE_FULL)
            return 43;
        (void)je_yield();
    }
}
