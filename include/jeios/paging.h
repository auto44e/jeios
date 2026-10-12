#ifndef JEIOS_PAGING_H
#define JEIOS_PAGING_H
#include <jeios/boot.h>
je_status je_arch_paging_init(const je_boot_context *ctx);
/* BSP only, interrupts disabled, owned kernel RAM page. No SMP TLB shootdown. */
je_status je_arch_page_guard(uintptr_t address);
extern unsigned char __kernel_start[], __kernel_end[], __text_end[], __rodata_end[];
extern unsigned char je_boot_guard[];
#endif
