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

/* Data IPC forwarders (M3c step 4): resolve the caller's table and run the pure
 * send/peek/commit against the kernel pool. The glue (syscall.c) does the
 * copy_from/to_user around these, honoring D6 ordering. */
je_ep_status je_endpoint_kern_send(je_vm *vm, uint32_t index, je_handle h,
                                   const void *kbuf, size_t len)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_try_send(&g_kep, &vm->procs[index].htable, h, kbuf, len);
}

je_ep_status je_endpoint_kern_send_preflight(je_vm *vm, uint32_t index,
                                             je_handle h, size_t len)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    /* Main-handle preflight only (no QUEUE_FULL) so per-cap errors outrank
     * QUEUE_FULL in the cap-send glue (A5-4). The final try_send_caps re-checks
     * QUEUE_FULL atomically at enqueue. */
    return je_endpoint_send_preflight_main(&g_kep, &vm->procs[index].htable, h, len);
}

je_ep_status je_endpoint_kern_recv_peek(je_vm *vm, uint32_t index, je_handle h,
                                        void *out_kbuf, size_t cap,
                                        size_t *out_len, je_ep_recv_lease *lease)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_recv_peek(&g_kep, &vm->procs[index].htable, h,
                                 out_kbuf, cap, out_len, lease);
}

je_ep_status je_endpoint_kern_recv_commit(const je_ep_recv_lease *lease)
{
    kep_init_once();
    return je_endpoint_recv_commit_caps(&g_kep, lease);
}

/* ---- capability transfer forwarders (M3c step 5) ------------------------- */

je_ep_status je_endpoint_kern_send_caps(je_vm *vm, uint32_t index, je_handle h,
                                        const void *kbuf, size_t len,
                                        const je_ep_capdesc *caps, uint8_t ncaps)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_try_send_caps(&g_kep, &vm->procs[index].htable, h,
                                     kbuf, len, caps, ncaps);
}

je_ep_status je_endpoint_kern_resolve_send_cap(je_vm *vm, uint32_t index,
                                               je_handle h, uint32_t req_rights,
                                               je_ep_capdesc *out)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES || !out) return JE_EP_BADINDEX;
    kep_init_once();
    je_handle_table *tbl = &vm->procs[index].htable;
    /* The handle must be an endpoint holding TRANSFER. */
    je_ep_channel *rec; uint8_t side;
    je_ep_status rs = je_endpoint_resolve(&g_kep, tbl, h, &rec, &side);
    if (rs != JE_EP_OK) return rs;
    uint32_t rights = 0u;
    je_handle_status gr = je_handle_rights_of(tbl, h, JE_ENDPOINT_OBJ_TYPE,
                                              JE_EP_RIGHT_TRANSFER, &rights);
    if (gr != JE_HANDLE_OK) {
        /* map DENIED/etc via the same table mapping the pure layer uses */
        switch (gr) {
        case JE_HANDLE_BADINDEX:  return JE_EP_BADINDEX;
        case JE_HANDLE_STALE:     return JE_EP_STALE;
        case JE_HANDLE_WRONGTYPE: return JE_EP_WRONGTYPE;
        case JE_HANDLE_DENIED:    return JE_EP_DENIED;
        default:                  return JE_EP_CORRUPT;
        }
    }
    /* Attenuation: the requested rights must be a subset of what the handle holds. */
    if (req_rights & ~rights) return JE_EP_DENIED;
    /* Build the descriptor: the target end is the handle's OWN end (the receiver
     * will get a handle to the same end the sender named). rec_index via pointer
     * arithmetic into the kernel pool. */
    out->rec_index = (uint16_t)(rec - &g_kep.rec[0]);
    out->side = side;
    out->cap_count_pad = 0u;
    out->generation = rec->generation;
    out->rights = req_rights;
    return JE_EP_OK;
}

je_ep_status je_endpoint_kern_recv_peek_caps(je_vm *vm, uint32_t index, je_handle h,
                                             void *out_kbuf, size_t cap,
                                             size_t *out_len, uint8_t hcap,
                                             je_ep_capdesc *out_caps,
                                             uint8_t *out_ncaps,
                                             je_ep_recv_lease *lease)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_recv_peek_caps(&g_kep, &vm->procs[index].htable, h,
                                      out_kbuf, cap, out_len, hcap,
                                      out_caps, out_ncaps, lease);
}

je_ep_status je_endpoint_kern_mint_recv_cap(je_vm *vm, uint32_t index,
                                            const je_ep_capdesc *d, je_handle *out)
{
    if (out) *out = 0u;
    if (!vm || index >= JE_VM_MAX_PROCESSES || !d || !out) return JE_EP_BADINDEX;
    kep_init_once();
    je_handle_table *tbl = &vm->procs[index].htable;
    je_ep_channel *tr; uint8_t ts;
    je_ep_status s = je_endpoint_capdesc_resolve(&g_kep, d, &tr, &ts);
    if (s != JE_EP_OK) return s;                 /* dead target -> STALE (A5-2) */
    /* Retain-before-commit: retain a handle_ref on the target, then alloc the
     * receiver handle; roll back the retain on alloc failure. */
    je_ep_status rr = je_endpoint_capdesc_retain_handle(&g_kep, d);
    if (rr != JE_EP_OK) return rr;               /* REFOVERFLOW */
    je_handle nh = 0u;
    je_handle_status a = je_handle_alloc(tbl, (void *)&tr->end[ts],
                                         JE_ENDPOINT_OBJ_TYPE, d->rights, &nh);
    if (a != JE_HANDLE_OK) {
        je_endpoint_capdesc_release_handle(&g_kep, d);   /* undo retain */
        switch (a) {
        case JE_HANDLE_QUOTA:   return JE_EP_QUOTA;
        case JE_HANDLE_NOSLOTS: return JE_EP_NOSLOTS;
        default:                return JE_EP_CORRUPT;
        }
    }
    *out = nh;
    return JE_EP_OK;
}

bool je_endpoint_kern_capdesc_live(const je_ep_capdesc *d)
{
    return je_endpoint_capdesc_live(&g_kep, d);
}

void je_endpoint_kern_capdesc_release_handle(const je_ep_capdesc *d)
{
    je_endpoint_capdesc_release_handle(&g_kep, d);
}

je_ep_status je_endpoint_kern_seed(je_vm *vm, uint32_t idxA, uint32_t idxB,
                                   je_handle *outA, je_handle *outB)
{
    if (outA) *outA = 0u;
    if (outB) *outB = 0u;
    if (!vm || idxA >= JE_VM_MAX_PROCESSES || idxB >= JE_VM_MAX_PROCESSES
        || idxA == idxB || !outA || !outB) return JE_EP_BADINDEX;
    /* A5-4: both VMs must be hooked so the seeded ends are on a reclaiming death
     * path (the raw channel_create bypasses op-10's A3-2 hook check). */
    if (vm->teardown != kep_teardown || vm->teardown_ctx != &g_kep
        || vm->irq_save == (uint64_t (*)(void))0
        || vm->irq_restore == (void (*)(uint64_t))0) return JE_EP_CORRUPT;
    kep_init_once();
    return je_endpoint_channel_create(&g_kep, &vm->procs[idxA].htable,
                                      JE_EP_RIGHTS_ALL,
                                      &vm->procs[idxB].htable, JE_EP_RIGHTS_ALL,
                                      outA, outB);
}

uint32_t je_endpoint_kernel_allocated(void)
{
    return g_kep_inited ? je_endpoint_pool_allocated(&g_kep) : 0u;
}
