/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Hosted coverage for ADR 0010 D9's pure DEVICE core decisions.
 * Architecture effects are explicit fakes; assignment, generation, stale
 * authority, re-arm and IRQ7 policy remain the production device.c code.
 */
#include <jeios/arch.h>
#include <jeios/device.h>
#include <jeios/proc.h>
#include <jeios/sched.h>
#include <jeios/vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static uint64_t g_ticks;
static bool g_irq3_unmasked;
static bool g_irq7_unmasked;
static uint8_t g_isr;
static unsigned g_eoi_count;
static unsigned g_mask_count;
static unsigned g_unmask_count;
static uint8_t g_bitmap;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            fprintf(stderr, "test_device: FAIL %s:%d: %s [%s]\n",              \
                    __FILE__, __LINE__, (what), #cond);                         \
        }                                                                        \
    } while (0)

uint64_t je_sched_ticks(void) { return g_ticks; }
void je_arch_console_write(const char *text) { (void)text; }
_Noreturn void je_arch_halt(void) { abort(); }
void je_arch_irq3_mask(void) { g_irq3_unmasked = false; ++g_mask_count; }
void je_arch_irq3_unmask(void) { g_irq3_unmasked = true; ++g_unmask_count; }
bool je_arch_irq3_is_unmasked(void) { return g_irq3_unmasked; }
bool je_arch_irq7_is_unmasked(void) { return g_irq7_unmasked; }
uint8_t je_arch_master_isr(void) { return g_isr; }
void je_arch_master_eoi(void) { ++g_eoi_count; }
void je_arch_tss_set_com2_io(bool allow) { g_bitmap = allow ? 0u : 0xffu; }

static je_proc_pool g_procs;
static je_vm g_vm;
static je_handle_slot g_slots[JE_VM_MAX_PROCESSES][JE_VM_HANDLE_CAP];
static je_device g_device;
static je_proc_id g_supervisor;
static je_proc_id g_driver;
static je_proc_id g_other;

static void reset_fixture(void)
{
    memset(&g_vm, 0, sizeof g_vm);
    memset(g_slots, 0, sizeof g_slots);
    je_proc_pool_init(&g_procs);
    CHECK(je_proc_alloc(&g_procs, 0u, true, &g_supervisor) == JE_OK,
          "supervisor allocation");
    CHECK(je_proc_alloc(&g_procs, 0u, false, &g_driver) == JE_OK,
          "driver allocation");
    CHECK(je_proc_alloc(&g_procs, 0u, false, &g_other) == JE_OK,
          "second target allocation");
    for (uint32_t i = 1u; i <= 2u; ++i) {
        g_vm.procs[i].state = JE_VM_READY;
        CHECK(je_handle_table_init(&g_vm.procs[i].htable, g_slots[i],
                                   JE_VM_HANDLE_CAP, JE_VM_HANDLE_CAP - 1u),
              "target handle table initialization");
    }
    je_device_init(&g_device, &g_procs);
    g_ticks = 0u;
    g_irq3_unmasked = false;
    g_irq7_unmasked = false;
    g_isr = 0u;
    g_eoi_count = 0u;
    g_mask_count = 0u;
    g_unmask_count = 0u;
    g_bitmap = 0xffu;
}

static je_abi_status assign_driver(je_proc_id target, je_handle *out)
{
    return je_device_assign(&g_device, g_supervisor, target, &g_vm,
                            target.index, out);
}

static void test_assign_statuses(void)
{
    je_handle h = 0u;
    reset_fixture();
    CHECK(je_device_assign(&g_device, g_supervisor, g_supervisor, &g_vm,
                           g_supervisor.index, &h) == JE_ABI_INVALID,
          "supervisor target is invalid");
    CHECK(je_device_assign(&g_device, g_driver, g_driver, &g_vm,
                           g_driver.index, &h) == JE_ABI_INVALID,
          "self target is invalid");
    CHECK(je_proc_on_dying(&g_procs, g_driver, JE_DEATH_KILLED, 0u) == JE_OK,
          "driver becomes non-LIVE");
    CHECK(je_device_assign(&g_device, g_supervisor, g_driver, &g_vm,
                           g_driver.index, &h) == JE_ABI_H_STALE,
          "non-LIVE target is stale");

    reset_fixture();
    CHECK(assign_driver(g_driver, &h) == JE_ABI_OK, "initial assign");
    CHECK(je_device_assign(&g_device, g_supervisor, g_other, &g_vm,
                           g_other.index, &h) == JE_ABI_H_DENIED,
          "already-bound device is denied");
}

static void test_revoke_and_generation(void)
{
    je_handle h = 0u;
    uint32_t pending = 0u;
    reset_fixture();
    CHECK(assign_driver(g_driver, &h) == JE_ABI_OK, "assign for revoke");
    g_irq3_unmasked = true;
    je_device_irq3(&g_device);
    CHECK(g_device.pending && g_device.irq3_pending_observed,
          "IRQ3 sets pending and records the transition");
    CHECK(je_device_irq_ack(&g_device, g_driver, &g_vm, g_driver.index,
                            h, &pending) == JE_ABI_OK && pending == 1u,
          "ACK observes pending");
    CHECK(g_device.rearm_pending && g_device.irq3_ack_observed,
          "ACK schedules a re-arm");
    CHECK(je_device_revoke(&g_device) == JE_ABI_OK, "revoke completes");
    CHECK(!g_device.active && g_device.owner.gen == 0u
          && !g_device.pending && !g_device.rearm_pending
          && !je_arch_irq3_is_unmasked(),
          "revoke clears all device state and masks IRQ3");
    CHECK(je_device_irq_ack(&g_device, g_driver, &g_vm, g_driver.index,
                            h, &pending) == JE_ABI_H_STALE,
          "stale ACK after revoke is rejected");

    reset_fixture();
    CHECK(assign_driver(g_driver, &h) == JE_ABI_OK, "assign for stale re-arm");
    g_device.rearm_pending = true;
    g_device.ack_gen = g_device.binding_gen - 1u;
    je_device_timer(&g_device, 100u);
    CHECK(!g_device.rearm_pending, "generation-mismatched re-arm is dropped");

    reset_fixture();
    g_device.binding_gen = UINT32_MAX - 3u;
    CHECK(assign_driver(g_driver, &h) == JE_ABI_OK,
          "assign at UINT32_MAX-3 succeeds");
    CHECK(g_device.binding_gen == UINT32_MAX - 2u,
          "assign bumps generation exactly once");
    CHECK(je_device_revoke(&g_device) == JE_ABI_OK,
          "revoke near generation limit completes");
    CHECK(g_device.binding_gen == UINT32_MAX - 1u,
          "revoke reaches UINT32_MAX-1 without wrapping");
    g_device.binding_gen = UINT32_MAX - 2u;
    CHECK(assign_driver(g_other, &h) == JE_ABI_H_QUOTA,
          "assign at UINT32_MAX-2 is refused");

    reset_fixture();
    CHECK(assign_driver(g_driver, &h) == JE_ABI_OK, "assign for sentinel revoke");
    g_device.binding_gen = UINT32_MAX - 1u;
    CHECK(je_device_revoke(&g_device) == JE_ABI_OK,
          "revoke retires the device at the sentinel");
    CHECK(g_device.binding_gen == UINT32_MAX && !g_device.active,
          "sentinel generation is retained and inactive");
}

static void test_irq7_and_tick_saturation(void)
{
    je_handle h = 0u;
    reset_fixture();
    CHECK(assign_driver(g_driver, &h) == JE_ABI_OK, "assign for real IRQ7");
    g_device.rearm_pending = false;
    g_isr = 0x80u;
    g_irq7_unmasked = false;
    je_device_irq7(&g_device);
    CHECK(g_eoi_count == 1u, "real IRQ7 always receives master EOI");
    CHECK(!g_device.rearm_pending, "real IRQ7 does not schedule re-arm");

    g_isr = 0u;
    g_irq3_unmasked = true;
    je_device_irq7(&g_device);
    CHECK(!g_irq3_unmasked && g_device.rearm_pending,
          "spurious IRQ7 masks IRQ3 and re-arms an active binding");
    CHECK(g_eoi_count == 1u, "spurious IRQ7 sends no EOI");

    reset_fixture();
    g_isr = 0u;
    g_irq3_unmasked = false;
    je_device_irq7(&g_device);
    CHECK(!g_device.rearm_pending && g_eoi_count == 0u,
          "inactive spurious IRQ7 has no device side effect");

    reset_fixture();
    CHECK(assign_driver(g_driver, &h) == JE_ABI_OK, "assign for saturated tick");
    g_ticks = UINT64_MAX;
    g_device.last_unmask = UINT64_MAX;
    g_device.rearm_pending = true;
    g_device.ack_gen = g_device.binding_gen;
    g_device.rearm_tick = UINT64_MAX;
    je_device_timer(&g_device, UINT64_MAX);
    CHECK(!g_device.rearm_pending && g_irq3_unmasked,
          "next-tick saturation remains serviceable at UINT64_MAX");
}

int main(void)
{
    test_assign_statuses();
    test_revoke_and_generation();
    test_irq7_and_tick_saturation();
    if (g_failures != 0u) {
        fprintf(stderr, "test_device: FAIL (%u of %u checks)\n",
                g_failures, g_checks);
        return 1;
    }
    printf("test_device: PASS (%u checks)\n", g_checks);
    return 0;
}
