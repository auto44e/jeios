/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>
#include <jeios/broker_test.h>

static const char broker_service_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS broker service";
static volatile uint32_t broker_service_data
    __attribute__((used, section(".data"))) = UINT32_C(0x31415926);

static uint32_t recv_control(uint64_t control, void *message, uint64_t cap,
                             uint32_t *hbuf, uint64_t hcap)
{
    uint64_t meta = 0u;
    uint64_t status = je_try_recv_caps(control, message, cap, hbuf, hcap,
                                       0u, &meta);
    if (status == JE_ABI_H_EMPTY) return 0u;
    if (status == JE_ABI_H_PEER_CLOSED) return UINT32_MAX;
    if (status != JE_ABI_OK || (meta & UINT64_C(0xffffffff)) == 0u
        || (meta >> 32) > hcap)
        return UINT32_MAX - 1u;
    return 1u;
}

static uint32_t service_echo(uint32_t *handles, uint32_t count)
{
    for (uint32_t i = 0u; i < count; ++i) {
        je_broker_test_echo message = {0};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv_caps(handles[i], &message,
                                           sizeof message, (uint32_t *)0, 0u,
                                           0u, &meta);
        if (status == JE_ABI_H_EMPTY) continue;
        if (status == JE_ABI_H_PEER_CLOSED) {
            (void)je_handle_close(handles[i]);
            handles[i] = 0u;
            continue;
        }
        if (status != JE_ABI_OK
            || (meta & UINT64_C(0xffffffff)) != sizeof message
            || (meta >> 32) != 0u
            || message.op != JE_BROKER_TEST_ECHO_REQUEST)
            return UINT32_MAX;
        message.op = JE_BROKER_TEST_ECHO_RESPONSE;
        if (je_try_send(handles[i], &message, sizeof message) != JE_ABI_OK)
            return UINT32_MAX;
    }
    return count;
}

int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    uint32_t broker_handles[JE_BROKER_MAX] = {0u};
    uint32_t broker_generations[JE_BROKER_MAX] = {0u};
    uint32_t broker_count = 0u;
    uint8_t control_bytes[sizeof(je_svc_config)] = {0u};
    (void)broker_service_rodata;
    (void)broker_service_data;
    if (version != JE_ABI_STARTUP_V1 || n != 1u
        || h1 != 0u || h2 != 0u || h3 != 0u)
        return 61;

    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };
    if (je_try_send(h0, &ready, sizeof ready) != JE_ABI_OK) return 62;

    for (;;) {
        je_svc_service_bind bind = {0};
        uint32_t minted[1] = {0u};
        uint32_t control = recv_control(h0, control_bytes, sizeof control_bytes,
                                        minted, 1u);
        if (control == UINT32_MAX) return 63;
        if (control == UINT32_MAX - 1u) return 64;
        if (control == 1u) {
            memcpy(&bind, control_bytes, sizeof bind);
            if (bind.op == JE_SVC_OP_SERVICE_BIND
                && minted[0] != 0u
                && broker_count < JE_BROKER_MAX) {
                broker_handles[broker_count] = minted[0];
                broker_generations[broker_count] = bind.service_generation;
                ++broker_count;
                je_svc_broker_reply ack = { JE_SVC_OP_BROKER_ACK, 0u };
                if (je_try_send(h0, &ack, sizeof ack) != JE_ABI_OK)
                    return 65;
            } else if (bind.op == JE_SVC_OP_CONFIG) {
                je_svc_config config = {0};
                memcpy(&config, control_bytes, sizeof config);
                if (config.incarnation < config.crash_before)
                    __asm__ volatile("ud2");
                return 66;
            } else if (bind.op == JE_SVC_OP_SERVICE_BIND) {
                return 66;
            } else {
                return 66;
            }
        }

        uint32_t echo_status = service_echo(broker_handles, broker_count);
        if (echo_status == UINT32_MAX) return 67;
        for (uint32_t i = 0u; i < broker_count; ++i) {
            if (broker_handles[i] == 0u) {
                for (uint32_t j = i + 1u; j < broker_count; ++j) {
                    broker_handles[j - 1u] = broker_handles[j];
                    broker_generations[j - 1u] = broker_generations[j];
                }
                broker_handles[broker_count - 1u] = 0u;
                broker_generations[broker_count - 1u] = 0u;
                --broker_count;
                --i;
            }
        }
        (void)broker_generations;
        if (je_yield() != JE_ABI_OK) return 68;
    }
}
