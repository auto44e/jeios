# M4 PR-F cross-lineage reviews

ADR 0000 part 4 attachment for PR-F (supervisor + D12 supervisor-loss). AI-assisted
review across two model lineages; NOT independent human qualification.

## Design review (before any code)

Two adversarial design reviews (Claude + Codex) ran against `/tmp/m4f/design_brief.md`
and converged on fixes folded into the design before implementation
(`/tmp/m4f/design_decisions.md`):
- D12 detect by IDENTITY (status != LIVE), not a DYING-only scan, because
  `je_kernel_idle_step` itself reaps (a DYING scan can miss a reaped supervisor).
- The baseline must be an identity/ownership predicate, not a count/byte compare
  (PROCESS gens are monotonic; count-compare passes after leak+double-free).
- The report / EXIT-code bitmask is evidence, not attestation: success requires
  DEAD + reason EXIT + completion magic + exact bits + kernel-side corroboration.
- Cross-CR3 report read: validate the mapping (PTE present + RW) before switching
  CR3; the fault-recovering usercopy is NOT usable from slot 0.
- `je_sched_kill_all_user` is a standalone slot-0 loop, not built on je_sched_kill.
These shaped the ADR 0010 D12 note and the svc.h pre-wiring.

## Stage 1 review (kernel mechanism, Codex-authored)

Claude adversarially reviewed the Stage-1 diff (kill_all_user + svc_report_read +
the image predicate + the hosted test).

Note on scope: Claude ran against the main `feat/m4f-supervisor` checkout (clean
tree), not the worker worktree, so its finding #1 ("no tests / test file missing")
was a wrong-tree artifact — `tests/test_sched_killall.c` exists in the worktree
and passes (66 checks) under `make check`. The lead verified the test is real and
exercises the fail-stop branches. The code-level findings still applied:

- **#2 (acted):** `svc_report_read` ran the PTE precheck BEFORE `je_arch_irq_save`
  and did not assert IF=0, so race-freedom rested on "no unmap syscall exists"
  rather than on the code. Fixed: the precheck and the raw read now share one IF=0
  section, with an explicit IF=0 assertion (`fail()` on violation).
- **#3 (acted):** the reader accepted self-contradictory reports (`phase==DONE`
  with `fail_code!=0`, or `phase==FAILED` with `fail_code==0`). Fixed: reject
  unless `(phase==FAILED) == (fail_code!=NONE)`.
- **#4 (acted):** the slot-0 guard tested `threads[current].kind != THREAD_KERNEL`
  (any kernel worker passes); changed to `current != 0u` to match the sched.h
  contract wording.
- **#5/#6 (noted for Stage 2):** `d12_refuse` MUST halt on any non-OK from
  kill_all_user; Stage 2 must ensure no gen==0 (pre-M4) user threads are alive
  when the monitor runs (they would force a safe INVALID refusal).

Confirmed clean by the review (with the lead concurring after reading the code):
two-phase kill is race-free under the single IF=0 section; spare identity ordering
is correct; BLOCKED_IPC frame invariant holds; WOKEN reservations are released by
the unchanged mark_dying; je_sched_kill/mark_dying/reap_dead are byte-identical;
the seq protocol is sound on one CPU; `perm != RW_NX` is the right exact check;
the image predicate relies on vm.c's page-granular overlap rejection.

Verification after fixes (worktree): `make check` (test_sched_killall 66 checks,
test_mkimage 562, test_image 88, all hosted suites) PASS; `make smoke` PASS.
Local only; remote CI reported separately on the PR.


## Stage 2 review (supervisor + children + D12 monitor) — blockers found

Both lineages (Claude + Codex) reviewed the Stage-2 diff and converged on blockers.
The D12 LOSS path and the identity baseline are structurally sound; the DEFAULT
SUCCESS attestation proved far less than it claimed. Root enabler: the monitor
does not reap during the run, so every dead child retains its VM slot until
teardown — which (a) fills all 8 VM slots by ARM time and (b) prevents the kernel
from distinguishing children by image.

Blockers (acted on in the Stage-2 fix pass):
- **B1 unbounded monitor.** A stuck/stalled supervisor loops `sti;hlt` forever
  with no watchdog. Add a wrap-safe tick cap → distinct fatal marker.
- **B2 corroboration trusts report counters.** `restarts/heartbeats/spoofer_bits/
  crash_attempts` participate in the security decision; a buggy supervisor can
  forge them. Make report fields diagnostic ONLY; corroborate from kernel state.
- **B3 child identity not established.** Any FAULT record counts, any live
  dispatch counts, any magic-EXIT record is "the spoofer". Need exact per-image
  population — add an image id to the PROCESS record (or a kernel child ledger)
  and require the exact {supervisor, restart-svc, survivor, spoofer, crashloop}
  set by image.
- **B4 rollback test is vacuous (Claude H1).** At ARM all 8 VM slots are full, so
  the failed-launch spawn returns BUSY→NOSLOTS regardless of the failpoint, and
  the kernel never checks `je_spawn_failpoint_fired()`. Free a VM slot (reduce
  crash-loop retention / reap during run), use a later failpoint step, and require
  `je_spawn_failpoint_fired() == 1`.

Important (acted on):
- Supervisor death reason unchecked on success → require `reason==EXIT` and an
  agreed nonzero exit code (this also lands the D12 "second success signal"
  the owner asked to add down the line — bring it forward).
- Survivor death ignored (`PEER_CLOSED` treated like EMPTY) → fail SURVIVOR.
- Spoofer guess probe accepts BADINDEX||STALE → require exactly STALE; pin the
  `0x00010001`-valid-in-supervisor-table invariant instead of relying on slot
  allocation order.
- Seqlock bypassed for counter mutations → publish complete snapshots under the
  odd/even bracket.
- Add the second zero-reap assertion and a stricter baseline (compare
  `used_count`, `retired`; restrict generation advance to the run's own slots).
- integration.py mode 12 matches loose text → require the FAULT reason value,
  that phase READY was observed, and that "scenario completed" is ABSENT.

Clean (confirmed by both): the monitor never calls idle_step and runs scan→read→
corroborate→kill→reap in one IF=0 section; the report read happens while the VM is
present; the identity baseline is a real ownership predicate (page_state, process
and VM identities), not a count compare; mode 12 selects the fault via a memory
word (no GPR), faults before DONE, and the loss decision is report-independent;
deadlines are wrap-safe; QUEUE_FULL is backpressure; the report seqlock structure
in publish() is correct. Pool maxima fit (but need assertions).

AI-assisted review; not human qualification.
