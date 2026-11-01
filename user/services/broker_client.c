/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>
#include <jeios/broker_test.h>

static const char broker_client_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS broker client";
static volatile uint32_t broker_client_data
    __attribute__((used, section(".data"))) = UINT32_C(0x27182818);

static uint32_t wait_control(uint64_t control, void *message, uint64_t cap,
                             uint32_t *hbuf, uint64_t hcap,
                             uint32_t expected_length,
                             uint32_t expected_caps)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return UINT32_MAX - 1u;
    for (;;) {
        uint64_t meta = 0u;
        uint64_t status = je_try_recv_caps(control, message, cap, hbuf, hcap,
                                           0u, &meta);
        if (status == JE_ABI_OK) {
            if ((meta & UINT64_C(0xffffffff)) != expected_length
                || (meta >> 32) != expected_caps) {
                if (hbuf && hcap != 0u)
                    for (uint32_t i = 0u; i < hcap; ++i)
                        if (hbuf[i] != 0u) (void)je_handle_close(hbuf[i]);
                (void)je_handle_close(control);
                return UINT32_MAX - 1u;
            }
            return 1u;
        }
        if (status == JE_ABI_H_PEER_CLOSED) return UINT32_MAX;
        if (status != JE_ABI_H_EMPTY) {
            (void)je_handle_close(control);
            return UINT32_MAX - 1u;
        }
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_BROKER_CLIENT_WAIT_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return UINT32_MAX - 1u;
    }
}

static uint32_t exercise(uint32_t broker, uint32_t *bits, uint32_t sequence)
{
    uint64_t denied_cap = (uint64_t)broker
        | ((uint64_t)(JE_ABI_RIGHT_SEND | JE_ABI_RIGHT_RECV) << 32);
    je_broker_test_echo message = {
        JE_BROKER_TEST_ECHO_REQUEST, sequence
    };
    if (je_try_send_caps(broker, &message, sizeof message,
                         &denied_cap, 1u) != JE_ABI_H_DENIED)
        return 0u;
    *bits |= JE_BROKER_TEST_BIT_NO_TRANSFER;
    if (je_try_send(broker, &message, sizeof message) != JE_ABI_OK)
        return 0u;
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_broker_test_echo response = {0};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv(broker, &response, sizeof response,
                                      0u, &meta);
        if (status == JE_ABI_OK) {
            if ((meta & UINT64_C(0xffffffff)) != sizeof response
                || (meta >> 32) != 0u
                || response.op != JE_BROKER_TEST_ECHO_RESPONSE
                || response.sequence != sequence)
                return 0u;
            *bits |= JE_BROKER_TEST_BIT_ECHO;
            return 1u;
        }
        if (status == JE_ABI_H_PEER_CLOSED) return 0u;
        if (status != JE_ABI_H_EMPTY) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_BROKER_CLIENT_WAIT_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

static uint32_t request(uint64_t control, uint32_t *broker,
                        uint32_t *generation, uint32_t *bits,
                        uint32_t expected_op)
{
    je_svc_broker_request request_message = {
        JE_SVC_OP_BROKER_REQUEST, JE_SVC_BROKER_SERVICE_ID
    };
    if (je_try_send(control, &request_message, sizeof request_message)
        != JE_ABI_OK)
        return 0u;

    je_svc_broker_grant grant = {0};
    uint32_t received[1] = {0u};
    uint32_t result = wait_control(control, &grant, sizeof grant,
                                   received, 1u,
                                   JE_SVC_BROKER_GRANT_LEN, 1u);
    if (result != 1u || (grant.op != expected_op) || received[0] == 0u) {
        if (received[0] != 0u) (void)je_handle_close(received[0]);
        (void)je_handle_close(control);
        return 0u;
    }
    *broker = received[0];
    *generation = grant.service_generation;
    je_svc_broker_reply ack = { JE_SVC_OP_BROKER_ACK, grant.transaction };
    if (je_try_send(control, &ack, sizeof ack) != JE_ABI_OK)
        return 0u;
    return exercise(*broker, bits, *generation);
}

static uint32_t report_exercised(uint64_t control)
{
    je_svc_config message = {
        JE_SVC_OP_CONFIG, JE_BROKER_TEST_EXERCISED, 0u, 0u
    };
    return je_try_send(control, &message, sizeof message) == JE_ABI_OK;
}

static uint32_t observe_peer_closed(uint32_t broker, uint32_t *bits)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        uint64_t meta = 0u;
        uint64_t status = je_try_recv(broker, (void *)0, 0u, 0u, &meta);
        if (status == JE_ABI_H_PEER_CLOSED) {
            *bits |= JE_BROKER_TEST_BIT_PEER_CLOSED;
            (void)je_handle_close(broker);
            return 1u;
        }
        if (status != JE_ABI_H_EMPTY) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_SCENARIO_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

int64_t je_main(uint64_t version, uint64_t n, uint64_t h0, uint64_t h1,
                uint64_t h2, uint64_t h3)
{
    uint32_t bits = 0u;
    uint32_t role = JE_BROKER_TEST_ROLE_A;
    uint32_t broker = 0u;
    uint32_t generation = 0u;
    (void)broker_client_rodata;
    (void)broker_client_data;
    if (version != JE_ABI_STARTUP_V1 || n != 1u
        || h1 != 0u || h2 != 0u || h3 != 0u)
        return 71;
    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };
    if (je_try_send(h0, &ready, sizeof ready) != JE_ABI_OK) return 72;
    bits |= JE_BROKER_TEST_BIT_READY;

    je_svc_config role_message = {0};
    if (wait_control(h0, &role_message, sizeof role_message,
                     (uint32_t *)0, 0u, JE_SVC_CONFIG_LEN, 0u) != 1u
        || role_message.op != JE_SVC_OP_CONFIG
        || role_message.crash_before != 0u
        || role_message.reserved != 0u)
        return 73;
    role = role_message.incarnation;
    if (role == JE_BROKER_TEST_ROLE_B) bits |= JE_BROKER_TEST_BIT_ROLE_B;
    if (role != JE_BROKER_TEST_ROLE_A && role != JE_BROKER_TEST_ROLE_B)
        return 74;

    if (!request(h0, &broker, &generation, &bits, JE_SVC_OP_BROKER_GRANT))
        return (int64_t)bits;
    bits |= JE_BROKER_TEST_BIT_GRANTS;
    if (!report_exercised(h0)) return (int64_t)bits;
    if (role == JE_BROKER_TEST_ROLE_B) {
        for (;;) {
            je_svc_config finish = {0};
            uint32_t received = wait_control(h0, &finish, sizeof finish,
                                             (uint32_t *)0, 0u,
                                             JE_SVC_CONFIG_LEN, 0u);
            if (received == UINT32_MAX) return (int64_t)bits;
            if (received == 1u && finish.op == JE_SVC_OP_CONFIG
                && finish.incarnation == JE_BROKER_TEST_FINISH)
                return (int64_t)bits;
            if (received != 0u) return (int64_t)bits;
        }
    }

    if (!observe_peer_closed(broker, &bits))
        return (int64_t)bits;
    if (!request(h0, &broker, &generation, &bits,
                 JE_SVC_OP_BROKER_RESTARTED))
        return (int64_t)bits;
    if (generation != 2u) return (int64_t)bits;
    bits |= JE_BROKER_TEST_BIT_RESTARTED;

    je_svc_broker_request exhausted_request = {
        JE_SVC_OP_BROKER_REQUEST, JE_SVC_BROKER_SERVICE_ID
    };
    if (je_try_send(h0, &exhausted_request, sizeof exhausted_request)
        != JE_ABI_OK)
        return (int64_t)bits;
    je_svc_broker_reply exhausted = {0};
    uint32_t no_caps[1] = {0u};
    uint32_t received = wait_control(h0, &exhausted, sizeof exhausted,
                                     no_caps, 1u,
                                     JE_SVC_BROKER_REPLY_LEN, 0u);
    if (received != 1u || exhausted.op != JE_SVC_OP_BROKER_EXHAUSTED
        || no_caps[0] != 0u)
        return (int64_t)bits;
    bits |= JE_BROKER_TEST_BIT_EXHAUSTED;
    return (int64_t)bits;
}
