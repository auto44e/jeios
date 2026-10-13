/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ARCH_H
#define JEIOS_ARCH_H
#include <stdint.h>
#include <jeios/status.h>
je_status je_arch_console_init(void);
void je_arch_console_hex(uint64_t value);
void je_arch_console_write(const char *text);
_Noreturn void je_arch_halt(void);
uint64_t je_arch_irq_save(void);
void je_arch_irq_restore(uint64_t flags);
void je_arch_timer_init(void);
void je_arch_timer_eoi(void);
/* Bootstrap detection only; not a scheduler capability record. */
int je_arch_has_nx(void);
#endif

