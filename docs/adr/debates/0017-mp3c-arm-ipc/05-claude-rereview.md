**VERDICT: FIX-FIRST.** B1, B2, B3 and E1 are genuinely closed; I confirmed each by injection. The default EL0 positive, though, has a new race (from 589447d) that makes the pre-PR gate unreliable. Five negative stages are still vacuous or red, and the docs weren't refreshed for E1. The worktree is untouched and clean; all experiments ran in `/tmp/mp3c-rr2`. Findings are posted to Slack (5 messages).

## BLOCKER

**F1. The default EL0 positive is flaky: `FATAL el0-test death-release` (el0_test.c:1754-1768).**
- **Observed rate:** 2/59 boots at HEAD, 2/34 at 942afef, 0/31 at fa7d1d6.
- **Cause:** the reuse probe requires `je_el0_death_rec->allocated == false`. That only becomes true once process A runs `HANDLE_CLOSE(death_a)`, which comes after its victim `TRY_SEND` (el0_test.c:654-658). If a timer IRQ lands between the send and the close, the reap and the reuse process's first `YIELD` win the race.
- **Diagnostic bitmask on the failing state:** `0x2030e`. The record is still allocated, the victim's end is CLOSED, and A's end is PEER_CLOSED. The waiter-release counter, no-stale-id and no-pending checks are all fine, so the kernel's D9 release is correct; the oracle is over-strict.
- **Deterministic repro:** a CPU-only spin of about 8M iterations between A's send and close gives 3/3 FATAL. A 2M-iteration spin gives 3/3 PASS.
- **Gate impact:**
  - My full `make mutants`: 73 killed / 1 equivalent / 0 SURVIVED / **2 BLOCKED** / 2 skipped. Both BLOCKED (CNTKCTL-ISS-GUARD, LONE-SURVIVOR) are this pristine-baseline FATAL.
  - The lead's 3 BLOCKED have the identical baseline tail, so they are this race, not load. Each KILLs 2/2 in isolation.
  - 23 mutants share this baseline, so a clean sweep is roughly a coin flip.
- **Fix (validated):** accept the record still being allocated if the victim's end is CLOSED, and keep the counter, no-stale and no-pending checks. With that change the forced race passes 2/2, and reverting the gate split (`je_lc_release_waiter`) still FATALs `death-release`. Also fix the `rec_freed=1` text in the marker. Effort: about 10 lines.

## SHOULD-FIX

**F2. waiters-busy is red and vacuous.**
- **Red:** smoke expects `FATAL IPC WAITERS_BUSY guard`, but the kernel prints `FATAL sched IPC WAITERS_BUSY guard`. That fails 2/2 at HEAD and at 942afef. The lead's 04:35 log shows the other text, so the committed code isn't what was tested.
- **Vacuous:** sched_arm.c:1396-1409 FATALs with the same text on both branches. With the endpoint.c:295 guard removed, the stage still gives the same FATAL, and the positive passes.
- The hosted `test_endpoint` does catch the guard removal (assert at :1425).

**F3. timed-out-late-send is flaky and vacuous.**
- It ends in `reap-mismatch` in 2/4 runs at HEAD and 2/3 at 942afef.
- In the "expected" outcome the peer has a *live WAITING* waiter (diagnostic `peer_cause=1`, 4/4). It is not a send to a TIMED_OUT waiter.
- With the D7 single-terminal check removed, it still prints `late-send ordering`.

**F4. Three more stages still imply coverage they don't have.**
- **woken-then-deadline** (sched_arm.c:1281): it FATALs unconditionally before any sweep runs. With the arbitrate CAS (endpoint.c:316) removed, the FATAL is the same and the positive passes. The hosted test catches it at :1435.
- **attenuation-bypass:** with the check *kept*, it still FATALs, because el0_test.c:1685 fires on any successful send by A. The property itself is covered by the ARM-MP3C-ATTENUATION mutant.
- **copy-fault-resume:** removing the release or close at glue:79-83 gives the same FATAL, and the positive passes. Removing the handle-array copy-fail rollback (glue:88-94) also passes.
- HANDOFF's claim that it "covers the resumed copy-fault release path" is false. Both copy-fail branches are missing from the honest-limits list. The six branches that list does name are accurate: each mutation survives the positive.

**F5. The MP3a-2 TIMER_PREEMPT gate isn't fully reinstated.**
- Smoke no longer accepts substitutes.
- But el0_test.c:1653 widened the kernel predicate:
  - **base:** ELR must be inside the 2-instruction CPU-only spin, and the peer must have run;
  - **HEAD:** any ELR in the arena, printed at switch time.
- The base predicate passes 3/3 at HEAD, so restoring it is free. The change is undocumented.

**F6. Docs are stale after E1.**
- There is no HANDOFF entry for b2a5d57.
- IMPLEMENTATION.md:7 says "no D1/E1 glue-shape refactor was made", and :24 says "byte-identity rerun pending".
- The recorded mutant tally (75/0/0) predates the E1 sweeps (72/3 BLOCKED for the lead, 73/2 for me).

## NIT

- **B1 guard placement:** the idle gating flags, canary pointer and run_base sit outside the guarded struct. With B1 reverted they get clobbered, so `idle stack guard` never fires; the proof times out instead (double fault with the check disabled). It can't false-PASS.
- **Dead x86 copy:** 340 lines of the old x86 case bodies remain under `#if 0` (syscall.c:842-1015, 1056-1221).
- **Shared body purity:** it still has `#if JE_TEST_MODE == 13/15` hooks, and the handle-case policy lives in large per-arch macros. Convergence is partial there.
- **Stale killer texts:**
  - LONE-SURVIVOR names `no live EL0 successor`, which no longer exists; the actual kill is `nothing can wake`.
  - REWIND is killed by the adjacent `IPC rewind guard` postcondition, not by the restart proof.
- **Empty disabled reasons:** both skipped mutants have an empty `disabled_reason`.
- **CPU-speed floor:** under `-icount shift≥7`, HEAD and 942afef fail and fa7d1d6 hangs; base passes at shifts 6-8. Fine on real-time TCG and physical hardware.

## Confirmed closed (by injection)

- **B1:**
  - The idle stack is at `0x40124000-0x40128000` with an unmapped guard page, outside .bss (which ends at `0x40117e40`).
  - Over 128 idle IRQs: minimum SP is top−0x200, deepest write top−0x2e0, bad SP 0, guard bytes hit 0.
  - No-kill profile 21/21 PASS with no `frame invalid`. Reverting B1 breaks the proof, and poking the guarded struct FATALs `idle stack guard`.
- **B2:** timer and sched tests PASS. All 13 sched mutations hit the same detectors as base. All CI profiles PASS at both base and HEAD.
- **B3:**
  - live-root-core reaches `FATAL sched copy live root`; its mutant KILLs at `copy-live-root-recovered`.
  - All 14 copy-fault profiles behave identically at base and HEAD.
  - The lone-survivor mutant KILLs on replay; it was BLOCKED only by F1 in my sweep.
  - Smoke strict gates hold.
- **E1:**
  - syscall.c includes `syscall_glue.inc` three times, and the x86 copy is deleted.
  - x86 `syscall.o` .text is md5-identical to 52adf4f for all 16 modes (size 11055/11089/11266/11219, matching the lead).
  - A semantic edit to the shared TRY_SEND changes x86 .text; a comment-only edit doesn't, so x86 really compiles the shared body.
  - ARM's IPC unit token-diff against 942afef is syntactic only, plus one FATAL message text; all other ARM objects are identical.
  - ARM has no PROCESS/DEVICE authority, and DUP subset checks are enforced in the core.
- **Markers:**
  - WOKEN_RESTART disappears when woken delivery is broken.
  - TIMER_WAKE is independent of TIMED_OUT.
  - ALL_BLOCKED_IDLE prints late and depends on the all-blocked state.
- **Negatives that are now load-bearing:** drop-the-rewind, commit-mismatch, short-buffer, idle-no-panic. Those four plus wrong-resume-assertion, re-resolve-on-resume, attenuation-bypass, copy-fault-resume and woken-then-deadline each FATAL'd at their named detector 4/4.
- **Other gates:** `make check` PASS; `git diff --check` clean.

**Effort:**
- About 200 build-and-boot experiments, including around 60 targeted injections.
- 145 repeat boots and 18 icount boots.
- A 102-profile base/942afef/HEAD matrix.
- 48 x86 mode builds plus the sensitivity and control builds.
- A full mutant sweep, plus 13 kill-point replays and 6 isolated mutant runs.

**Fix effort:** about half a day including re-verification.

My mutant sweep ran with other scratch boots going at the same time, so it was not solo. This is AI-assisted review on QEMU TCG (cache/TLB not observed), not human qualification.
