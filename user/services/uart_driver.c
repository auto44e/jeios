/* SPDX-License-Identifier: Apache-2.0 */
#include <jeios/user.h>
#include <jeios/svc.h>

#define D9_READY  0x44525952u
#define D9_RESULT 0x54534552u
#define D9_DEGRADED 1u
#define D9_PRESENT 2u

static const char driver_rodata[] __attribute__((used, section(".rodata")))
    = "JeiOS D9 COM2 driver";
static volatile uint32_t driver_data __attribute__((used, section(".data")));

typedef struct {
    uint32_t op;
    uint32_t value;
} d9_message;

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

static void uart_reset_and_probe(void)
{
    /* The sequence is deliberately ring-3: the kernel never touches COM2. */
    port_out(0x2f9u, 0u);  /* IER */
    port_out(0x2fau, 0xc7u); /* FCR */
    port_out(0x2fbu, 3u);  /* LCR */
    port_out(0x2fcu, 0x1bu); /* MCR, loopback + OUT2 for the mode-14 proof */
    port_out(0x2ffu, 0x5au); /* SCR */
    (void)port_in(0x2fau);  /* IIR */
    (void)port_in(0x2fdu);  /* LSR */
    (void)port_in(0x2feu);  /* MSR */
    (void)port_in(0x2f8u);  /* RBR */
    (void)port_in(0x2f9u);  /* IER */
    (void)port_in(0x2fbu);  /* LCR */
    (void)port_in(0x2fcu);  /* MCR */
    (void)port_in(0x2ffu);  /* SCR */
    port_out(0x2f9u, 3u);  /* enable received-data + THRE sources */
    port_out(0x2f8u, 0x5au); /* loopback byte: creates a real IRQ3 on QEMU COM2 */
}

int64_t je_main(uint64_t version, uint64_t n, uint64_t endpoint,
                uint64_t unused1, uint64_t unused2, uint64_t unused3)
{
    (void)driver_rodata;
    driver_data = 0u;
    (void)unused1; (void)unused2; (void)unused3;
    if (version != JE_ABI_STARTUP_V1 || n != 1u || endpoint == 0u)
        return 10;
    je_svc_ready ready = { JE_SVC_OP_READY, JE_SVC_PROTO_V0 };
    if (je_try_send(endpoint, &ready, sizeof ready) != JE_ABI_OK)
        return 11;

    uint64_t device = 0u;
    d9_message message = {0};
    uint64_t meta = 0u;
    for (uint32_t i = 0u; i < 2000u; ++i) {
        uint64_t status = je_try_recv(endpoint, &message, sizeof message, 0u, &meta);
        if (status == JE_ABI_OK) {
            if ((meta & UINT64_C(0xffffffff)) != sizeof message
                || (meta >> 32) != 0u
                || message.op < D9_READY || message.op > D9_READY + 13u)
                return 12;
            device = (uint32_t)message.value;
            break;
        }
        if (status != JE_ABI_H_EMPTY) return 13;
        if (je_yield() != JE_ABI_OK) return 14;
    }
    if (device == 0u) return 15;

    uart_reset_and_probe();
    uint32_t result = D9_PRESENT;
    if (port_in(0x2fdu) == 0xffu || port_in(0x2fau) == 0xffu)
        result = D9_DEGRADED;
    uint32_t mode = message.op - D9_READY;
    uint64_t pending = 0u;
    if (mode != 13u)
        (void)je_dev_irq_ack(device, &pending);
    d9_message report = { D9_RESULT, result };
    (void)je_try_send(endpoint, &report, sizeof report);
    if (result == D9_DEGRADED)
        return 19;
    if (mode == 1u || mode == 13u) {
        d9_message go = {0};
        for (;;) {
            uint64_t status = je_try_recv(endpoint, &go, sizeof go, 0u, &meta);
            if (status == JE_ABI_OK && go.op == D9_RESULT) break;
            if (status != JE_ABI_H_EMPTY) return 17;
            if (je_yield() != JE_ABI_OK) return 18;
        }
    } else if (mode == 2u) {
        (void)je_handle_close(device);
    }
    if (mode == 1u) {
        /* The admin revoke happened before this resumed syscall. This stale
         * ACK must not reopen IRQ3 or arm a new generation. */
        (void)je_dev_irq_ack(device, &pending);
    }
    /* The forbidden-port fault is the isolation proof. A fresh incarnation is
     * used by the supervisor for revoke/death checks after this one dies. */
    uint32_t mode_probe = mode;
    if (mode_probe == 0u) {
        (void)port_in(0x2f7u);
    } else if (mode_probe == 3u) {
        uint16_t value;
        __asm__ volatile("inw %w1,%0" : "=a"(value) : "d"((uint16_t)0x2ffu) : "memory");
    } else if (mode_probe == 4u) {
        uint32_t value;
        __asm__ volatile("inl %w1,%0" : "=a"(value) : "d"((uint16_t)0x2fdu) : "memory");
    } else if (mode_probe == 5u) {
        (void)port_in(0x300u);
    } else if (mode_probe == 6u) {
        (void)port_in(0x20u);
    } else if (mode_probe == 7u) {
        (void)port_in(0x40u);
    } else if (mode_probe == 8u) {
        (void)port_in(0x64u);
    } else if (mode_probe == 9u) {
        (void)port_in(0x70u);
    } else if (mode_probe == 10u) {
        (void)port_in(0x80u);
    } else if (mode_probe == 11u) {
        (void)port_in(0x3f8u);
    } else if (mode_probe == 12u) {
        uint8_t bytes[2] = {0u, 0u};
        uint64_t count = 2u;
        __asm__ volatile("cld; rep insb"
                         : "+D"(bytes), "+c"(count)
                         : "d"((uint16_t)0x2ffu)
                         : "memory");
    }
    if (mode != 13u)
        (void)je_dev_irq_ack(device, (uint64_t *)0);
    return 16;
}
