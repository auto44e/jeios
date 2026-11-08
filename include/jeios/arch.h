/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ARCH_H
#define JEIOS_ARCH_H
#include <stdbool.h>
#include <stdint.h>
#include <jeios/status.h>
je_status je_arch_console_init(void);
void je_arch_console_hex(uint64_t value);
void je_arch_console_write(const char *text);
_Noreturn void je_arch_halt(void);
uint64_t je_arch_irq_save(void);
void je_arch_irq_restore(uint64_t flags);
/* True iff maskable interrupts are currently enabled (x86: RFLAGS.IF set). Used
 * by scheduler code to ENFORCE an interrupts-off precondition at a trust
 * boundary (not to toggle state); a hosted test overrides it to exercise the
 * precondition without privileged flag manipulation. CONTRACT: every port MUST
 * provide a real strong definition that reports the true hardware state. A weak
 * or hardcoded-false stub is forbidden — it would let an IF-on misuse slip past
 * the reaper's fail-stop. The hosted override must default to interrupts-off and
 * only the dedicated IF-precondition test may report enabled. */
bool je_arch_irqs_enabled(void);
void je_arch_timer_init(void);
void je_arch_timer_eoi(void);
void je_arch_irq3_mask(void);
void je_arch_irq3_unmask(void);
bool je_arch_irq3_is_unmasked(void);
bool je_arch_irq7_is_unmasked(void);
uint8_t je_arch_master_isr(void);
void je_arch_master_eoi(void);
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
void je_arch_tss_set_com2_io(bool allow);
uint8_t je_arch_tss_com2_io(void);
uintptr_t je_arch_tss_address(void);
#endif

