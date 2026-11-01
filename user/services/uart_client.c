/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>
#include <jeios/uart_test.h>

static const char uart_client_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS UART broker client";
static volatile uint32_t uart_client_data
    __attribute__((used, section(".data"))) = 0u;

static uint32_t recv_control(uint64_t control, void *buffer, uint32_t capacity,
                             uint32_t *minted, uint32_t *length,
                             uint32_t *caps)
{
    uint64_t meta = 0u;
    uint64_t status = je_try_recv_caps(control, buffer, capacity, minted, 1u,
                                       0u, &meta);
    if (length) *length = (uint32_t)meta;
    if (caps) *caps = (uint32_t)(meta >> 32);
    if (status == JE_ABI_H_EMPTY) return 0u;
    if (status == JE_ABI_H_PEER_CLOSED) return UINT32_MAX;
    if (status != JE_ABI_OK) return UINT32_MAX - 1u;
    return 1u;
}

static uint32_t wait_role(uint64_t control, uint32_t *out_role)
{
    if (!out_role) return 0u;
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_svc_config config = {0};
        uint32_t minted[1] = {0u};
        uint32_t length = 0u;
        uint32_t caps = 0u;
        uint32_t result = recv_control(control, &config, sizeof config, minted,
                                       &length, &caps);
        if (result == 1u) {
            if (caps != 0u || length != JE_SVC_CONFIG_LEN
                || config.op != JE_SVC_OP_CONFIG
                || config.crash_before != 0u || config.reserved != 0u)
                return 0u;
            if (config.incarnation != JE_UART_CLIENT_NORMAL
                && config.incarnation != JE_UART_CLIENT_HARDENED)
                return 0u;
            *out_role = config.incarnation;
            return 1u;
        }
        if (result != 0u) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

static uint32_t wait_command(uint64_t control, uint32_t expected)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_svc_config config = {0};
        uint32_t minted[1] = {0u};
        uint32_t length = 0u;
        uint32_t caps = 0u;
        uint32_t result = recv_control(control, &config, sizeof config, minted,
                                       &length, &caps);
        if (result == 1u) {
            if (caps != 0u || length != JE_SVC_CONFIG_LEN
                || config.op != JE_SVC_OP_CONFIG
                || config.incarnation != expected || config.crash_before != 0u
                || config.reserved != 0u)
                return 0u;
            return 1u;
        }
        if (result != 0u) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

static uint32_t wait_grant(uint64_t control, uint32_t expected_op,
                           uint32_t *out_channel)
{
    if (!out_channel) return 0u;
    *out_channel = 0u;
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_svc_broker_grant grant = {0};
        uint32_t minted[1] = {0u};
        uint32_t length = 0u;
        uint32_t caps = 0u;
        uint32_t result = recv_control(control, &grant, sizeof grant, minted,
                                       &length, &caps);
        if (result == 1u) {
            if (length != JE_SVC_BROKER_GRANT_LEN || caps != 1u
                || grant.op != expected_op || minted[0] == 0u)
                return 0u;
            *out_channel = minted[0];
            je_svc_broker_reply ack = {
                JE_SVC_OP_BROKER_ACK, grant.transaction
            };
            return je_try_send(control, &ack, sizeof ack) == JE_ABI_OK;
        }
        if (result != 0u) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

static uint32_t wait_unavailable(uint64_t control)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_svc_broker_reply reply = {0};
        uint32_t minted[1] = {0u};
        uint32_t length = 0u;
        uint32_t caps = 0u;
        uint32_t result = recv_control(control, &reply, sizeof reply, minted,
                                       &length, &caps);
        if (result == 1u) {
            return length == JE_SVC_BROKER_REPLY_LEN && caps == 0u
                && reply.op == JE_SVC_OP_BROKER_UNAVAIL
                && reply.detail == JE_SVC_UNAVAIL_HARDENED
                && minted[0] == 0u;
        }
        if (result != 0u) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

static uint32_t send_request(uint64_t control, uint32_t hardened)
{
    if (hardened != 0u) {
        je_svc_broker_request request = {
            JE_SVC_OP_BROKER_HARDENED, JE_SVC_UART_SERVICE_ID
        };
        return je_try_send(control, &request, sizeof request) == JE_ABI_OK;
    }
    je_svc_broker_request request = {
        JE_SVC_OP_BROKER_REQUEST, JE_SVC_UART_SERVICE_ID
    };
    return je_try_send(control, &request, sizeof request) == JE_ABI_OK;
}

static uint32_t send_write(uint32_t channel, uint32_t seq,
                           const char *text, uint32_t length,
                           uint32_t expected_status)
{
    je_svc_uart_request request = {
        JE_SVC_OP_UART_WRITE, seq, length, 0u, {0u}
    };
    memcpy(request.bytes, text, length);
    if (je_try_send(channel, &request, sizeof request) != JE_ABI_OK)
        return 0u;
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        je_svc_uart_reply reply = {0};
        uint64_t meta = 0u;
        uint64_t status = je_try_recv(channel, &reply, sizeof reply, 0u, &meta);
        if (status == JE_ABI_OK)
            return (meta & UINT64_C(0xffffffff)) == JE_SVC_UART_REPLY_LEN
                && (meta >> 32) == 0u
                && reply.op == JE_SVC_OP_UART_REPLY && reply.seq == seq
                && reply.status == expected_status
                && (expected_status != JE_SVC_UART_IO_OK || reply.len == length);
        if (status == JE_ABI_H_PEER_CLOSED) return 2u;
        if (status != JE_ABI_H_EMPTY) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

static uint32_t wait_peer_closed(uint32_t channel)
{
    uint64_t start = 0u;
    if (je_clock_ticks(&start) != JE_ABI_OK) return 0u;
    for (;;) {
        uint64_t meta = 0u;
        uint64_t status = je_try_recv(channel, (void *)0, 0u, 0u, &meta);
        if (status == JE_ABI_H_PEER_CLOSED) {
            (void)je_handle_close(channel);
            return 1u;
        }
        if (status != JE_ABI_H_EMPTY) return 0u;
        uint64_t now = 0u;
        if (je_clock_ticks(&now) != JE_ABI_OK
            || now - start >= JE_SVC_UART_DEADLINE)
            return 0u;
        if (je_yield() != JE_ABI_OK) return 0u;
    }
}

static uint32_t send_after_revoke(uint32_t channel, uint32_t seq)
{
    static const char text[] = "R!";
    je_svc_uart_request request = {
        JE_SVC_OP_UART_WRITE, seq, 2u, 0u, {0u}
    };
    memcpy(request.bytes, text, 2u);
    uint64_t status = je_try_send(channel, &request, sizeof request);
    if (status == JE_ABI_H_PEER_CLOSED) return 1u;
    if (status != JE_ABI_OK) return 0u;
    return wait_peer_closed(channel);
}

static uint32_t report(uint64_t control, uint32_t value)
{
    je_svc_config message = { JE_SVC_OP_CONFIG, value, 0u, 0u };
    return je_try_send(control, &message, sizeof message) == JE_ABI_OK;
}

int64_t je_main(uint64_t version, uint64_t n, uint64_t control,
                uint64_t unused1, uint64_t unused2, uint64_t unused3)
{
    (void)uart_client_rodata;
    (void)uart_client_data;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    if (version != JE_ABI_STARTUP_V1 || n != 1u || control == 0u)
        return 101;
    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };
    if (je_try_send(control, &ready, sizeof ready) != JE_ABI_OK)
        return 102;
    uint32_t role = 0u;
    if (!wait_role(control, &role)) return 103;
    if (role == JE_UART_CLIENT_HARDENED) {
        if (!send_request(control, 1u) || !wait_unavailable(control))
            return 104;
        if (!report(control, JE_UART_PROGRESS_DONE)) return 105;
        return 0x5548;
    }

    uint32_t channel = 0u;
    if (!send_request(control, 0u)
        || !wait_grant(control, JE_SVC_OP_BROKER_GRANT, &channel)
        || send_write(channel, 1u, "I1", 2u, JE_SVC_UART_IO_OK) != 1u
        || !report(control, JE_UART_PROGRESS_I1))
        return 106;
    if (!wait_peer_closed(channel)
        || !send_request(control, 0u)
        || !wait_grant(control, JE_SVC_OP_BROKER_RESTARTED, &channel)
        || send_write(channel, 2u, "I2", 2u, JE_SVC_UART_IO_OK) != 1u
        || !report(control, JE_UART_PROGRESS_I2))
        return 107;
    if (!wait_command(control, JE_UART_COMMAND_DETACH)
        || send_write(channel, 3u, "D!", 2u, JE_SVC_UART_IO_DETACHED) != 1u
        || !report(control, JE_UART_PROGRESS_D_SENT))
        return 108;
    if (!wait_command(control, JE_UART_COMMAND_REVOKED)
        || !send_after_revoke(channel, 4u)
        || !report(control, JE_UART_PROGRESS_R_SENT)
        || !send_request(control, 0u)
        || !wait_grant(control, JE_SVC_OP_BROKER_RESTARTED, &channel)
        || !report(control, JE_UART_PROGRESS_DONE))
        return 109;
    (void)je_handle_close(channel);
    return JE_SVC_UART_DONE_EXIT;
}
