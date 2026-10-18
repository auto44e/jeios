/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3c sub-PR 3 (ADR 0009 D-SEQ step 3): the kernel's single endpoint pool and
 * the je_vm wiring (teardown hook + IRQ critical section). This is the only file
 * that holds endpoint global state; endpoint.c stays pure/hostable. Lives in the
 * real kernel build only (NOT linked into the hosted unit tests), so it may use
 * the arch IRQ primitives and the kernel pool freely.
 */
#include <jeios/endpoint_kern.h>
#include <jeios/arch.h>

/* The one kernel-wide pool. A dying process's endpoints are reclaimed into it by
 * the teardown hook; a leaked record would eventually exhaust it (the whole
 * point of A3-1). */
static je_endpoint_pool g_kep;
static bool g_kep_inited;

static void kep_init_once(void)
{
    if (!g_kep_inited) { je_endpoint_pool_init(&g_kep); g_kep_inited = true; }
}

/* Teardown hook: the release walk over the dying process's table. A negative
 * return is an invariant violation (pool corruption) and MUST stop the kernel
 * before the table is zeroed — continuing would zero away the leak evidence and
 * ship a lie. */
static void kep_teardown(void *ctx, je_handle_table *t)
{
    je_endpoint_pool *pool = (je_endpoint_pool *)ctx;
    if (je_endpoint_release_table(pool, t) < 0) {
        je_arch_console_write("JeiOS fatal: endpoint release walk failed "
                              "(pool corrupt)\r\n");
        je_arch_halt();
    }
}

void je_endpoint_kern_install(je_vm *vm)
{
    if (!vm) return;
    kep_init_once();
    vm->teardown     = kep_teardown;
    vm->teardown_ctx = &g_kep;
    vm->irq_save     = je_arch_irq_save;
    vm->irq_restore  = je_arch_irq_restore;
}

je_ep_status je_endpoint_kern_create(je_vm *vm, uint32_t index,
                                     je_handle *out0, je_handle *out1)
{
    if (out0) *out0 = 0u;
    if (out1) *out1 = 0u;
    if (!vm || index >= JE_VM_MAX_PROCESSES || !out0 || !out1)
        return JE_EP_BADINDEX;
    /* STRUCTURAL GUARANTEE (A3-2): refuse to mint an endpoint into a table whose
     * death path will not reclaim it atomically. Require the FULL wiring — the
     * teardown hook AND the IRQ critical-section pair — not just the hook, so a
     * partially-wired vm cannot mint without the A3-1 critical section. */
    if (vm->teardown != kep_teardown || vm->teardown_ctx != &g_kep
        || vm->irq_save == (uint64_t (*)(void))0
        || vm->irq_restore == (void (*)(uint64_t))0)
        return JE_EP_CORRUPT;
    kep_init_once();
    je_handle_table *t = &vm->procs[index].htable;
    return je_endpoint_channel_create(&g_kep, t, JE_EP_RIGHTS_ALL,
                                      t, JE_EP_RIGHTS_ALL, out0, out1);
}

je_ep_status je_endpoint_kern_close(je_vm *vm, uint32_t index, je_handle h)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_handle_close(&g_kep, &vm->procs[index].htable, h);
}

uint32_t je_endpoint_kernel_allocated(void)
{
    return g_kep_inited ? je_endpoint_pool_allocated(&g_kep) : 0u;
}
