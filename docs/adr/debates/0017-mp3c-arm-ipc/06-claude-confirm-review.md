**VERDICT: FIX-FIRST.** F2, F5 and F6 are closed, and the F1 race no longer fails. But relaxing the F1 oracle opened a new hole in the proof that a dead waiter's endpoint record gets freed (D9), and nothing else in the gate catches it. B1/B2/B3/E1 and the markers did not regress. I didn't touch the worktree; all experiments ran in `/tmp/mp3c-fc`. Four findings went to Slack, plus a final summary.

## BLOCKER

**G1. The relaxed death-release check accepts a real leak (el0_test.c:1693-1712).**
- **Injection:** the death path clears the dying waiter but keeps its reference on the endpoint record (`je_endpoint_wait_release_by_id`). This is the exact ADR 0017 D4 hazard: the record is never freed.
- **At HEAD:** smoke PASSes 4/4 in the default profile and 3/3 in the forced-race profile, with `rec_freed=0`. A diagnostic print at the check shows both ends CLOSED, the record still allocated, and one leaked reference.
- **At b2a5d57 (strict check):** `FATAL death-release` 4/4.
- **Nothing else catches it:** `make check` passes, and added as a `mutants.py` mutant it SURVIVES.
- **Why it slips through:** in the still-allocated branch, the waiter check only sees WAITING waiters, but this fixture kills the victim while WOKEN. The only remaining guard is a global release counter. The coarse probes are still caught through that counter: stubbing `je_lc_release_waiter` and skipping WOKEN waiters both FATAL.
- **Fix (validated in scratch, ~12 lines):** accept a still-allocated record only if all of these hold:
  - A's end is still PEER_CLOSED;
  - the generation is unchanged;
  - the victim has no waiter of any kind;
  - both ends have zero in-flight references.

  Results: default 6/6 PASS, forced race 6/6 PASS, and all three leak injections FATAL. Also add the leak as a mutant.

## SHOULD-FIX

- **G2. Coverage lost with the timed-out-late-send stage.** I let a late send override a TIMED_OUT waiter (one direction of the endpoint.c:315 check). That passes `make check` and the ARM positive 3/3. The hosted test at test_endpoint.c:1435 only checks the other direction. No mutant pins this check, and the deadline mutant HANDOFF cites is disabled. Fix: add the reverse assertion plus a hosted mutant (~15 min).
- **G3. Honest limits shrank.** The current HANDOFF entry and IMPLEMENTATION row now list only the two copy-fault branches. Six still-uncovered branches were dropped. At HEAD, removing the mint rollback still passes the positive, and so does removing the PEER_CLOSED release. The two copy-fault branches themselves are described accurately.
- **G4. Review transcripts not attached (ADR 0000 part 4).** `docs/adr/debates/0017-mp3c-arm-ipc/` only has 00-02. This review's transcript is at `/tmp/mp3c-final-confirm-claude.md` for the lead to attach.

## NIT

- **The new timer rearm is load-bearing.** Without it, TIMER_PREEMPT never fires 5/5 and the proof loops until the 70 s timeout instead of failing with a named FATAL.
- **Dead `JE_G_FAULT_WAITERS_BUSY` hook.** It prints the old non-canonical text, which smoke would now reject.
- **Unused `EL0_IPC_LATE_SEND` constant** left in smoke_arm.py.
- **Two killer texts are imprecise.**
  - The attenuation mutant actually dies at `register-round-trip`.
  - The PEER_CLOSED-wake mutant actually dies at `reap-mismatch`.

  Both are real kills.

## What I injected to confirm closure

| Item | Evidence |
|---|---|
| F1 race | Forced race on unmodified HEAD 10/10 PASS (`rec_freed=0`). Default positive 30/30 under 4-way parallel load; 2 boots hit the natural race and passed. The WAITER-RELEASE-DEATH mutant FATALs 3/3 (default) and 3/3 (race). Two solo `make mutants` runs: both 75 killed / 1 equivalent / 0 SURVIVED / 0 BLOCKED / 2 skipped. |
| F2 | Smoke text matches the kernel text; stage hit its named FATAL 2/2. Removing the guard gives `guard missing`, a distinct smoke FAIL. |
| F3/F4 | All 8 remaining stages hit their named FATAL 2/2. Removing each guard changes the result (different FATAL, a PASS, or a hang), so none is vacuous. Removing the whole check is caught by the hosted test at :1435; the attenuation mutant kills. |
| F5 | Shortening the spin to 1 iteration: no TIMER_PREEMPT at HEAD, but PASS at b2a5d57 (so the old widening was real). The check is identical to base. |
| F6 | No `#if 0` left (syscall.c 1238 → 898 lines). Docs and tally are correct, and the LONE-SURVIVOR and REWIND kill points match their new texts. |
| B1 | Idle stack/BSS guard marker clean on all 30 positives; no-kill profile 8/8 with no `frame invalid`. |
| B2 | Timer and sched PASS; all 13 sched mutations hit the same detector as base. |
| B3 | live-root-core FATALs `sched copy live root`; its mutant and the lone-survivor mutant KILL. |
| E1 | x86 `syscall.o` `.text` identical to 52adf4f in all 16 modes. A real edit to the shared code changes it; a comment-only edit doesn't. |
| Markers | All 7 still load-bearing by mutant or injection. `make check` PASS; `git diff --check` clean. |

**Effort:** about 175 QEMU boots across roughly 100 builds, 34 x86 builds, 3 `make check` runs, 2 full mutant sweeps and 1 single-mutant run. Fixing everything should take about an hour including a re-sweep.

This is AI-assisted review on QEMU TCG (cache/TLB not observed), not human qualification.
