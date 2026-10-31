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

/* Data IPC (M3c step 4): forward to the pure send/peek/commit against the kernel
 * pool, resolving the caller's table. The glue does copy_from/to_user around
 * these (D6). */
je_ep_status je_endpoint_kern_send(je_vm *vm, uint32_t index, je_handle h,
                                   const void *kbuf, size_t len);
je_ep_status je_endpoint_kern_send_preflight(je_vm *vm, uint32_t index,
                                             je_handle h, size_t len);
je_ep_status je_endpoint_kern_recv_peek(je_vm *vm, uint32_t index, je_handle h,
                                        void *out_kbuf, size_t cap,
                                        size_t *out_len, je_ep_recv_lease *lease);
je_ep_status je_endpoint_kern_recv_commit(const je_ep_recv_lease *lease);

/* Capability transfer (M3c step 5): forwarders to the pure cap API against the
 * kernel pool + the caller's table. The glue (syscall.c) resolves user handle
 * arrays to/from descriptors and does copy_from/to_user. */
je_ep_status je_endpoint_kern_send_caps(je_vm *vm, uint32_t index, je_handle h,
                                        const void *kbuf, size_t len,
                                        const je_ep_capdesc *caps, uint8_t ncaps);
/* Resolve one user handle (in the caller's table) to a cap descriptor to SEND:
 * must be an endpoint holding TRANSFER; the requested rights must be a subset of
 * the handle's rights (DENIED otherwise). Fills *out. */
je_ep_status je_endpoint_kern_resolve_send_cap(je_vm *vm, uint32_t index,
                                               je_handle h, uint32_t req_rights,
                                               je_ep_capdesc *out);
je_ep_status je_endpoint_kern_recv_peek_caps(je_vm *vm, uint32_t index, je_handle h,
                                             void *out_kbuf, size_t cap,
                                             size_t *out_len, uint8_t hcap,
                                             je_ep_capdesc *out_caps,
                                             uint8_t *out_ncaps,
                                             je_ep_recv_lease *lease);
/* Mint a received cap descriptor into the caller's table as a fresh handle with
 * the descriptor's rights. Returns the handle (0 on failure) + status. */
je_ep_status je_endpoint_kern_mint_recv_cap(je_vm *vm, uint32_t index,
                                            const je_ep_capdesc *d, je_handle *out);
bool je_endpoint_kern_capdesc_live(const je_ep_capdesc *d);
void je_endpoint_kern_capdesc_release_handle(const je_ep_capdesc *d);
/* TEST FIXTURE ONLY (A5-4): seed a channel with one end in vm->procs[idxA]'s
 * table and the other in procs[idxB]'s — a cross-process channel that ring-3
 * cannot bootstrap itself yet. Both procs' VM MUST be hooked (asserted). Returns
 * the two handles. NOT a ring-3 ABI. */
je_ep_status je_endpoint_kern_seed(je_vm *vm, uint32_t idxA, uint32_t idxB,
                                   je_handle *outA, je_handle *outB);

/* Pool occupancy (live channel records). Kernel-internal / self-test only. */
uint32_t je_endpoint_kernel_allocated(void);

/* ---- M3c D7/A6 blocking-recv bridge ---- */
/* Release owner (A6-CLARIFY-2), called by sched.c mark_dying with the dying slot. */
bool je_endpoint_kern_waiter_release_by_id(uint32_t slot);
/* ADR 0010 D8 evidence counter (kernel-internal, NOT ABI; PR-D lead addition from the
 * Claude review P2-3). It counts reservations released because a WOKEN waiter
 * (head_reserved) was released by the death path (waiter_release_by_id). Lets the
 * M4d-s1 fixture OBSERVE "killed while WOKEN-reserved" instead of inferring it. */
uint32_t je_endpoint_kern_woken_reservations_released(void);
/* Recv glue: entry probe (deliverable? empty? closed?) and waiter registration. */
je_ep_status je_endpoint_kern_recv_entry_probe(je_vm *vm, uint32_t index, je_handle h);
je_ep_status je_endpoint_kern_wait_register(je_vm *vm, uint32_t index, je_handle h,
                                            uint32_t waiter_id,
                                            je_ep_channel **out_rec, uint8_t *out_side);
/* Wake a receiver blocked on the peer of the sender's end after a successful send. */
void je_endpoint_kern_send_wake(je_vm *vm, uint32_t index, je_handle h);
/* Internal cancel (A6-2; dev/test only). */
bool je_endpoint_kern_wait_cancel(uint32_t slot);
/* Continuation read of a WOKEN waiter's reserved head (bytes+caps) from the owned
 * rec/side; and the post-commit waiter release. */
je_ep_status je_endpoint_kern_recv_peek_owned(je_ep_channel *rec, uint8_t side,
                                              void *out_kbuf, size_t cap,
                                              size_t *out_len, uint8_t hcap,
                                              je_ep_capdesc *out_caps, uint8_t *out_ncaps,
                                              je_ep_recv_lease *lease);
void je_endpoint_kern_wait_release(je_ep_channel *rec, uint8_t side, uint32_t who);

#endif /* JEIOS_ENDPOINT_KERN_H */
