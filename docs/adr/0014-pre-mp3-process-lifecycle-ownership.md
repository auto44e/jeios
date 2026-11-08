# ADR 0014: Pre-MP3 process-lifecycle ownership (decision record, not an implementation)

Status: **proposed / owner-gated** (2026-11-06). Recorded as fix-gate F13
requires: a bounded lifecycle-ownership decision + an explicit migration trigger,
captured in a reviewed contract BEFORE MP3. This ADR records the DECISION and the
trigger; it authorizes **no code change** by itself. The run-queue migration to
`sched_rr.c` (ADR 0013) is **not** implemented or authorized here. Owner approval
is required before any of the implementation described below is written; until
then this stands as a **pre-MP3 blocker**.

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
`je_vm_destroy`, never by reap), and the release-owner rules (`mark_dying` is the
sole waiter release). Arch specifics (CR3/TSS install, the IF/liveness guards, the
trap instruction, idle `hlt`, the TSS COM2 I/O bitmap) stay behind arch seams the
module calls — not embedded in the neutral lifecycle code.

**3. No-regression gates (required before any such change merges).** Unchanged
x86 `make check`, `make smoke`, `make integration`, every `JE_TEST_MODE` profile,
`test_sched_killall`, the M3/M4 integration proofs, and a shared lifecycle
conformance suite exercised by both the x86 and ARM paths. A green diff that
changes any of these markers is rejected.

**4. Explicit migration trigger (the thing ADR 0013 lacked).** The x86
`sched.c` run queue migrates onto `sched_rr.c` only when **either** (a) the next
scheduler *feature* lands that would otherwise be implemented twice (e.g.
cooperative yield, priorities, or SMP readiness), **or** (b) any **cross-arch
parity claim** is made (x86 and ARM asserting the same scheduler contract). Until
one of those fires, the two run queues stay separate and the lifecycle module is
the only shared surface. The migration itself is a further owner-approved ADR,
not authorized here.

**5. Disposition now: DEFERRED, kept as a pre-MP3 blocker.** The extraction is a
reviewed design, not authorized implementation. It is recorded as a **pre-MP3
blocker**: MP3 (ARM userspace/IPC) MUST NOT begin until the owner approves and the
extraction (or an explicit owner waiver) is in place, because MP3 is the point
that forces the tested-`sched.c` change this ADR front-runs.

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
