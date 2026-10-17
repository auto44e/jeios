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
/* M3a #4 step 3: ring-transition support. tss_set_rsp0 sets the kernel stack the
 * CPU loads on a CPL3->CPL0 trap; cpu_policy_init enforces ADR 0007 D7 (no
 * FPU/SIMD) and must run once after paging init, before any user thread. The
 * *_get/_cr0/_cr4 readbacks are test-only proofs the writes took. */
void je_arch_tss_set_rsp0(uint64_t rsp0);
uint64_t je_arch_tss_get_rsp0(void);
void je_arch_cpu_policy_init(void);
uint64_t je_arch_cpu_policy_cr0(void);
uint64_t je_arch_cpu_policy_cr4(void);
#endif

