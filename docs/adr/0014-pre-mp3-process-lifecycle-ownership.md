# ADR 0014: Pre-MP3 process-lifecycle ownership (decision record, not an implementation)

Status: **accepted / implementation authorized** (owner approval 2026-11-08;
originally proposed 2026-11-06 as fix-gate F13). The owner approved the bounded
lifecycle-only extraction (Option A) as its own dedicated PR, to land BEFORE MP3
so the ARM userspace path rebases onto it. The run-queue migration to
`sched_rr.c` (ADR 0013) is still **not** authorized here — only the lifecycle
extraction in parts 1–3 and 5 below. See the 2026-11-08 amendment at the end for
the owner-approved wording corrections, the two-arch gate reading, and the two
guards deferred to MP3 entry criteria.

Supersedes the trigger-less "migrate later" flex in ADR 0013 (see the amendment
appended to `0013-mp2-aarch64-scheduling.md`).

## Context (verified ground truth, 2026-11-06, core `main` b28049a)

The process lifecycle currently lives **inside x86-coupled `kernel/core/sched.c`**:
process registration/identity, thread↔VM link, dying, reaping, VM and kstack
lifetime, and the arch operations embedded in the scheduler and in `device.c`.
Concretely (sched.c at b28049a):

- **Registry/identity:** `registered_vm[]` / `registered_proc[]` / `process_pool`
  (sched.c:58-61); `je_sched_set_proc_pool`, `je_sched_register_process`,
  `je_sched_unregister_process`, `je_sched_has_process`, `je_sched_current_proc_id`.
- **Thread/VM link + kstack:** `je_thread_create_user_start` (guards the VM-held
  kstack, builds the x86 `je_trap_frame`, calls `je_proc_link`); the kstack
  "belongs to the vm process and is freed by `je_vm_destroy`, never by
  `je_sched_reap`" (sched.c:170-173).
- **Dying:** `mark_dying` (sole waiter release via
  `je_endpoint_kern_waiter_release_by_id`, device death hooks, `je_proc_on_dying`,
  x86 death record).
- **Reap:** `je_sched_reap`, `je_sched_drop_user`, `je_sched_reap_dead`.
- **x86 couplings:** CR3 load, `TSS.RSP0` set, `int $0x81` (yield/exit),
  `sti; hlt` (idle), RFLAGS.IF checks, the `je_trap_frame` cs/ss/rflags and cr2.
  `device.c` also calls `je_arch_tss_set_com2_io` (x86 TSS I/O bitmap) from a
  core module.

ADR 0013 (accepted) scoped the MP2 ARM scheduler as a NEW pure
`kernel/core/sched_rr.c`, leaving x86 `sched.c` **untouched this PR**, with a
**trigger-less** flex: "migrate `sched.c` onto `sched_rr.c` (later)". The
2026-11 ecosystem audit (Q3) and its owner-reviewed response established that
because the lifecycle lives inside `sched.c`, **ARM MP3 forces a tested-`sched.c`
change either way** — so a bounded lifecycle-only extraction should precede MP3,
and ADR 0013's flex needs a real trigger. `docs/HANDOFF.md` records: "Lifecycle
extraction is a proposal requiring owner review, not authorization to change
tested paths."

## Decision (+ why)

**1. Scope — lifecycle only, no run-queue change.** A future owner-approved,
bounded change MAY extract the process-lifecycle ownership listed above into a
shared, arch-neutral module with a **single ownership boundary**, used by both the
existing x86 `sched.c` run queue and the ARM `sched_rr.c` path. It covers:
process registration/identity, thread↔VM link, dying/reaping, VM and kstack
lifetime, and the sole-waiter-release rule. It does **NOT** change the x86 run
queue, does **NOT** implement or migrate to `sched_rr.c`, and does **NOT** add
PCID/SMP. This boundary is what stops "an unbounded scheduler rewrite
masquerading as a bug fix" (the gate's words).

**2. Single ownership boundary.** The extracted module owns `registered_vm` /
`registered_proc` / `process_pool`, the kstack-ownership rule (freed by
`je_vm_destroy`, never by reap), and the waiter-release rule. The release rule,
corrected (2026-11-08 amendment): there is **one release function**
(`je_endpoint_kern_waiter_release_by_id`, wrapped as the module's
`je_lc_release_waiter`) with **two idempotent call sites** — the death path
(`mark_dying`) and the live-teardown path (`je_sched_drop_user`) — and it takes
**effect once** (a second call on an already-released slot is a no-op). The
earlier "`mark_dying` is the sole waiter release" wording was inaccurate:
`je_sched_drop_user` also releases, and is the only release when spawn or the
harness tears down a live thread. Arch specifics (CR3/TSS install, the
IF/liveness guards, the trap instruction, idle `hlt`, the TSS COM2 I/O bitmap)
stay behind arch seams — and, corrected (2026-11-08 amendment), the direction is:
**the per-arch run-queue caller invokes the neutral module; the module never
reads run-queue or architectural state** (no `je_trap_frame`, CR3, TSS.RSP0,
RFLAGS, `int 0x80/0x81`, `hlt`, or run-queue slot state in neutral code). This is
the link-time-port boundary the 2026-11-08 design review settled.

**3. No-regression gates (required before any such change merges).** Unchanged
x86 `make check`, `make smoke`, `make integration`, every `JE_TEST_MODE` profile,
`test_sched_killall`, the M3/M4 integration proofs, and a shared lifecycle
conformance suite. D3 gate reading, clarified (2026-11-08 amendment): the
conformance suite runs the **neutral module (with recording fakes) and the x86
binding now**; the **ARM binding is recorded as an MP3 entry criterion**, because
ARM has no userspace/lifecycle path to bind to until MP3 builds one. A green diff
that changes any x86 marker, or that turns any `sched.c` mutant BLOCKED (the six
REAPER-*/F08 anchors must stay killed), is rejected.

**4. Explicit migration trigger (the thing ADR 0013 lacked).** The x86
`sched.c` run queue migrates onto `sched_rr.c` only when **either** (a) the next
scheduler *feature* lands that would otherwise be implemented twice (e.g.
cooperative yield, priorities, or SMP readiness), **or** (b) any **cross-arch
parity claim** is made (x86 and ARM asserting the same scheduler contract). Until
one of those fires, the two run queues stay separate and the lifecycle module is
the only shared surface. The migration itself is a further owner-approved ADR,
not authorized here.

**5. Disposition: AUTHORIZED (2026-11-08), was DEFERRED/pre-MP3-blocker.** As
first recorded this was a reviewed design, not authorized implementation, held as
a **pre-MP3 blocker**. The owner approved the extraction on 2026-11-08 (Option A):
it is now authorized as its own dedicated PR, to land BEFORE MP3 so the ARM
userspace path rebases onto it. MP3 (ARM userspace/IPC) still MUST NOT begin until
this extraction is merged. The run-queue migration to `sched_rr.c` remains
unauthorized (part 4 trigger + a future ADR).

Why this shape: it converts a trigger-less "later" into a bounded contract with a
named boundary, named gates, and a named trigger — so the eventual change is
reviewable against this record rather than open-ended, and so MP3 cannot silently
drag `sched.c` along without the owner's sign-off.

## Predictions if other choices were made

- **If we implemented the extraction now (no owner gate):** it would touch the
  M1/M2-tested `sched.c` and the M3/M4 lifecycle on agent initiative — exactly the
  "frozen/tested path changed by agent majority" the AGENTS.md hard-override
  forbids. The risk is a large, hard-to-review diff landing without the owner
  weighing the ownership boundary; deferring keeps the decision cheap to review
  and the code untouched until approved.
- **If we migrated `sched.c` → `sched_rr.c` now (the ADR 0013 flex, eagerly):**
  it would merge two run queues under one implementation before any feature needs
  it, adding integration churn and a second tested-path rewrite on top of the MP2
  ARM work, with no functional gain yet. The trigger (part 4) defers that cost to
  when it buys something.
- **If we left ADR 0013's flex trigger-less (do nothing):** MP3 would arrive and
  force an ad-hoc `sched.c` change under schedule pressure, with no agreed
  boundary or gates — the unbounded-refactor-as-bugfix risk the gate calls out.
- **If the lifecycle module also absorbed the run queue (not "lifecycle only"):**
  the boundary would be unbounded and the review impossible to scope; restricting
  to lifecycle keeps part 1's promise checkable.

## Flex options to consider in future

- **FLEX (planning review):** the exact module name/location of the extracted
  lifecycle owner, and whether `device.c`'s TSS-COM2 coupling is pulled behind the
  same arch seam or a separate one, are revisitable with the planning agent when
  the extraction is actually designed. The *intent* (one ownership boundary,
  lifecycle only, arch behind seams) is firm.
- **FLEX (planning review):** the migration trigger (part 4) may be tightened or
  relaxed — e.g. if SMP is pulled forward, (a) fires sooner. The *existence* of a
  named trigger is firm; its exact conditions are revisitable.
- Reopening requires a new ADR (the migration ADR) or an owner amendment here; an
  agent may not begin the extraction on this record alone.

## Attached correspondence

The lifecycle-extraction requirement and the ADR 0013 trigger amendment were
settled in the 2026-11 ecosystem audit and its debate: see
`docs/audit/2026-11-ecosystem-audit.md` (Q3 and the owner-escalation list),
`docs/audit/2026-11-audit-response.md`, and the debate transcript
`docs/audit/debate/01-debate-transcript.md`. No new peer-debate was run for this
record; it transcribes the already-settled audit conclusion into the proper ADR
form per fix-gate F13. AI-assisted recording, not independent human qualification;
owner approval is still required to authorize the implementation.

## Amendment — 2026-11-08 (owner-approved: design, wording, gate, deferrals)

The owner approved the extraction (Option A) and the four calls below. This
amendment records the settled design and the owner's rulings; it does not change
parts 1 and 4's intent.

### Design settled: link-time port, not a runtime ops table

A two-lineage design review plus a tie-break debate (transcripts:
`docs/adr/debates/0014-lifecycle-extraction/`) settled the extraction shape. The
**decisive, verified** constraint: `tests/test_sched_killall.c` `#include`s
`kernel/core/sched.c` and reads its internals white-box, and the six `sched.c`
mutants (`REAPER-SIGN`, `REAPER-IF-CHECK`, `REAPER-CR3-GUARD`, `REAPER-RSP0-GUARD`,
`REAPER-VM-INDEX`, `F08-ONE-THREAD`) anchor on exact text inside `reap_dead` /
`je_sched_reap_dead` / `je_thread_create_user_start`. Moving that text out of
`sched.c` turns those mutants BLOCKED (CI `mutants` job fails; invisible to
`make check`). Therefore the mutant-anchored code and the white-box test's reach
**stay in `sched.c` byte-identical**, and only the truly-neutral ownership moves.

- **Moves to new `kernel/core/lifecycle.c` + `include/jeios/lifecycle.h`** (plain
  extern functions, no function-pointer table; `lifecycle.h` must not include
  `sched.h`): the registry state and its writers; `je_lc_release_waiter`; the
  `je_lc_on_dying` side-effect ordering (release → device fault-note on fault
  deaths → device dying → `je_proc_on_dying`); the kill D8 checks that use only
  the PROCESS record; `destroy_owned → on_reaped`.
- **Stays in `sched.c`:** `threads[]`/`current`/`ready`, the death-record writes,
  the whole `reap_dead` loop and all five reap anchors, the kill/kill_all state
  gates and F08 scan, the initial-frame synthesis, `abort_scheduler`, and every
  CR3/RSP0/IF/frame guard. The `je_sched_*` names are kept as thin wrappers (no
  rename — the callers are numerous and spanning tested paths). The slot
  `{vm, vm_index, proc_id}` triple is a synchronized cache (one writer = thread
  creation), never a second authority.
- Neutral functions return a `je_status`; `sched.c` keeps calling
  `abort_scheduler` (console text and the hosted longjmp harness unchanged).

### Owner rulings

1. **Release-rule wording (approved):** corrected in part 2 — one release
   function, two idempotent call sites (dying + drop), effective once.
2. **Seam-direction wording (approved):** corrected in part 2 — the per-arch
   run-queue caller invokes the neutral module; the module never reads run-queue
   or architectural state.
3. **D3 two-arch gate (approved reading):** clarified in part 3 — neutral module
   (with fakes) + x86 binding run now; the ARM binding is an MP3 entry criterion.
4. **Two guards deferred to MP3 entry criteria (owner took the recommendation):**
   - **(a) ARM reap must not free a VM-owned user kstack.** The ARM run queue
     `je_rr_reap` (`kernel/core/sched_rr.c`) always calls `stack_free`, and the
     ARM `stack_free` unmaps and frees — which would re-introduce the M3a
     user-kstack double-free that the x86 reaper's "clear the user slot, never
     free its stack" rule prevents. Not fixed here (it touches the #101-tested
     `sched_rr.c`); recorded as an **MP3 entry criterion**: before an ARM user
     thread can be reaped, the ARM path must mark a VM-owned stack as not
     run-queue-freed, proven by a `test_sched_rr` case (0 `stack_free`, exactly 1
     VM destroy) plus a mutant that deletes the skip.
   - **(b) Reaper slot-cache cross-check.** `je_sched_kill`/`kill_all` cross-check
     the slot against `je_proc_run_link`, but `je_sched_reap_dead` does not. Not
     added here (it would change a mutant-anchored tested path and is not needed
     for the extraction); recorded as an **MP3 entry criterion / NOT enforced
     yet**, to be added as its own guarded commit with its own mutant when the
     ARM reaper path is built.

### Flex note — deferred run-queue migration (per owner's standing pattern)

The rejected-now alternative is the eager `sched.c` → `sched_rr.c` run-queue
**migration**. It stays deferred under part 4's trigger: it reopens only when
either (a) the next scheduler feature would otherwise be built twice (cooperative
yield, priorities, SMP readiness) or (b) a cross-arch scheduler-parity claim is
made — and then only via a further owner-approved migration ADR. This extraction
deliberately does not migrate the run queue; the two run queues stay separate and
the neutral lifecycle module is the only shared surface.

AI-assisted recording and review; not independent human qualification.
