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
