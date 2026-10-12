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
