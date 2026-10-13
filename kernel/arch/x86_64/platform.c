/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <jeios/arch.h>
static inline void out8(uint16_t port, uint8_t value)
{ __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port)); }
static inline uint8_t in8(uint16_t port)
{ uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static int serial_ready;
je_status je_arch_console_init(void)
{
    /* Development console only: classic 16550 at COM1, 38400 baud. */
    out8(0x3f9, 0); out8(0x3fb, 0x80);
    out8(0x3f8, 3); out8(0x3f9, 0);
    out8(0x3fb, 3); out8(0x3fa, 0xc7); out8(0x3fc, 0x0b);
    serial_ready = in8(0x3fd) != 0xff;
    return serial_ready ? JE_OK : JE_ERR_UNSUPPORTED;
}
void je_arch_console_write(const char *s)
{
    if (!serial_ready || s == 0) return;
    for (; *s; ++s) {
        /* Bounded poll: absence of a working UART must not hang boot.
         * This iteration budget is NOT a wall-clock timeout. */
        unsigned budget = 100000;
        while (!(in8(0x3fd) & 0x20) && budget) --budget;
        if (!budget) return;
        out8(0x3f8, (uint8_t)*s);
    }
}
int je_arch_has_nx(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(0x80000000u), "c"(0));
    if (a < 0x80000001u) return 0;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(0x80000001u), "c"(0));
    return (d & (1u << 20)) != 0;
}
_Noreturn void je_arch_halt(void)
{
    for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
}

void je_arch_console_hex(uint64_t value)
{
    char text[19] = "0x0000000000000000";
    const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        text[17-i] = digits[value & 15]; value >>= 4;
    }
    je_arch_console_write(text);
}

uint64_t je_arch_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli":"=r"(flags)::"memory");
    return flags;
}
void je_arch_irq_restore(uint64_t flags)
{
    if (flags & (UINT64_C(1)<<9)) __asm__ volatile("sti":::"memory");
}
static void io_wait(void) { out8(0x80,0); }
void je_arch_timer_init(void)
{
    /* BSP legacy PC only: PIC ICW sequence, all IRQs masked except IRQ0.
     * PIT channel 0 mode 2, divisor 11932 (~100 Hz), not a precise clock. */
    out8(0x21,0xff);out8(0xa1,0xff);
    out8(0x20,0x11);io_wait();out8(0xa0,0x11);io_wait();
    out8(0x21,0x20);io_wait();out8(0xa1,0x28);io_wait();
    out8(0x21,4);io_wait();out8(0xa1,2);io_wait();
    out8(0x21,1);io_wait();out8(0xa1,1);io_wait();
    out8(0xa1,0xff);out8(0x21,0xfe);
    out8(0x43,0x34);out8(0x40,(uint8_t)(11932&255));out8(0x40,(uint8_t)(11932>>8));
}
void je_arch_timer_eoi(void) { out8(0x20,0x20); }

