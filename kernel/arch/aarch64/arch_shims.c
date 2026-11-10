/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MP3c AArch64 bridges for the frame-neutral endpoint/lifecycle core.
 *
 * These are deliberately small architecture seams: endpoint critical sections
 * mask DAIF.I, scheduler identity comes from the ARM RR table, and the device
 * hooks remain fail-closed in the EL0 proof profile.
 */
#include <stddef.h>
#include <stdint.h>

#include <jeios/arch.h>
#include <jeios/lifecycle.h>
#include <jeios/sched.h>
#include <jeios/sched_rr.h>

#include "trapframe.h"

extern uint64_t je_arm_timer_ticks(void);
extern uint32_t je_arm_sched_current_identity;
extern void je_arm_sched_wake_ipc(uint32_t slot);

static inline uint64_t arm_read_daif(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, daif" : "=r"(value) :: "memory");
    return value;
}

uint64_t je_arch_irq_save(void)
{
    const uint64_t flags = arm_read_daif();
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return flags;
}

void je_arch_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0\n\tisb" : : "r"(flags) : "memory");
}

bool je_arch_irqs_enabled(void)
{
    return (arm_read_daif() & (UINT64_C(1) << 7)) == 0u;
}

void je_arch_console_write(const char *text)
{
    const uintptr_t dr = UINT64_C(0x09000000);
    const uintptr_t fr = dr + UINT64_C(0x18);
    if (!text) return;
    for (unsigned i = 0u; i < 4096u && text[i] != '\0'; ++i) {
        for (unsigned wait = 0u; wait < 100000u; ++wait)
            if ((*(volatile uint32_t *)fr & UINT32_C(1 << 5)) == 0u) break;
        *(volatile uint32_t *)dr = (uint32_t)(unsigned char)text[i];
    }
}

void je_arch_console_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (unsigned i = 0u; i < 16u; ++i)
        text[2u + i] = digits[(value >> ((15u - i) * 4u)) & 0xfu];
    text[18] = '\0';
    je_arch_console_write(text);
}

_Noreturn void je_arch_halt(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
    for (;;) __asm__ volatile("wfe" ::: "memory");
}

void je_sched_wake_ipc(uint32_t slot)
{
    je_arm_sched_wake_ipc(slot);
}

je_status je_sched_current_slot(uint32_t *out_slot)
{
    if (!out_slot || je_arm_sched_current_identity >= JE_RR_MAX)
        return JE_ERR_INVALID;
    *out_slot = je_arm_sched_current_identity;
    return JE_OK;
}

uint64_t je_sched_ticks(void)
{
    return je_arm_timer_ticks();
}

/* The remaining architecture services are not part of the ARM EL0 profile. */
je_status je_arch_console_init(void) { return JE_OK; }
void je_arch_timer_init(void) {}
void je_arch_timer_eoi(void) {}
void je_arch_irq3_mask(void) {}
void je_arch_irq3_unmask(void) {}
bool je_arch_irq3_is_unmasked(void) { return false; }
bool je_arch_irq7_is_unmasked(void) { return false; }
uint8_t je_arch_master_isr(void) { return 0u; }
void je_arch_master_eoi(void) {}
int je_arch_has_nx(void) { return 1; }
void je_arch_tss_set_rsp0(uint64_t rsp0) { (void)rsp0; }
uint64_t je_arch_tss_get_rsp0(void) { return 0u; }
void je_arch_cpu_policy_init(void) {}
uint64_t je_arch_cpu_policy_cr0(void) { return 0u; }
uint64_t je_arch_cpu_policy_cr4(void) { return 0u; }
void je_arch_tss_set_com2_io(bool allow) { (void)allow; }
uint8_t je_arch_tss_com2_io(void) { return 0u; }
uintptr_t je_arch_tss_address(void) { return 0u; }
