# Audit continuation: agent work packages

Owner request: 2026-11-14 (America/New_York). Baseline core main
`b2b209f3863a27cf25d746547038f4985bf7e984`; PR #107 is merged.
State: **planning/handoff only; pending items are not implemented by this PR**.
Tracking: [core #87](https://github.com/auto44e/jeios/issues/87),
[#18](https://github.com/auto44e/jeios/issues/18),
[#19](https://github.com/auto44e/jeios/issues/19),
[mobile #2](https://github.com/auto44e/jeios-mobile/issues/2).

## Start here

Read AGENTS.md, the current implementation, frozen ADRs, and
[the closure ledger](2026-11-fix-gate-closure.md). The original F01–F14 gate was
closed with fixes and accepted dispositions (#94/#97–#100). Do not reopen every
row or repeat completed work. MP2 scheduling #101, lifecycle extraction #102,
ARM EL0 #103/#104, typed handles #105 and IPC #106 are merged. Old entrypoint
instructions that still say to implement MP2 or await ADR 0014 are historical.

Use separate bounded PRs in the order A0, A1–A3, then A5. A4 cleanup may proceed
separately after characterization tests; A6–A8 are qualification/design tracks,
not permission to implement SMP or buy/flash hardware. Preserve current ABI,
error precedence and ownership invariants. Existing owner-review requirements
for tested paths/frozen contracts still apply; prepare concrete changes for review.

## A0 — exact-revision evidence and status reconciliation

Inspect latest main and concurrent branches before starting. At this observation,
main workflow [43607930996](https://github.com/auto44e/jeios/actions/runs/43607930996)
is running, not yet a PASS. The cancelled Nov 10 run is historical, not the current
run. Wait for/check completion, investigate failed or cancelled required jobs and
obtain complete hosted, AMD64 integration and applicable ARM matrix evidence at
one exact revision. Do not substitute an earlier green revision. Record skipped
jobs and infrastructure failures separately. Do not disable checks to close this.

Acceptance: linked exact-SHA CI jobs/artifacts, commands/configurations, local vs
remote evidence, and refreshed STATUS/HANDOFF/issue #87. Reconcile source changes
since the closure ledger; mark obsolete follow-ups with source/test evidence.

## A1 — endpoint waiter lifetime mutation coverage

Reinspect `GAP-EP-WAIT-INFLIGHT` in tools/mutants.json and current endpoint,
scheduler and lifecycle code, including MP3c. Add a killer using the production
wait-registration/release path: a parked waiter holds the record after both public
handles close; release on wake, timeout, cancellation or process death is exactly
once; slot reuse cannot revive a stale waiter. Assert identities/generations and
allocation/reclamation state, not merely an aggregate count. Cover reference
exhaustion and no partial publication.

Acceptance: pristine baseline PASS; removal of the protecting lifetime increment
fires the intended detector and is KILLED, not compile failure or infrastructure
failure. Enable the gap entry only after that evidence exists. Do not describe the
old surviving mutation as a proven UAF in unmodified current main.

## A2 — x86 interrupt-routing test seam

Characterize production timer/yield/IRQ3/IRQ7 dispatch and unexpected/spurious
paths before adding a narrow hosted seam. Assert correct handler, vector,
acknowledgement ownership, state transition and absence of unauthorized device
access. Keep real interrupt entry/return and CPU ordering claims in QEMU tests.

Acceptance: positive/negative routing tests, deliberate wrong-route mutations,
existing x86 integration tests retained, and no production ABI/layout changes
hidden inside test scaffolding. Hosted fake results alone do not prove hardware.

## A3 — receive capacity and shared delivery logic

Trace every current receive path after #106, including architecture-neutral and
ARM restart paths. Re-evaluate the historical `(uint8_t)hcap` narrowing concern;
if gone, cite the exact replacement and boundary tests. Otherwise validate/clamp
in the wide type before conversion, preserving defined ABI status precedence.
Exercise 0, maximum, maximum+1 and values beyond the narrow type's range.

Characterize staged data/cap delivery, rights attenuation, peek/reservation,
copyout faults, mint rollback, consumption/commit, retries and cancellation before
extracting a shared helper. No duplicate delivery, leaked minted handles, stale
reservations or partial message consumption on failure. Distinguish syscall
restart from replay of already-committed effects.

Acceptance: both architectures' direct and blocking paths, queue/handle exhaustion,
fault-injected rollback and boundary tests. Submit defensive fixes and mechanical
helper extraction separately where that makes review clearer. Evidence-backed
no-change disposition is valid if newer code already resolves a concern.

## A4 — deferred harness/device cleanup

Prepare a behavior-preserving split of `main.c` test fixtures; separate optional
device observation state from production ownership where useful; trace apparently
unused parameters across all callers before removal. The completed ADR 0014
lifecycle extraction is not pending again. Do not alter tested layouts/markers or
expand scheduler functionality under this cleanup.

Acceptance: unchanged production behavior and all affected JE_TEST_MODE markers,
no new warnings, narrow diffs, failure tests before extraction, and complete
applicable integration profiles. Document any remaining owner-gated deferral.

## A5 — ARM service parity and experimental interface compatibility

Plan bounded ARM supervisor, broker and device-service parity against existing
AMD64 M4 behavior. Explicitly list supported ABI operations, architecture-specific
device choices and unavailable functions; do not assume x86 COM2/TSS mechanisms
transfer to ARM. Review the contract before modifying frozen semantics.

Acceptance: shared-source service fixtures where practical; capability denial,
spawn/status/kill, service death/restart, stale handles, quota and timeout tests;
one service crashes while an independent service continues on each architecture.
Compare defined results across architectures. Publish a support matrix with
exact QEMU profiles. MP3 IPC completion does not itself complete M4 service parity.

## A6 — mobile integration qualification

Work in jeios-mobile for target metadata/packaging/pin changes; shared kernel
changes remain here. Review current integration pin, readiness flags and target
contracts, then propose a specific reviewed upstream revision with reproducible
integration evidence. Keep desktop/server ARM proofs separate from mobile target
qualification, suspend/input/power requirements and physical Pixel support.

Acceptance: planning validators plus actual target execution evidence required by
the changed support tier; immutable pin and linked core CI. No readiness flag is
advanced by documentation or upstream merge alone. No flash/unlock/wipe authorized.

## A7 — SMP ownership review (design, not SMP implementation)

Inventory mutable globals, IRQ-mask assumptions, per-CPU state, run queues,
process/endpoint lifetimes, lock order, memory ordering and TLB shootdowns. Identify
where single-CPU enforcement currently protects invariants. Specify the bounded
SMP gate, prerequisite contracts and adversarial concurrency tests.

Acceptance: reviewed ownership table and proposed ADR/work packages with failure
and deadlock scenarios. Do not equate local interrupt masking with mutual exclusion
across CPUs or remove single-CPU restrictions without separate implementation proof.

## A8 — physical ARM evidence lane

Specify one candidate board/profile and reproducible boot/recovery procedure,
firmware/tool versions, memory/cache/TLB/interrupt/timer evidence, diagnostics and
failure recovery. Explain what QEMU cannot establish and what Pixel needs beyond
this lane. Choosing a test plan does not authorize purchase, flashing or key work.

Acceptance: reviewed dossier and explicit untested limits; actual hardware results
only after separately authorized execution. No inherited support for other SoCs.

## Agent delivery checklist

Each PR must name A-ID, baseline/final revision, source symbols, reproducer or
coverage gap, chosen contract, alternatives and reason, tests/mutants with actual
results, residual limits and required owner decisions. Update #87 and the evidence
ledger; retain dated history. No invented assignees, deadlines or human review.
Mark an item done only when its implementation/evidence or reviewed no-change
rationale is linked. Keep #18/#19 open for their broader assurance scopes.

This continuation does not reclassify every deferred item as a new global blocker.
Prioritize current correctness/assurance before dependent features, while retaining
the accepted closure and separate platform qualification gates.
