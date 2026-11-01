/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3c sub-PR 3 (ADR 0009 D-SEQ step 3): the kernel's single endpoint pool and
 * the je_vm wiring (teardown hook + IRQ critical section). This is the only file
 * that holds endpoint global state; endpoint.c stays pure/hostable. Lives in the
 * real kernel build only (NOT linked into the hosted unit tests), so it may use
 * the arch IRQ primitives and the kernel pool freely.
 */
#include <jeios/endpoint_kern.h>
#include <jeios/sched.h>   /* je_sched_wake_ipc for the blocking-recv bridge */
#include <jeios/arch.h>
#include <jeios/abi.h>

_Static_assert(JE_EP_PROCESS_OBJ_TYPE == JE_ABI_OBJ_PROCESS,
               "PROCESS object type must match the ABI");

/* ADR 0010 D2 endpoint pool formula (needs both vm.h and endpoint.h). */
_Static_assert(JE_EP_POOL_CAP >= 2u * (JE_VM_MAX_PROCESSES - 1u) + JE_BROKER_MAX + 1u + 2u
               + (JE_VM_MAX_PROCESSES - 1u) * JE_EP_CHILD_QUOTA_MAX + JE_EP_RESERVE,
               "ADR 0010 D2 endpoint pool formula");

/* The one kernel-wide pool. A dying process's endpoints are reclaimed into it by
 * the teardown hook; a leaked record would eventually exhaust it (the whole
 * point of A3-1). */
static je_endpoint_pool g_kep;
static bool g_kep_inited;
static uint32_t g_woken_reservations_released;

static void kep_init_once(void)
{
    if (!g_kep_inited) { je_endpoint_pool_init(&g_kep); g_kep_inited = true; }
}

void je_endpoint_kern_set_proc_pool(je_proc_pool *procs)
{
    kep_init_once();
    g_kep.procs = procs;
}
/* Forward decl: peer-close wake helper (defined with the bridge functions). */
static void kern_peer_close_wake(je_ep_channel *rec, uint8_t side);

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
    /* A6-PEERWAKE: the release walk may have transitioned surviving peer ends to
     * PEER_CLOSED. Wake any receiver blocked on such an end (empty inbound ->
     * PEER_CLOSED; a nonempty end was already WOKEN by its enqueue, drain-first).
     * The walk closes via the pure handle_close (no wake), so do it here. */
    for (uint16_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        if (!pool->rec[i].allocated) continue;
        for (uint8_t s = 0; s < 2u; ++s) kern_peer_close_wake(&pool->rec[i], s);
    }
}

bool je_endpoint_kern_is_wired(const je_vm *vm)
{
    return vm != (const je_vm *)0
        && vm->teardown == kep_teardown
        && vm->teardown_ctx == &g_kep
        && vm->irq_save != (uint64_t (*)(void))0
        && vm->irq_restore != (void (*)(uint64_t))0;
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
    if (!je_endpoint_kern_is_wired(vm))
        return JE_EP_CORRUPT;
    kep_init_once();
    je_handle_table *t = &vm->procs[index].htable;
    je_proc_id creator = JE_PROC_ID_NONE;
    extern je_status je_sched_current_proc_id(je_proc_id *out);
    extern je_status je_sched_current_user(je_vm **out_vm, uint32_t *out_index);
    je_vm *caller_vm = (je_vm *)0;
    uint32_t caller_index = 0u;
    if (je_sched_current_user(&caller_vm, &caller_index) == JE_OK) {
        if (caller_vm != vm || caller_index != index
            || je_sched_current_proc_id(&creator) != JE_OK
            || creator.gen == 0u)
            return JE_EP_CORRUPT;
    }
    return je_endpoint_channel_create_by(&g_kep, creator, t, JE_EP_RIGHTS_ALL,
                                         t, JE_EP_RIGHTS_ALL, out0, out1);
}

je_ep_status je_endpoint_kern_dup(je_vm *vm, uint32_t index, je_handle src,
                                  uint32_t rights, je_handle *out)
{
    if (out) *out = 0u;
    if (!vm || index >= JE_VM_MAX_PROCESSES || !out) return JE_EP_BADINDEX;
    if (!je_endpoint_kern_is_wired(vm)) return JE_EP_CORRUPT;
    kep_init_once();
    return je_endpoint_handle_dup(&g_kep, &vm->procs[index].htable, src, rights, out);
}

je_ep_status je_endpoint_kern_close(je_vm *vm, uint32_t index, je_handle h)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    /* Capture the end being closed BEFORE the close invalidates the handle, so we
     * can wake a receiver blocked on the PEER end if this close peer-closes it
     * (A6-PEERWAKE). A resolve failure just means the normal close path reports it. */
    je_ep_channel *rec = (je_ep_channel *)0; uint8_t side = 0u;
    bool have = (je_endpoint_resolve(&g_kep, &vm->procs[index].htable, h, &rec, &side)
                 == JE_EP_OK);
    je_ep_status s = je_endpoint_handle_close(&g_kep, &vm->procs[index].htable, h);
    if (s == JE_EP_OK && have)
        kern_peer_close_wake(rec, (uint8_t)(side ^ 1u));   /* peer end */
    return s;
}

/* Data IPC forwarders (M3c step 4): resolve the caller's table and run the pure
 * send/peek/commit against the kernel pool. The glue (syscall.c) does the
 * copy_from/to_user around these, honoring D6 ordering. */
je_ep_status je_endpoint_kern_send(je_vm *vm, uint32_t index, je_handle h,
                                   const void *kbuf, size_t len)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    je_ep_status s = je_endpoint_try_send(&g_kep, &vm->procs[index].htable, h, kbuf, len);
    if (s == JE_EP_OK) je_endpoint_kern_send_wake(vm, index, h);   /* A6: wake peer waiter */
    return s;
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
    je_ep_status s = je_endpoint_try_send_caps(&g_kep, &vm->procs[index].htable, h,
                                               kbuf, len, caps, ncaps);
    if (s == JE_EP_OK) je_endpoint_kern_send_wake(vm, index, h);   /* A6: wake peer waiter */
    return s;
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
    if (!je_endpoint_kern_is_wired(vm)) return JE_EP_CORRUPT;
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

bool je_endpoint_kern_quiescent(void)
{
    if (!g_kep_inited) return true;
    for (uint32_t i = 0u; i < JE_EP_POOL_CAP; ++i) {
        const je_ep_channel *rec = &g_kep.rec[i];
        if (rec->allocated) return false;
        for (uint8_t side = 0u; side < 2u; ++side) {
            const je_ep_end *end = &rec->end[side];
            if (end->handle_refs != 0u || end->inflight_refs != 0u
                || end->state != JE_EP_END_FREE || end->waiter.present
                || end->waiter.head_reserved || end->inbound.count != 0u)
                return false;
            for (uint8_t q = 0u; q < JE_EP_QUEUE_DEPTH; ++q)
                if (end->inbound.cap_count[q] != 0u)
                    return false;
        }
    }
    return true;
}

/* ---- M3c D7/A6 blocking-recv bridge (ties the pure waiter to the scheduler) ---- */

/* A6-CLARIFY-2: the single release owner, called by sched.c mark_dying with the
 * dying thread's slot (= waiter_id). Releases any waiter/reservation it holds. */
bool je_endpoint_kern_waiter_release_by_id(uint32_t slot)
{
    if (!g_kep_inited) return false;
    bool woken_reserved = false;
    for (uint16_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        if (!g_kep.rec[i].allocated) continue;
        for (uint8_t s = 0; s < 2u; ++s) {
            const je_ep_waiter *waiter = &g_kep.rec[i].end[s].waiter;
            if (waiter->present && waiter->waiter_id == slot
                && waiter->cause == JE_EP_WAIT_WOKEN
                && waiter->head_reserved) {
                woken_reserved = true;
                break;
            }
        }
        if (woken_reserved) break;
    }
    bool released = je_endpoint_wait_release_by_id(&g_kep, slot);
    if (released && woken_reserved) ++g_woken_reservations_released;
    return released;
}

/* Entry probe + waiter register for the recv glue. */
je_ep_status je_endpoint_kern_recv_entry_probe(je_vm *vm, uint32_t index, je_handle h)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_recv_entry_probe(&g_kep, &vm->procs[index].htable, h);
}

je_ep_status je_endpoint_kern_wait_register(je_vm *vm, uint32_t index, je_handle h,
                                            uint32_t waiter_id,
                                            je_ep_channel **out_rec, uint8_t *out_side)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES) return JE_EP_BADINDEX;
    kep_init_once();
    return je_endpoint_wait_register(&g_kep, &vm->procs[index].htable, h,
                                     waiter_id, out_rec, out_side);
}

/* Wake a receiver blocked on the PEER of the sender's end after a successful send.
 * A message sent on end `h` (sender's own end) lands in the PEER end's inbound, so
 * the waiter to wake is on the peer end. Arbitrate WOKEN there; on WON wake the
 * scheduler slot. Idempotent/no-op if no waiter or already terminal. Call AFTER a
 * send returned JE_EP_OK, under IF=0. Resolving `h` here cannot fail (it just
 * succeeded in the send). */
void je_endpoint_kern_send_wake(je_vm *vm, uint32_t index, je_handle h)
{
    if (!vm || index >= JE_VM_MAX_PROCESSES || !g_kep_inited) return;
    je_ep_channel *rec; uint8_t side;
    if (je_endpoint_resolve(&g_kep, &vm->procs[index].htable, h, &rec, &side) != JE_EP_OK)
        return;
    uint32_t id = 0u;
    if (je_endpoint_wait_arbitrate(rec, (uint8_t)(side ^ 1u), JE_EP_WAIT_WOKEN, &id)
        == JE_EP_ARB_WON)
        je_sched_wake_ipc(id);
}

/* Wake a receiver blocked on an end that just became PEER_CLOSED (A6-PEERWAKE),
 * when that end's inbound is EMPTY (drain-first A6-1: a non-empty end was already
 * woken WOKEN by its enqueue). `rec,side` name the end that transitioned to
 * PEER_CLOSED. Arbitrate PEER_CLOSED + wake. Under IF=0. */
static void kern_peer_close_wake(je_ep_channel *rec, uint8_t side)
{
    if (!rec) return;
    if (je_endpoint_end_state(rec, side) != JE_EP_END_PEER_CLOSED) return;
    if (je_endpoint_inbound_count(rec, side) != 0u) return;   /* drain-first */
    uint32_t id = 0u;
    if (je_endpoint_wait_arbitrate(rec, side, JE_EP_WAIT_PEER_CLOSED, &id)
        == JE_EP_ARB_WON)
        je_sched_wake_ipc(id);
}

/* Internal cancel (A6-2; no ABI path in v1). Arbitrate CANCELLED on the waiter
 * owned by `slot` wherever it is parked, and wake it. Used only by the dev-gated
 * cancel/forced-kill test hook. Returns true iff a waiter was cancelled. */
bool je_endpoint_kern_wait_cancel(uint32_t slot)
{
    if (!g_kep_inited) return false;
    for (uint16_t i = 0; i < JE_EP_POOL_CAP; ++i) {
        if (!g_kep.rec[i].allocated) continue;
        for (uint8_t s = 0; s < 2u; ++s) {
            /* Only cancel the waiter OWNED by `slot` — check identity BEFORE the
             * terminal CAS, so we never stamp an unrelated waiter CANCELLED. */
            uint32_t wid = 0u;
            if (!je_endpoint_wait_id(&g_kep.rec[i], s, &wid)) continue;
            if (wid != slot) continue;
            uint32_t id = 0u;
            if (je_endpoint_wait_arbitrate(&g_kep.rec[i], s, JE_EP_WAIT_CANCELLED, &id)
                == JE_EP_ARB_WON) { je_sched_wake_ipc(id); return true; }
            return false;   /* found but already terminal */
        }
    }
    return false;
}

/* The continuation's reserved-head read: finish a WOKEN recv from the OWNED
 * rec/side (never re-resolves the handle — A6-5). Peeks the reserved head
 * (bytes + cap descriptors) into the out buffers, returns the lease for commit. */
je_ep_status je_endpoint_kern_recv_peek_owned(je_ep_channel *rec, uint8_t side,
                                              void *out_kbuf, size_t cap,
                                              size_t *out_len, uint8_t hcap,
                                              je_ep_capdesc *out_caps, uint8_t *out_ncaps,
                                              je_ep_recv_lease *lease)
{
    kep_init_once();
    return je_endpoint_recv_peek_owned_caps(&g_kep, rec, side, out_kbuf, cap,
                                            out_len, hcap, out_caps, out_ncaps, lease);
}

/* Release the waiter + reservation on an owned end after a successful consume
 * (the continuation calls this post-commit). */
void je_endpoint_kern_wait_release(je_ep_channel *rec, uint8_t side, uint32_t who)
{
    (void)je_endpoint_wait_release(&g_kep, rec, side, who);
}

uint32_t je_endpoint_kern_woken_reservations_released(void)
{
    return g_woken_reservations_released;
}
