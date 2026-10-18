/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JEIOS_ENDPOINT_KERN_H
#define JEIOS_ENDPOINT_KERN_H
/*
 * M3c sub-PR 3 (ADR 0009 D-SEQ step 3, A3-1): the kernel's single endpoint pool
 * and the wiring that makes a process's death reclaim its endpoints.
 *
 * endpoint.c stays pure and hostable (zero global state); THIS file owns the one
 * kernel-wide je_endpoint_pool and bridges it to je_vm:
 *   - je_endpoint_kern_install(vm): one-time wiring, sets vm->teardown (the
 *     release walk), vm->teardown_ctx (the pool), and vm->irq_save/irq_restore
 *     (the A3-1 critical section) from the arch IRQ primitives. Call exactly once
 *     per je_vm before it becomes runnable; the fields are immutable thereafter.
 *   - je_endpoint_kern_create(vm, &h0, &h1): the ONLY endpoint creation path for
 *     a process table. It refuses (JE_EP_CORRUPT) unless `vm` has the teardown
 *     hook installed, so "an endpoint lives in a ring-3 table ⇒ that vm's
 *     je_vm_destroy walks it" holds by construction (A3-2). Used by the
 *     endpoint_create syscall AND the main.c self-tests, so neither can skip the
 *     wiring check.
 *   - je_endpoint_kern_close(vm, h): typed close of an endpoint handle in the
 *     caller's table, for the HANDLE_CLOSE dispatcher.
 *   - je_endpoint_kernel_allocated(): pool occupancy, kernel-internal/test-only
 *     (the fault-death proof reads it); NOT a ring-3 ABI.
 */
#include <jeios/endpoint.h>
#include <jeios/vm.h>
#include <jeios/handle.h>

void je_endpoint_kern_install(je_vm *vm);

/* Mint a channel's two ends into `vm`'s current-process table. Returns JE_EP_OK
 * and the two handles, or an error (incl. JE_EP_CORRUPT if `vm` is unwired). */
je_ep_status je_endpoint_kern_create(je_vm *vm, uint32_t index,
                                     je_handle *out0, je_handle *out1);

/* Typed close of handle `h` as an endpoint in `vm`'s process `index` table. */
je_ep_status je_endpoint_kern_close(je_vm *vm, uint32_t index, je_handle h);

/* Pool occupancy (live channel records). Kernel-internal / self-test only. */
uint32_t je_endpoint_kernel_allocated(void);

#endif /* JEIOS_ENDPOINT_KERN_H */
