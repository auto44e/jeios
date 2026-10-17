/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M3a #5 step (b): bounded, recoverable user copies (ADR 0007 D9). Prevalidates
 * every page of a user span (present + correct permission) BEFORE touching it,
 * then copies through the single approved assembly site (usercopy.S). An
 * exception-fixup net recovers a not-present (#PF P=0) fault AT THE EXACT copy
 * instruction, with an active owner-matched copy scope and CR2 inside the
 * declared span; EVERY other kernel #PF stays fatal.
 *
 * Reconciliation of prevalidation + fixup (from the step-(b) design debate):
 * in M3a all pages are resident and immutable during a syscall and there is one
 * thread per process, so a prevalidated copy should not fault. The fixup is the
 * fail-closed net: a recovered not-present fault means a validator/scope bug and
 * is reported as a copy failure (INVALID); a PROTECTION fault (P=1), reserved,
 * instruction-fetch, or any other PFEC is NOT recovered — it stays fatal, so a
 * real invariant break fail-stops instead of being masked.
 *
 * Honest limits: SMAP/SMEP are not enabled in M3a, so this net covers faults
 * from the approved site only, not stray kernel dereferences of mapped user
 * addresses; and "never recovers an unrelated fault" holds under the stated
 * invariants (uncorrupted kernel state), not against arbitrary prior corruption.
 */
#include <jeios/usercopy.h>
#include <jeios/abi.h>
#include <jeios/sched.h>
#include <jeios/vm.h>
#include <jeios/paging.h>

/* Exact copy-site RIPs + the fixup entry (usercopy.S). */
extern unsigned char je_copy_user_load_rip[], je_copy_user_store_rip[];
extern unsigned char je_copy_user_fixup[];
int je_copy_user_run(void *dst, const void *src, uint64_t len);

/* PFEC bits (page-fault error code). */
#define PFEC_P     0x1u   /* 1 = protection violation, 0 = not-present */
#define PFEC_WRITE 0x2u   /* 1 = write access */
#define PFEC_USER  0x4u   /* 1 = CPL3 access */
#define PFEC_RSVD  0x8u
#define PFEC_FETCH 0x10u

typedef enum { COPY_NONE = 0, COPY_FROM_USER = 1, COPY_TO_USER = 2 } copy_dir;

/* The single active copy scope. One copy runs at a time (single CPU, interrupts
 * disabled, no scheduling during a copy — ADR D10), so a single record suffices.
 * `owner` pins it to the process whose span it describes. Cleared on EVERY exit
 * path (success, validation failure, fault recovery). */
static struct copy_scope {
    int       active;
    copy_dir  dir;
    uint64_t  user_lo, user_hi; /* [lo, hi) declared user span */
    je_vm    *owner_vm;
    uint32_t  owner_index;
} g_scope;

static void scope_clear(void) { struct copy_scope z = {0}; g_scope = z; }

/* ---- pointer / span validation (D9): no dereference before validation ------ */

static int canonical(uint64_t a) { uint64_t hi=a>>47; return hi==0 || hi==UINT64_C(0x1ffff); }

/* Validate a user span [addr, addr+len) structurally: canonical, no wraparound,
 * entirely inside the arena (so never a kernel range). Returns 0 on success. */
static int span_structure_ok(uint64_t addr, uint64_t len)
{
    if (len == 0) return 1;                 /* caller handles zero-length */
    if (!canonical(addr)) return 0;
    if (addr > UINT64_MAX - len) return 0;  /* wraparound (subtraction-based) */
    uint64_t end = addr + len;              /* exclusive */
    if (addr < JE_VM_ARENA_BASE) return 0;
    if (end > JE_VM_ARENA_BASE + JE_VM_ARENA_SIZE) return 0;
    return 1;
}

/* Per-page presence + permission check for the active process. `need_write`
 * requires an RW page; otherwise any readable user page is fine. Returns 0 if
 * any page in the span is not present or lacks the required permission. */
static int span_pages_ok(je_vm *vm, uint32_t index, uint64_t addr, uint64_t len, int need_write)
{
    const je_vm_ops *o = vm->ops;
    uint64_t first = (addr - JE_VM_ARENA_BASE) & ~UINT64_C(0xfff);
    uint64_t last  = (addr + len - 1 - JE_VM_ARENA_BASE) & ~UINT64_C(0xfff);
    for (uint64_t off = first; off <= last; off += JE_PAGE_SIZE) {
        uint64_t phys; je_vm_perm perm; int present;
        if (o->read_user_pte(o->context, index, off, &phys, &perm, &present) != JE_OK) return 0;
        if (!present) return 0;
        if (need_write) {
            if (perm != JE_VM_PERM_RW_NX) return 0;  /* only RW/NX is writable */
        } else {
            /* any present user leaf is readable (RX, R/NX, RW/NX all readable) */
        }
    }
    return 1;
}

/* Common copy core: structure-check, page-check, set scope, run the approved
 * assembly site, clear scope. `kptr` is kernel storage; `uaddr`/`len` is the
 * user span. Returns JE_ABI_OK or JE_ABI_INVALID; never dereferences the user
 * pointer before validation; retains no user pointer. */
static je_abi_status copy_core(copy_dir dir, void *kptr, uint64_t uaddr, uint64_t len)
{
    if (len == 0) return JE_ABI_OK;          /* touches neither pointer */
    je_vm *vm; uint32_t index;
    if (je_sched_current_user(&vm, &index) != JE_OK || !vm || !vm->ops) return JE_ABI_INVALID;
    if (!span_structure_ok(uaddr, len)) return JE_ABI_INVALID;
    int need_write = (dir == COPY_TO_USER);
    if (!span_pages_ok(vm, index, uaddr, len, need_write)) return JE_ABI_INVALID;

    g_scope.active = 1; g_scope.dir = dir;
    g_scope.user_lo = uaddr; g_scope.user_hi = uaddr + len;
    g_scope.owner_vm = vm; g_scope.owner_index = index;

    int faulted;
    if (dir == COPY_FROM_USER)
        faulted = je_copy_user_run(kptr, (const void *)(uintptr_t)uaddr, len);
    else
        faulted = je_copy_user_run((void *)(uintptr_t)uaddr, kptr, len);

    scope_clear();
    return faulted ? JE_ABI_INVALID : JE_ABI_OK;  /* a recovered fault = failure */
}

je_abi_status je_copy_from_user(void *kdst, uint64_t usrc, uint64_t len)
{
    return copy_core(COPY_FROM_USER, kdst, usrc, len);
}
je_abi_status je_copy_to_user(uint64_t udst, const void *ksrc, uint64_t len)
{
    return copy_core(COPY_TO_USER, (void *)ksrc, udst, len);
}

/* ---- exception-fixup hook (called from je_trap_dispatch on a CPL0 #PF) ------
 * Returns 1 and rewrites frame->rip to the fixup entry iff this is a recoverable
 * copy-site fault: cs==8, vector 14, PFEC is not-present (P=0) and NOT a
 * protection/reserved/fetch fault, the faulting RIP is EXACTLY one of the two
 * approved copy instructions, a copy scope is active, CR2 is inside the declared
 * span, the scope owner is the current user process, and the PFEC write bit
 * matches the copy direction (load site -> read fault; store site -> write
 * fault). Otherwise returns 0 and the caller keeps the fatal path. */
int je_usercopy_try_fixup(je_trap_frame *f, uint64_t cr2, uint64_t pfec)
{
    if (!f || f->vector != 14 || f->cs != 8) return 0;           /* CPL0 #PF only (exact kernel CS) */
    if (!g_scope.active) return 0;
    if (pfec & (PFEC_P | PFEC_RSVD | PFEC_FETCH)) return 0;        /* only not-present data faults */
    /* Exact RIP equality against the two approved sites (never a range match). */
    uint64_t rip = f->rip;
    int is_load  = (rip == (uint64_t)(uintptr_t)je_copy_user_load_rip);
    int is_store = (rip == (uint64_t)(uintptr_t)je_copy_user_store_rip);
    if (!is_load && !is_store) return 0;
    /* Direction must match: a from_user copy only recovers a read fault at the
     * load site; a to_user copy only recovers a write fault at the store site. */
    if (g_scope.dir == COPY_FROM_USER) { if (!is_load  || (pfec & PFEC_WRITE)) return 0; }
    else                               { if (!is_store || !(pfec & PFEC_WRITE)) return 0; }
    /* CR2 must lie inside the declared user span, and the owner must be current. */
    if (cr2 < g_scope.user_lo || cr2 >= g_scope.user_hi) return 0;
    je_vm *vm; uint32_t index;
    if (je_sched_current_user(&vm, &index) != JE_OK) return 0;
    if (vm != g_scope.owner_vm || index != g_scope.owner_index) return 0;
    /* Recover: resume at the fixup entry, which returns 1 from je_copy_user_run. */
    f->rip = (uint64_t)(uintptr_t)je_copy_user_fixup;
    return 1;
}

/* ---- copy_probe(src, dst, len) op 3 (ADR D8) ------------------------------
 * The experimental copy-boundary test syscall mandated by M3a as the vehicle
 * that exercises copy_from/to_user. "Development/experimental" means an
 * unrestricted test op, NOT a capability-gated production endpoint; it is present
 * in M3a builds (not compile-excluded). The separate op-4 fixup injection IS
 * compile-guarded (JE_DEV_COPY_INJECT). Validate both spans (<=256), stage src
 * into a kernel scratch buffer via copy_from_user, then copy_to_user into dst.
 * Accepts overlap (fully staged before output). Returns JE_ABI_OK or
 * JE_ABI_INVALID; never commits output on a failed input stage. */
#define JE_COPY_PROBE_MAX 256u
je_abi_status je_copy_probe(uint64_t usrc, uint64_t udst, uint64_t len)
{
    static unsigned char scratch[JE_COPY_PROBE_MAX];
    if (len > JE_COPY_PROBE_MAX) return JE_ABI_INVALID;
    if (len == 0) return JE_ABI_OK;                      /* touches neither pointer */
    if (je_copy_from_user(scratch, usrc, len) != JE_ABI_OK) return JE_ABI_INVALID;
    if (je_copy_to_user(udst, scratch, len) != JE_ABI_OK) return JE_ABI_INVALID;
    return JE_ABI_OK;
}

#ifdef JE_DEV_COPY_INJECT
/* DEV-ONLY (compile-guarded, excluded from the default build): deliberately
 * trigger a not-present #PF at the approved copy site to prove the exception
 * fixup RECOVERS it rather than panicking. Runs in the current user process's
 * syscall context (so the scope owner/CR2/RIP/PFEC checks are all genuine),
 * sets a scope over `uaddr` (which the caller guarantees is an UNMAPPED arena
 * page), raw-calls je_copy_user_run with NO prevalidation, and returns 1 iff the
 * fixup recovered. Not reachable from copy_probe (which always prevalidates). */
int je_copy_fixup_inject(uint64_t uaddr)
{
    static unsigned char scratch[1];
    je_vm *vm; uint32_t index;
    if (je_sched_current_user(&vm, &index) != JE_OK || !vm || !vm->ops) return 0;
    /* Confirm the target really is not-present, so we prove a P=0 recovery. */
    uint64_t off = uaddr - JE_VM_ARENA_BASE, phys; je_vm_perm perm; int present;
    if (vm->ops->read_user_pte(vm->ops->context, index, off & ~UINT64_C(0xfff),
                               &phys, &perm, &present) != JE_OK) return 0;
    if (present) return 0;  /* must be a not-present page for a P=0 fault */

    /* (1) LOAD-side recovery: copy_from_user reads the unmapped user page. */
    g_scope.active = 1; g_scope.dir = COPY_FROM_USER;
    g_scope.user_lo = uaddr; g_scope.user_hi = uaddr + 1;
    g_scope.owner_vm = vm; g_scope.owner_index = index;
    int load_recovered = je_copy_user_run(scratch, (const void *)(uintptr_t)uaddr, 1);
    scope_clear();

    /* (2) STORE-side recovery: copy_to_user writes the unmapped user page. */
    g_scope.active = 1; g_scope.dir = COPY_TO_USER;
    g_scope.user_lo = uaddr; g_scope.user_hi = uaddr + 1;
    g_scope.owner_vm = vm; g_scope.owner_index = index;
    int store_recovered = je_copy_user_run((void *)(uintptr_t)uaddr, scratch, 1);
    scope_clear();

    return load_recovered && store_recovered; /* both exact sites + directions recovered */
}
#endif
