# M4 PR-D reviews (2026-10-31, AI-assisted; not human qualification)

Worker mode: Codex wrote the implementation and fixture, and Claude reviewed them. Claude wrote the tests from headers only, and Codex reviewed them with mutants. Lead: Kiro. Reviewer outputs are verbatim.

Lead check before review: `mark_dying`'s body is byte-identical to main `62c0115` (owner-gated tested code untouched); only the PR-D stub line was removed from sched.c.

## Round 1 — Claude reviewing the Codex implementation and fixture

I found 1 P1, 3 P2 and 4 P3 findings. The kernel side of PROC_KILL looks sound; every finding is in the M4d-s1 test fixture or is a coverage gap. This was a read-only, AI-assisted review: nothing was built or run, and it doesn't count as independent human review.

**P1-1 — The READY-victim check in the QEMU test can never fail.** `kernel/core/main.c:1929-1938` reads `vm_s3_heartbeat()`, which reads `ARENA_BASE+PAGE_SIZE` (0x40001000). The READY victim increments 0x40001e00 instead (`user_demo.S:843`). The value at 0x40001000 never changes, so "READY victim never resumes" is not tested at all. No other check covers it: there's no `dispatches_of` on the READY victim, and nothing asserts it ran before the kill.
- Fix: read `vm_s3_scratch(ready_victim, 0x1e00)`, or change the victim to `incq 0x40001000`.
- Assert the heartbeat is above 0 before releasing gate 1, so we know it was dispatched and was really READY.
- Snapshot `je_sched_dispatches_of(ready_victim)` after the kill and assert it stays the same for the 0x40-tick window.

**P2-1 — The BLOCKED_IPC check races the reaper and can read a freed address space.** The phase-3 loop (`main.c:1941-1964`) calls `je_kernel_idle_step()` on every pass while the killer is still between PROC_KILL and writing phase 3 (it still has to call proc_status and close).
- If the reaper runs first, `je_vm_destroy`/`unwind` zeroes `procs[idx]`. The next `vm_s3_scratch(blocked_victim, …)` then loads CR3 = 0, which is undefined behaviour or a triple fault.
- In the same case `je_proc_status` reports DEAD instead of DYING, so the test fails spuriously.
- It passes today only because the killer usually finishes inside one tick.
- Fix: poll with `pause` until phase 3 and run all the checks, then reap, which is how the WOKEN phase already does it.

**P2-2 — The BLOCKED_IPC check can't tell whether the victim's continuation ran.** Line 1958 only rejects TIMED_OUT and CANCELLED. If the continuation ran it would write OK (0, the same as the scratch default) or PEER_CLOSED (12, likely after the killer's close). Neither is caught, and nothing checks `blocked_progress != 99` or `dispatches_of(blocked_victim)` after the kill. The status checks in the pre-kill window (`main.c:1915-1918`) have the same weakness.
- Fix: in the phase-3 critical section, before any reaping, assert `progress == 1` and that dispatches equal the pre-kill snapshot. The only real evidence today is the endpoint-reclaimed count (`allocated == 1`), and that covers the leaked-inflight-ref case.

**P2-3 — The WOKEN-reserved state is inferred, never observed.** The checks at `main.c:2027-2039` (queue count 1, no waiter, `!head_reserved`, dispatches unchanged) would give the same result if `try_send` never woke the victim and it was killed while still BLOCKED. `!head_reserved` is trivially true if the head was never reserved. Separately, the comment at `user_demo.S:836-839` says the killer "never yields" between send and kill, but a timer can still preempt ring 3 between the two `int 0x80`s. If that happens the test fails (it never falsely passes), so it's a flake source.
- Fix: add a dev-only count of reservations released by `mark_dying` (`waiter_release_end` while the waiter was WOKEN), snapshot it, and assert it rises by exactly 1 across the kill.
- Alternatively, record `cause == WOKEN` and the READY state from the wake path.

**P3-1 — The "first reason wins" label is overstated.** The re-kill at `user_demo.S:~1040` targets a record already KILLED, so it only proves the call is idempotent. Fix: kill a record that ended by EXIT and assert the reason stays 1.

**P3-2 — Idempotence is only tested on a DEAD record.** No test re-kills a DYING (not yet reaped) record. Fix: re-kill the blocked victim right after its kill, before phase 3.

**P3-3 — Untested paths.** Killing the supervisor is never exercised; the fixture has no supervisor record. A kill where the run link is valid but the thread is in an unexpected state isn't hosted-tested either; that path is a fail-stop by design. A unit test for `je_proc_is_supervisor` plus `je_sched_kill` ordering would cover the first.

**P3-4 — Minor kernel nits.** In `sched.c:730`, the `death_rip` saved for a BLOCKED or WOKEN victim is the `int 0x80` address, because `block_current` wound `f->rip` back by `JE_INT80_LEN`. It's cosmetic since the ABI doesn't expose it; worth a comment. Also, `vm_index` from `run_link` is dropped; checking it against `threads[slot].vm_index` would add defence in depth.

**Confirmed sound:**
- **Order:** `je_sched_kill` (`sched.c:676-733`) matches the sched.h order exactly: invalid → self → DYING/DEAD returns OK, decided from the record → not-LIVE → supervisor → unlinked → slot invariants → `mark_dying`.
- **IF and frame:** IF is saved, aborts if it was set, and is restored on every return path. RIP and CS are snapshotted before `mark_dying` sets the frame to 0, so no frame is read after zeroing.
- **Unreachable targets:** kernel threads, the caller itself, an unlinked record, a reused identity (the generation is checked in `record_for_const`) and a slot belonging to a different identity are all rejected or fail-stop. SLEEPING, ZOMBIE and RUNNING victims can't occur for a linked user thread on a single CPU.
- **Release:** `mark_dying` releases the waiter's inflight ref and any WOKEN reservation through `waiter_release_end` and clears `wait_pending`. The DYING state blocks `wake_ipc`, timer TIMED_OUT stamping and dispatch, so the victim can't observe CANCELLED and its continuation can't run.
- **Reaper:** reaping happens only from the bootstrap with IF=0. The record survives as DEAD while handles reference it, and there's no use-after-free between the run link and the slot.
- **Glue:** type is checked before rights (WRONGTYPE, then DENIED). Handle errors map through `handle_status_to_abi`, RDX is 0, and the decode reserves RSI–R9 and RDI[63:32], matching abi.h op 16.
- **Negative fixture cases:** the self, no-KILL-right, wrong-type (SELF handle), stale-handle and unlinked-stays-LIVE cases are real, non-vacuous assertions.
- **Bounded waits:** every bootstrap wait is tick-bounded, and IF=1 during the `pause` loops, so ticks keep advancing.

## Round 1 — Codex reviewing the Claude tests (mutants) and the fixture's power

Reviewed commit `62e510456bb7474fda46b1c12e38968ea58744b0`; no repo edits, no `make`, hosted builds only under `/tmp`.

Hosted mutation result: **0 survivors**.

- `proc.c`: state omission, link omission, early writes, swapped outputs, slot-keyed/invalid-generation/live-only supervisor checks were all killed.
- `syscall.c`: wide-handle omission, each reserved-register omission (`RSI/RDX/R10/R8/R9`), wrong action/result, and low-32-bit op aliasing were all killed.
- Unmutated hosted `test_proc` and `test_syscall` passed.

1. **P1 — QEMU survivor: supervisor guard is untested.**
   M4d creates every record with `supervisor=false`; removing `je_proc_is_supervisor()` can pass the fixture.

   Fix: mint a PROCESS handle to a supervisor record and assert `INVALID`, unchanged `LIVE` state.

2. **P2 — QEMU survivor: killer rescheduling is not directly asserted.**
   The phase handshakes prove eventual progress, but do not prove `PROC_KILL` returns without switching/requeueing the caller.

   Fix: capture killer dispatch/current-thread identity around each kill and require no scheduler transition.

3. **P3 — partial waiter-release survivor.**
   Full release removal is caught by the WOKEN reserved-head check. However, a mutant that fails to release only a `WAITING`/BLOCKED waiter can pass because reaping later calls `je_sched_drop_user()` and masks the defect.

   Fix: before reaping the BLOCKED victim, assert its waiter is already absent, cause `NONE`, and its inflight reference released.

Contract judgments:

- **A1:** sound as written. `valid` includes DYING/DEAD, so supervisor remains true for those records; clarify wording for future callers.
- **A2:** real documentation inconsistency. `sched.h` should be declared authoritative and the map aligned, especially for supervisor-vs-terminal ordering.
- **A3:** sound and well tested; every NULL-output case preserves all non-NULL outputs.

The forged `DEAD + run_linked` test is justified white-box coverage: it independently kills the missing-state-check mutant that is unreachable through the public lifecycle API.

Confirmed sound: self-kill, record-based idempotence, `wait_cancel` substitution, full waiter-release omission, unlinked-LIVE rejection, and wrong death reason are caught by the fixture. **AI review, not human qualification.**
## Lead disposition

- **Kernel path judged sound by both reviewers.** Hosted mutation testing found 0 survivors.
- **Claude P1-1 (the READY check could never fail: wrong heartbeat address): accepted.** This is exactly the class of false evidence AGENTS rule 9 forbids. A fixture that cannot fail is not a test.
- **Codex P1 (supervisor guard untested): accepted.**
- **Claude P2-1, P2-2, P2-3 and Codex P2/P3: accepted.**
  - P2-1: a race with the reaper that could read a freed address space.
  - P2-2: the fixture could not detect whether the continuation ran.
  - P2-3: the WOKEN state was inferred rather than observed. The lead added the kernel-internal counter `je_endpoint_kern_woken_reservations_released` to the header so the fixture can observe it.
  - Codex P2: the "killer not rescheduled" claim is now asserted.
  - Codex P3: a partial waiter-release mutant could have survived.
- **Ambiguities A1–A3:**
  - A1: `is_supervisor` stays true for DYING/DEAD records. Accepted as written.
  - A2: `sched.h` is authoritative. The /tmp code map was a scratch document, not repository text.
  - A3: accepted as written.

## Verification round — Claude (fixture after round 3)

Two findings are not really fixed. The supervisor-guard check can never fail, which also leaves Codex's P1 open. The new EXIT first-reason step races. Everything else checks out. Read-only, AI-assisted review at 84ec10d; nothing was built or run.

## Round-1 findings

| # | Verdict | Evidence |
|---|---|---|
| Mine P1-1 (READY check can't fail) | **FIXED** | Reads `0x1e00` and requires it to be above 0 before the gate (`main.c:2032-2043`). Holds heartbeat and `dispatches_of` steady for 0x40 ticks (`main.c:2070-2081`). |
| Mine P2-1 (reaper race) | **FIXED** | No reaping until phase 3: `m4d_wait_interrupt` at `main.c:2120`; the killer waits for gate 3 after the reap (`user_demo.S:965-969`). |
| Mine P2-2 (continuation unchecked) | **FIXED** | `0xdead` sentinel, `progress==1` and the dispatch snapshot (`main.c:2108-2113`). Waiter absent, cause NONE and inflight ref back to baseline (`main.c:2096-2101`). The baseline is a real check: register adds a ref and release removes it (`endpoint.c:298/361`). |
| Mine P2-3 (WOKEN inferred) | **FIXED** | Counter must rise by exactly 1 (`main.c:2201`). It only counts when the released waiter was `cause==WOKEN && head_reserved` (`endpoint_kern.c:312-327`). A timer tick between send and kill is still a flake source, but it can only make the test fail, never pass falsely. |
| Mine P3-1 (first reason) | **FIXED-BUT-WRONG** | See new P2-B. |
| Mine P3-2 (DYING re-kill) | **FIXED**, conditional on P2-B | The EXIT victim is not reaped before the killer's kill, so it is DYING then. |
| Mine P3-3 / Codex P1 (supervisor guard) | **NOT FIXED** (the test can never fail) | See new P1-A. |
| Mine P3-4 (kernel nits) | **FIXED** | `vm_index` is now cross-checked (`sched.c:720,724`) and the `death_rip` comment is added (`sched.c:729-731`). |
| Codex P2 (synchronous return) | **PARTIAL** | Checks `killer_dispatches == before+1`, but only for the READY kill (`main.c:2055`). The BLOCKED and WOKEN kills have no such check, yet the smoke marker says "synchronous-return" without qualification. |
| Codex P3 (BLOCKED waiter release) | **FIXED** | `main.c:2085-2101`, read before any reap. |

## New P1/P2

**P1-A: the supervisor guard check can never fail (`main.c:1900-1906`, `user_demo.S:~1088`).**
- The supervisor record is LIVE and was never linked to a thread.
- If `je_proc_is_supervisor` is removed (`sched.c:708-711`), `je_proc_run_link` then fails and returns `JE_ERR_INVALID` (`sched.c:715-718`).
- That is the same 2 the test expects, and the state stays LIVE. The "supervisor guard" claim in the marker is therefore not proven.
- Fix: give the supervisor record a real linked thread, so removing the guard would actually kill it. Or add a unit-test check, as Codex proposed.

**P2-B: the EXIT kill races the victim's exit (`main.c:2241-2256`, `user_demo.S:~1060`).**
- The asm comment says the bootstrap waits until the EXIT victim is DYING. It does, but the killer never waits for that.
- Right after gate 5, the killer runs the negative cases and then kills `0x1c58`.
- If the killer is dispatched before the new EXIT victim (it was created last, at the tail of the run queue), the victim is still READY. The kill records KILLED, the killer fails, and the bootstrap times out.
- This can't produce a false pass, but it can fail, possibly every time depending on queue order.
- Fix: add a gate 6 that the bootstrap writes after it sees DYING/EXIT.

**P2-C: the synchronous-return check can fail spuriously (`main.c:2035-2045`).**
- `killer_dispatches_before` is captured, then IRQs are restored, then gate 1 is written.
- A tick in that gap, or one between the kill and marker 2, gives a count of +2 and a spurious failure. It can only fail, never pass falsely.

**WOKEN counter: something other than a kill can increment it.** It has two callers:
- `mark_dying` (`sched.c:246`). That path also runs for exit and faults, not only kills.
- `je_sched_drop_user` (`sched.c:547`). It runs on teardown of a non-dying thread, so tearing down a live thread that is WOKEN with a reservation also increments the counter.

Inside the M4d snapshot window (`main.c:2163-2201`) nothing reaps or drops, and only the killer and the WOKEN victim can die. So the +1 check is sound for this fixture. The counter is not death-only in general, though: its comment and name should say "any release of a WOKEN+reserved waiter", or the drop path should be excluded. It also runs in every build and scans the pool on every thread death with IRQs off. That is bounded, but it is not dev-only.

**Other categories you asked about:**
- **No reads of a reaped victim:** every `vm_s3_scratch` and `htable` read happens before that victim's reap loop.
- **No unbounded waits:** every wait is bounded by ticks, and `sti;hlt` is safe because of the one-instruction interrupt shadow.
- **One unasserted value:** the "exit(42)" code is never checked.

## Lead mutant runs (through `make smoke`, so the ISO is fresh)

| Mutant | Result |
|---|---|
| `je_sched_kill` with no `mark_dying` | **killed** (`JeiOS fatal: M4d READY victim resumed after kill`) |
| Supervisor guard removed | **survived**: the round-2 supervisor case was vacuous, because the record was unlinked and the next check returns the same INVALID. Claude reached the same conclusion independently (P1-A). |

## Lead disposition

- **P1-A and the surviving mutant:** round 4 links the supervisor record to a real thread.
- **P2-B:** a gate is added and exit code 42 is asserted.
- **P2-C and Codex P2:** the flaky dispatch-equality check is replaced. The stronger, deterministic proof is that the killer's very next syscall (PROC_STATUS) already reads DYING/KILLED, for every kill state.
- **WOKEN counter:** its scope comment is corrected in `endpoint_kern.h` (any release path, not kill-only).
- **pause-poll stall:** its root cause is **not determined**. It is recorded as an open hazard in HANDOFF.
