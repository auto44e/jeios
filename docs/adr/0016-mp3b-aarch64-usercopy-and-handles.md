# ADR 0016: MP3b — AArch64 user-copy with ELR-keyed fault fixup, and typed-handle syscalls

Status: **accepted** (owner-approved 2026-11-09). The owner approved both gated
decisions as recommended — **D1 = Option A′** (a dedicated, narrowly-authorized
returnable slot-4 copy-fixup branch with a three-state double-fault guard) and
**D2 = Option A** (ARM-local SELF-only handle glue; the shared frame-neutral
extraction and x86 convergence deferred to MP3c) — with the rejected alternatives
recorded as dated flex notes (below). As with ADR 0015, once MP3 is entirely
complete the planning agent audits the result and may adjust without reopening the
decision shape.

MP3b is the aarch64 analogue of the x86 M3b boundary: (A) bounded, recoverable
copies between kernel storage and EL0 user memory, with an exception-fixup net keyed
to the **exact** faulting instruction (ADR 0007 D9), and (B) wiring the
already-arch-neutral typed-handle table (ADR 0008; `handle.c`/`handle.h`) and the
pure `je_syscall_decode` through the ARM SVC path so an EL0 process can mint, close,
query, dup and inspect-rights of a handle and run the `copy_probe` op.

It changes previously-tested ARM code — the MP1 "a synchronous handler never
returns; a nested synchronous fault is always a double fault" invariant and the
`je_arm_in_handler` guard — under that approval, and it reuses the x86 security
*shape* of ADR 0007 D9 (single approved copy site; prevalidate every page; recover
only the exact site under an owner-matched, direction-matched, span-bounded scope;
every other kernel fault stays fatal) on ARM mechanisms.

Settled by a two-lineage pre-code design review (Claude, 12 QEMU boots with
injection + Codex), which caught four real blockers in the first design before any
code was written; transcripts under `docs/adr/debates/0016-mp3b-usercopy-handles/`.
AI-assisted review, not independent human qualification. Reviewed baseline: `main`
0cfcde0 (post-MP3a).

## Context

After MP3a, ARM erets to EL0 and services a bounded SVC ABI (x8=op, x0–x5=args,
x0=status, x1=result), but the ARM SVC glue maps only `abi_query`, `yield` and
`exit` and returns `UNSUPPORTED` for everything else; it does not copy user memory
and does not touch the handle table. The arch-neutral `handle.c` and the pure
`je_syscall_decode` are already linked into the ARM build; `kernel/core/usercopy.c`
is **not** linked on ARM, and `syscall.c` is compiled on ARM with
`-DJE_SYSCALL_HOSTED_TEST`, which excludes the entire x86-`je_trap_frame`-typed
dispatch glue (`syscall.c:213–1207`) and leaves only the pure decoder.

A user-copy fault is a **kernel-mode (EL1h) synchronous data abort** (ESR.EC=0x25,
vector 4). Today that slot routes to `je_arm_vec_common` → `bl je_arm_exception`,
which is `_Noreturn` and halts; and the double-fault guard `je_arm_in_handler` is
**already set for the whole EL0 SVC** (set in `je_arm_el0_sync_save`), so a nested
copy abort is currently caught by `je_arm_vec_common` and sent to
`je_arm_double_fault` before any C runs. Making the copy case recoverable therefore
requires touching that frozen guard/return invariant — hence the owner gate.

The ARM user arena is at `JE_VM_ARENA_BASE_AARCH64 = 0x0000_1000_0000_0000` (2^44);
`vm->ops->read_user_pte` (`arm_read_user_pte`, `vm_backend.c`) walks the process's
own software L3 table; the per-process TTBR0 root is `je_arm_user_root[slot]`.

## Decision

### D1 — Returnable copy-fixup: a dedicated slot-4 branch + a three-state guard (Option A′)

Exactly one kernel fault becomes recoverable: a not-present (translation-fault,
level 3) data abort at the **exact** approved copy instruction, taken while an
owner-matched copy scope is active while `guard==1` in a returning handler.
The current proof exercises EL0 SVC return, but the guard is not an SVC-type
tag; an exact-site fault from any returning handler context is covered.
Everything else at EL1h stays fatal and `je_arm_exception` stays `_Noreturn`.

Mechanism:

1. **Entry prefilter (stack-free asm).** On the `je_arm_vec_common` guard-hit
   branch, before any stack use, test `ELR_EL1 ∈ {je_copy_user_load_site,
   je_copy_user_store_site}` **and** the guard == 1 (exactly one nesting, in a
   returning handler). If either fails, branch to `je_arm_double_fault` unchanged. This keeps the
   double-fault guarantee intact for every fault that is not the approved copy site.
2. **Three-state guard.** `je_arm_in_handler` becomes tri-valued: 0 = not in a
   handler, 1 = in a returning handler (set by the relevant saver), 2 = inside the copy-fault
   check. The prefilter sets the guard to 2 before building the frame, so a fault
   *inside the check itself* is still a double fault. On recovery the guard is
   **restored to 1, never cleared**, so the remainder of the SVC stays guarded.
3. **Check (C).** Build the frozen 304-byte frame and call `je_arm_copy_fault(tf)`,
   which recovers only if: `vector==4`, `(spsr & 0x1f)==0x5` (EL1h), `EC==0x25`,
   `IL==1`, `ISS[24:0]` is exactly `0x007` (load) or `0x047` (store) — translation
   fault level 3 with FnV=EA=CM=S1PTW=0 — the faulting `ELR` exactly equals the load
   or store site for the scope's direction, `FAR ∈ [user_lo, user_hi)`, the scope
   owner is the current user process, **and** the live `TTBR0_EL1.BADDR` equals
   `je_arm_user_root[slot]`. On reject, the existing fatal diagnostic runs
   (non-returning). On accept, `ELR := je_copy_user_fixup`.
4. **Dedicated restore tail.** A copy-fixup-only restore path asserts `x0==sp`,
   `elr==fixup`, `(spsr & ~NZCV)==0x3c5`, restores the GPRs, `add sp,#304`, and
   `eret`. It does **not** use `je_arm_restore_selected` (which clears the guard)
   and does **not** consult `frame_shape_class` (the scheduler validator is
   untouched).

**Why.** It is the minimum change that satisfies the frozen-asm contract: the frozen
invariant gains exactly one authorized exception and the guard gains one state;
`frame_shape_class`, the restore seam, and `je_arm_exception`'s `_Noreturn` contract
are all unchanged. The exact-ELR prefilter plus guard==1 plus the full C check make a
false recovery of an unrelated EL1h abort unreachable while kernel state is intact.

**If otherwise (Option B — rejected).** Generalizing `je_arm_exception` to return an
int and letting vectors.S decide would touch the tested fatal-handler contract for
*every* exception, not just the copy site, widening the blast radius of a
security-critical path for no functional gain. Both reviewers rejected it.

**Flex (what reopens D1).** If SMP or multiple threads per process arrive, the single
global copy scope and the single-nesting guard==1 assumption must be revisited
(per-CPU scope + a per-CPU guard). If PAN/UAO is enabled later, the LDTR/STTR
EL0-permission reasoning must be re-confirmed (LDTR is EL0-checked only while UAO=0;
PAN does not affect LDTR/STTR). *(Dated flex note 2026-11-09.)*

### D2 — ARM-local SELF-only handle glue; defer the shared extraction (Option A)

MP3b adds an **ARM-local** frame-neutral glue layer that marshals the SVC registers
into `je_syscall_decode` and into SELF-only handle operations
(`handle_self`/`close`/`query`/`rights`/`dup_self`) plus `copy_probe`, writing
`x0=status`, `x1=result`. Any non-SELF object type fails closed (`WRONGTYPE`); on ARM
only SELF objects can exist and `vm->teardown` is NULL, so no endpoint/device code is
reachable or linked. The x86 `je_syscall_dispatch` and its typed close/dup lifetimes
are left **byte-for-byte unchanged**. ARM stops converting every non-RETURN/YIELD/EXIT
action to `UNSUPPORTED` and instead dispatches the approved MP3b actions.

**Why.** The x86 glue is `je_trap_frame`-typed, compiled out on ARM, and pulls in
endpoint/device symbols not linked on ARM; it has only QEMU/integration evidence
(not hosted-tested). Extracting it now would bundle an edit to tested x86 code into an
ARM feature PR. An ARM-local SELF-only layer delivers the MP3b handle surface with
zero risk to the x86 tested path and fails closed for every type MP3b does not yet
support.

**If otherwise (Option B — rejected).** Doing the shared frame-neutral extraction now
would require re-verifying the x86 handle path end-to-end inside this PR and risks a
silent behavior change to typed close/dup lifetimes that only QEMU/integration tests
would catch.

**Flex (what reopens D2).** MP3c (capability IPC) needs ENDPOINT objects on ARM, at
which point the ARM-local SELF-only glue is replaced by the shared frame-neutral
extraction, converged with the x86 path and verified with byte-identical x86
`objdump` evidence that the refactor did not change x86 codegen. *(Dated flex note
2026-11-09; reopens at MP3c.)*

## Settled by the design review (not owner-gated)

These were decided by the two-lineage review and are recorded for provenance:

- **Two copy routines, not one.** A single LDTRB/STTRB loop cannot work: the
  *kernel*-side access of an unprivileged pair is also checked at EL0 permission and
  faults. `je_copy_from_user_run` does `LDTRB` (user read) + `STRB` (kernel write);
  `je_copy_to_user_run` does `LDRB` (kernel read) + `STTRB` (user write). This gives
  one exact user-side faulting site per direction, so direction is fixed by
  construction (the scope still records direction as defense-in-depth).
- **Leaf, x0–x3 + x30 only.** The frozen frame carries the vector index and guard
  scratch in x16–x18; a returning slot-4 exception must preserve them, so the copy
  routines stay leaf and touch only x0–x3 and x30. Enforced by a `tools/` grep check
  on the `.S` file.
- **DFSC exactly 0x07.** Recover only a level-3 translation fault (`ISS[24:0]`
  `0x007`/`0x047`). The arena L0→L1→L2 chain is built at create time and never
  changes, so an in-arena L0–L2 fault indicates a wrong/corrupt root and must stay
  fatal. Access-flag and permission faults stay fatal (user leaves set AF; stores are
  prevalidated to RW). This mirrors `mmu_fault_check`.
- **Live-root check.** `arm_read_user_pte` walks the process's own software L3, not
  the live TTBR0; without a live-root check, a fault while another process's root is
  installed could be turned into a silent cross-process copy. ARM `copy_core` asserts
  `TTBR0_EL1.BADDR == je_arm_user_root[slot]` (fatal on mismatch).
- **current-user shim.** Add an arch-neutral `je_sched_current_user(vm,index)` for ARM
  (a thin wrapper over `je_arm_sched_current_user(vm,index,slot)`) that also checks
  the RR-ready state, accepts NULL out-pointers like x86, and rejects `vm==NULL` /
  `index>=JE_VM_MAX_PROCESSES`, so the same `usercopy.c` core runs on both arches.
- **Proof = one QEMU build per fatal-expected negative stage** (ARM_MMU_FAULT style).
  Dropping a check makes an injected fault *recover*, so the correct build must halt
  with an exact syndrome line and the mutant must turn that into a smoke FAIL. Each
  negative needs an input that **only** its target check rejects (ELR=site+4;
  FAR=span upper bound; owner mismatch; direction mismatch; a permission fault at the
  exact site). Positive copies use distinct non-zero patterns, cross a page, and
  include lengths 0/256/255 and an unmapped-hole recovery, counting fixups so a
  prevalidation rejection is distinguishable from a recovered fault. A returning
  copy-fixup must assert guard==1 afterward, with a mutant that returns through
  `restore_selected` (which the base proof could not otherwise detect).

## Honest limits

QEMU TCG does not model stale TLB/cache; MMU-on maintenance in the backend remains
reviewed, not observed. cortex-a72 has no PAN/UAO/HAFDBS; the LDTR/STTR EL0-permission
property holds with UAO=0 and `SCTLR.SPAN=1`. This is AI-assisted work; it is not
independent human qualification.
