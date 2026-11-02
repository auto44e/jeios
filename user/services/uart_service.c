/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>
#include <jeios/uart_test.h>

#define UART_BASE 0x2f8u
#define UART_IER  (UART_BASE + 1u)
#define UART_IIR  (UART_BASE + 2u)
#define UART_FCR  (UART_BASE + 2u)
#define UART_LCR  (UART_BASE + 3u)
#define UART_MCR  (UART_BASE + 4u)
#define UART_LSR  (UART_BASE + 5u)
#define UART_MSR  (UART_BASE + 6u)
#define UART_RBR  UART_BASE

#define UART_LSR_DR   0x01u
#define UART_LSR_THRE 0x20u
#define UART_POLL_MAX 64u
#define UART_CHANNEL_MAX JE_SVC_UART_CLIENT_CAP

static const char uart_service_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS brokered COM2 UART service";
static volatile uint32_t uart_mode
    __attribute__((used, section(".je_report.go"), aligned(4)));

static uint8_t port_in(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static void port_out(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port) : "memory");
}

static uint32_t recv_one(uint64_t endpoint, uint8_t *bytes, uint32_t capacity,
                         uint32_t *minted, uint32_t *length,
                         uint32_t *caps)
{
    uint64_t meta = 0u;
    uint64_t status = je_try_recv_caps(endpoint, bytes, capacity, minted, 1u,
                                       0u, &meta);
    if (length) *length = (uint32_t)meta;
    if (caps) *caps = (uint32_t)(meta >> 32);
    if (status == JE_ABI_H_EMPTY) return 0u;
    if (status == JE_ABI_H_PEER_CLOSED) return UINT32_MAX;
    if (status != JE_ABI_OK) return UINT32_MAX - 1u;
    return 1u;
}

static uint32_t close_minted(uint32_t *minted, uint32_t count)
{
    if (!minted || count > 1u) return 0u;
    if (count == 0u) return 1u;
    for (uint32_t i = 0u; i < count; ++i) {
        if (minted[i] != 0u) (void)je_handle_close(minted[i]);
        minted[i] = 0u;
    }
    return 1u;
}

static uint32_t uart_probe(void)
{
    /*
     * LCR is deliberately first: DLAB is known before touching the register
     * aliases. MCR=OUT2 without loopback is the real-TX profile used by QEMU's
     * second serial backend.
     */
    port_out(UART_LCR, 0x03u);
    port_out(UART_IER, 0u);
    port_out(UART_FCR, 0xc7u);
    port_out(UART_MCR, 0x0bu);
    (void)port_in(UART_IIR);
    uint8_t lsr = port_in(UART_LSR);
    (void)port_in(UART_MSR);
    (void)port_in(UART_RBR);
    if (lsr == 0xffu || port_in(UART_IIR) == 0xffu)
        return JE_SVC_UART_DEGRADED;
    port_out(UART_IER, 0x03u);
    return JE_SVC_UART_PRESENT;
}

static uint32_t uart_write(const uint8_t *bytes, uint32_t length,
                           uint32_t *written)
{
    if (written) *written = 0u;
    if (!bytes || !written || length > JE_SVC_UART_PAYLOAD_MAX)
        return JE_SVC_UART_IO_INVALID;
    for (uint32_t i = 0u; i < length; ++i) {
        uint32_t ready = 0u;
        for (uint32_t poll = 0u; poll < UART_POLL_MAX; ++poll) {
            if ((port_in(UART_LSR) & UART_LSR_THRE) != 0u) {
                ready = 1u;
                break;
            }
        }
        if (ready == 0u) return JE_SVC_UART_IO_WOULDBLOCK;
        port_out(UART_BASE, bytes[i]);
        *written = i + 1u;
    }
    return JE_SVC_UART_IO_OK;
}

static uint32_t serve_request(uint64_t endpoint, uint32_t device,
                              uint32_t attached)
{
    je_svc_uart_request request = {0};
    uint64_t meta = 0u;
    uint64_t status = je_try_recv(endpoint, &request, sizeof request, 0u,
                                   &meta);
    if (status == JE_ABI_H_EMPTY) return 1u;
    if (status == JE_ABI_H_PEER_CLOSED) {
        (void)je_handle_close(endpoint);
        return 2u;
    }
    if (status != JE_ABI_OK
        || (meta & UINT64_C(0xffffffff)) != JE_SVC_UART_REQUEST_LEN
        || (meta >> 32) != 0u || request.reserved != 0u
        || request.len > JE_SVC_UART_PAYLOAD_MAX) {
        (void)je_handle_close(endpoint);
        return 2u;
    }

    je_svc_uart_reply reply = {
        JE_SVC_OP_UART_REPLY, request.seq, JE_SVC_UART_IO_INVALID, 0u, {0u}
    };
    if (attached == 0u) {
        reply.status = JE_SVC_UART_IO_DETACHED;
    } else if (request.op == JE_SVC_OP_UART_WRITE) {
        reply.status = uart_write(request.bytes, request.len, &reply.len);
    } else if (request.op == JE_SVC_OP_UART_READ) {
        reply.status = JE_SVC_UART_IO_EMPTY;
        while (reply.len < request.len
               && (port_in(UART_LSR) & UART_LSR_DR) != 0u) {
            reply.bytes[reply.len] = port_in(UART_RBR);
            ++reply.len;
        }
        if (reply.len != 0u) reply.status = JE_SVC_UART_IO_OK;
    }
    if (je_try_send(endpoint, &reply, sizeof reply) != JE_ABI_OK) {
        (void)je_handle_close(endpoint);
        return 2u;
    }
    (void)device;
    return 0u;
}

static uint32_t handle_control(uint64_t control, uint32_t *device,
                               uint32_t *device_present, uint32_t *attached,
                               uint32_t *brokerable, uint32_t *generation,
                               uint32_t *channels, uint32_t *channel_count)
{
    uint8_t bytes[JE_SVC_UART_REQUEST_LEN] = {0u};
    uint32_t minted[1] = {0u};
    uint32_t length = 0u;
    uint32_t caps = 0u;
    uint32_t result = recv_one(control, bytes, sizeof bytes, minted, &length,
                               &caps);
    if (result == 0u) return 1u;
    if (result == UINT32_MAX) return 0u;
    if (result == UINT32_MAX - 1u) {
        (void)je_handle_close(control);
        return 0u;
    }
    if (caps > 1u) {
        (void)close_minted(minted, caps);
        (void)je_handle_close(control);
        return 0u;
    }

    uint32_t op = 0u;
    if (length >= sizeof op) memcpy(&op, bytes, sizeof op);
    if (caps != 0u && op != JE_SVC_OP_SERVICE_BIND) {
        close_minted(minted, caps);
        (void)je_handle_close(control);
        return 0u;
    }
    if (op == JE_SVC_OP_SERVICE_BIND) {
        je_svc_service_bind bind = {0};
        if (length != JE_SVC_SERVICE_BIND_LEN || caps != 1u
            || minted[0] == 0u || *attached == 0u || *brokerable == 0u
            || *generation == 0u || *channel_count >= UART_CHANNEL_MAX) {
            (void)close_minted(minted, caps);
            (void)je_handle_close(control);
            return 0u;
        }
        memcpy(&bind, bytes, sizeof bind);
        if (bind.service_generation != *generation) {
            (void)close_minted(minted, caps);
            (void)je_handle_close(control);
            return 0u;
        }
        je_svc_broker_reply ack = { JE_SVC_OP_BROKER_ACK, bind.transaction };
        if (je_try_send(control, &ack, sizeof ack) != JE_ABI_OK) {
            (void)close_minted(minted, caps);
            (void)je_handle_close(control);
            return 0u;
        }
        channels[*channel_count] = minted[0];
        ++*channel_count;
        return 1u;
    }
    if (caps != 0u) {
        (void)close_minted(minted, caps);
        (void)je_handle_close(control);
        return 0u;
    }
    if (op == JE_SVC_OP_UART_ATTACH) {
        je_svc_uart_attach attach = {0};
        if (length != JE_SVC_UART_ATTACH_LEN) {
            (void)je_handle_close(control);
            return 0u;
        }
        memcpy(&attach, bytes, sizeof attach);
        if (attach.device_handle == 0u) {
            if (*device_present != 0u) {
                (void)je_handle_close(*device);
                *device = 0u;
                *device_present = 0u;
                *attached = 0u;
                *brokerable = 0u;
                je_svc_uart_control detached = {
                    JE_SVC_OP_UART_DETACHED, 0u
                };
                if (je_try_send(control, &detached, sizeof detached)
                    != JE_ABI_OK)
                    return 0u;
            }
            return 1u;
        }
        if (*device_present != 0u) {
            (void)je_handle_close(*device);
            *device = 0u;
            *device_present = 0u;
            *attached = 0u;
            *brokerable = 0u;
        }
        *device = attach.device_handle;
        *device_present = 1u;
        uint32_t profile = uart_probe();
        je_svc_uart_control reply = {
            JE_SVC_OP_UART_ATTACH_OK, profile
        };
        if (je_try_send(control, &reply, sizeof reply) != JE_ABI_OK) {
            (void)je_handle_close(*device);
            *device = 0u;
            *device_present = 0u;
            return 0u;
        }
        if (profile == JE_SVC_UART_PRESENT) {
            *attached = 1u;
            *brokerable = 1u;
        } else {
            (void)je_handle_close(*device);
            *device = 0u;
            *device_present = 0u;
            *attached = 0u;
            *brokerable = 0u;
        }
        return 1u;
    }
    if (op == JE_SVC_OP_UART_PING) {
        je_svc_uart_control ping = {0};
        if (length != JE_SVC_UART_CONTROL_LEN) {
            (void)je_handle_close(control);
            return 0u;
        }
        memcpy(&ping, bytes, sizeof ping);
        if (ping.arg == 0u) {
            (void)je_handle_close(control);
            return 0u;
        }
        if (*generation == 0u) *generation = ping.arg;
        if (je_try_send(control,
                        &(je_svc_uart_control){JE_SVC_OP_UART_PONG, ping.arg},
                        sizeof(je_svc_uart_control)) != JE_ABI_OK)
            return 0u;
        return 1u;
    }
    (void)je_handle_close(control);
    return 0u;
}

int64_t je_main(uint64_t version, uint64_t n, uint64_t control,
                uint64_t unused1, uint64_t unused2, uint64_t unused3)
{
    (void)uart_service_rodata;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    uart_mode = JE_SVC_UART_MODE_NONE;
    if (version != JE_ABI_STARTUP_V1 || n != 1u || control == 0u)
        return 91;

    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };
    if (je_try_send(control, &ready, sizeof ready) != JE_ABI_OK)
        return 92;

    uint32_t device = 0u;
    uint32_t device_present = 0u;
    uint32_t attached = 0u;
    uint32_t brokerable = 0u;
    uint32_t generation = 0u;
    uint32_t channels[UART_CHANNEL_MAX] = {0u};
    uint32_t channel_count = 0u;
    for (;;) {
        if (uart_mode == JE_SVC_UART_MODE_HANG) {
            uint8_t ignored[JE_SVC_UART_REQUEST_LEN] = {0u};
            uint64_t meta = 0u;
            uint64_t status = je_try_recv(control, ignored, sizeof ignored,
                                          0u, &meta);
            if (status != JE_ABI_H_EMPTY
                && status != JE_ABI_OK
                && status != JE_ABI_H_PEER_CLOSED)
                return 93;
            /*
             * HANG means "do not answer", not "occupy the CPU".  This is a
             * single-CPU fixture: yield after discarding each control record
             * so the supervisor can observe the bounded PING deadline and
             * perform the policy kill.  No broker channel is serviced here.
             */
            if (je_yield() != JE_ABI_OK) return 95;
            continue;
        }
        if (handle_control(control, &device, &device_present, &attached,
                           &brokerable, &generation,
                           channels, &channel_count) == 0u)
            return 94;
        for (uint32_t i = 0u; i < channel_count; ++i) {
            if (channels[i] == 0u) continue;
            uint32_t result = serve_request(channels[i], device, attached);
            if (result == 2u)
                channels[i] = 0u;
        }
        for (uint32_t i = 0u; i < channel_count; ++i) {
            if (channels[i] != 0u) continue;
            for (uint32_t j = i + 1u; j < channel_count; ++j)
                channels[j - 1u] = channels[j];
            channels[channel_count - 1u] = 0u;
            --channel_count;
            --i;
        }
        if (je_yield() != JE_ABI_OK) return 96;
    }
}
