**VERDICT: FIX-FIRST.** I found 3 blockers, 1 escalation, and several should-fix items. The `make mutants` aggregate is green (I reproduced it), but it hides at least two regressions. The worktree is untouched at fa7d1d6 and clean; every experiment ran in `/tmp/mp3c-adv` scratch copies. All findings are also posted to Slack (6 messages).

## BLOCKER

**B1. The slot-0 idle context has no stack** (`sched_arm.c:77`, `:780-794`).
- **Mechanism:** the restore seam sets SP to the idle frame address + 304. Every IRQ taken while idle then runs the C IRQ path (`je_arm_irq` → `je_arm_sched_irq` → reap / deadline sweep / switch) with its stack *below* the frame. That memory is live .bss: `je_arm_frame_generation[]`, `je_arm_sched_current_identity` and `je_arm_ipc[12..15]`.
- **Injection:** a 0xA5 guard zone below the frame. Observed: 416 bytes overwritten. Expected: 0.
- **Injection:** compare frame cookies at idle exit. Observed: `g6=0x400854c8` (a saved LR), and BLOCKED slot 2's cookie is still 2 while its generation entry was overwritten to 3, then 1. The address-taken `waiter` local (`:1228`) lives at `[sp+136]`, which is `&gen[2]`.
- **A/B test:** I disabled the test-only kill-at-wake hook so slot 2 resumes normally. HEAD gives `FATAL sched frame invalid`. The same change plus an 8 KiB idle stack gives no frame-invalid. The idle-stack fix alone gives a positive PASS with all 7 markers.
- The positive proof only passes because the corrupted slot happens to be the G3 kill victim and is never resumed.
- A second natural reproduction: ARM-MP3C-PEER-CLOSED-WAKE is "killed" at HEAD by `frame invalid`. With the idle fix applied, its kill becomes the user-side check.

**B2. Previously-passing ARM profiles regressed, including one in remote CI.**
- `ARM_TIMER_TEST=1` and `ARM_SCHED_TEST=1` both FAIL with `FATAL sched frame not consumed`. Both PASS at 52adf4f. `ci.yml` runs `ARM_TIMER_TEST`.
- All 13 `ARM_SCHED_MUTATION` profiles die before printing `MUTATION FIRED`. At base, all 13 "FAIL as expected" with their named detectors.
- **Cause:** the idle-frame install (`:780-794`) is not gated to EL0 builds.
- **A/B test:** wrapping it in `#ifdef JE_ARM_EL0_TEST` restores timer-test PASS, sched-test PASS, all 13 mutation detectors as at base, and EL0 still PASSes.
- The HANDOFF evidence never ran these profiles. The other 12 non-EL0 profiles behave the same at base and HEAD.

**B3. Coverage that passed at base was lost and hidden as "legacy".**
- **ARM-MP3B-LIVE-ROOT-CORE:** the un-mutated profile PASSes at base (`FATAL sched copy live root`) and double-faults at HEAD. B is now created late, so `el0_test.c:1272` reads `je_el0_good_slot[1]=4294967295` and then `je_arm_user_root[UINT32_MAX]` (out of bounds). The copy_core live-root security guard has had no coverage since. The other 13 copy-fault profiles are unchanged.
- **ARM-USER-LONE-SURVIVOR-READY-BEFORE-PICK:** KILLED at base; at HEAD the mutation PASSes smoke, so it SURVIVES. Its disabled_reason doesn't say so.
- **MP3a-2 gates relaxed:** base required exactly one `EL0 TIMER_PREEMPT from=1 to=2` and one `LONE_IRQ_RESELECT`. HEAD prints neither.
  - `smoke_arm.py:1330` and `:1352` now accept `TIMER_WAKE` or the SVC reselect marker as substitutes.
  - `el0_test.c:1579` lets TIMED_OUT set `je_el0_irq_marker_printed`.
  - None of this is recorded in the ADR or HANDOFF.

## ESCALATE (owner)

**E1. ADR 0017 D1 is not implemented as approved.** `syscall_glue.inc:15-17` sends x86 to `syscall_glue_x86.inc`, its own unchanged copy that uses no `JE_G_*` accessors. ARM compiles a separate, re-derived body. That is the rejected Option 3 (fork now, converge later) under the Option 1 name. x86 byte-identity holds trivially because x86 never compiles the shared body. Leftovers:
- dead `#else` branches in the ARM half of the glue;
- about 380 lines under `#if 0` in `syscall_arm_ipc.c` (140-249, 288-400, 408-570).

## SHOULD-FIX

1. **Vacuous markers:**
   - `ALL_BLOCKED_IDLE woke=1` prints at the boot handoff (`sched_arm.c:1120`, serial line 40), before any process has run.
   - `WOKEN_RESTART` fires on B's 4-byte/2-cap recv, which never parks (the park/resume trace shows no PARK). With woken delivery broken, the marker still prints.
   - `TIMER_WAKE` is printed in the same branch as `TIMED_OUT`, so it is not independent evidence.
2. **Negative stages whose FATAL never depends on the guard being tested:**
   - drop-the-rewind with the rewind restored still gives `IPC rewind guard`.
   - attenuation-bypass with the check restored still gives `attenuation-bypass`.
   - waiters-busy and woken-then-deadline reach raw PASS once their normal-path sentinel is removed; the named condition never occurs.
   - commit-mismatch and copy-fault-resume are unconditional sentinels.
   - The real rewind detector is the resume assertion (rewind dropped, sentinel removed → `IPC resume assertion`).
3. **Untested branches in the ARM resumed-recv fork.** These mutants survive the positive smoke: resumed dead-cap gate, mint rollback, copy-fail release, short-buffer release and PEER_CLOSED release. So do: the one-thread-per-process check, the continuation identity check, and swapping the reap/sweep order. Every resumed recv in the fixture carries 0 caps and has no copy fault.

## NIT

- **x86 endpoint_kern.o:** +32 bytes in `je_endpoint_kern_waiter_release_by_id` (the new counter), so the linked x86 ELF `.text` differs by 32 bytes in every mode. syscall.o itself is identical to 52adf4f for TEST_MODE 0-15.
- **Stale mutant text:** the disabled TWO-PHASE-COMMIT mutant is KILLED at HEAD, so its GAP note is out of date. WAITER-RELEASE-DEATH's killer text still says "GAP".
- **The `post_svc_elr >= BASE+4` guard is unreachable.** A blocking SVC at ARENA_BASE parks with `elr=BASE+4` and restarts correctly (injected). The ADR's "cannot rewind below the arena" rationale is moot.
- **`can_wake` ignores SLEEPING slots.** This deviates from D3; it is latent because ARM EL0 has no sleep syscall.

## Confirmed OK (by injection)

- **Resume assertion:** `-4` fatals on every wake.
- **Continuation clearing:** without the wait-finish, or without clearing the continuation at death, the next SVC trips the resume assertion. Skipping the continuation is caught by the user-side check.
- **Scheduler state guards:** pick selecting BLOCKED → `frame invalid`. `je_rr_wake` on SLEEPING is caught by `test_sched_rr`.
- **Idle mechanics:** the idle WFI is reached via ERET with the guard clear (the IRQ is taken from the idle frame). FAULT 11 is a real no-wake control: removing its fatal makes it hang.
- **Gate split (D9):** reverting it gives `FATAL el0-test death-release`, and that marker is decided from kernel state.
- **Cap transfer:** a real copy fault on the 2-cap recv rolls back correctly (minted0 closed, minted1 null, message still queued).
- **Checks:** `make check` PASS. Full `make mutants`: 72 killed / 1 equivalent / 0 survived / 0 blocked / 5 skipped. The DCC mutant was killed 1/1 in the aggregate and 3/3 in isolation, so I could not reproduce that flake.

**Effort:** 59 targeted injections (about 160 ARM boots), 28 copy-fault and 68 profile runs comparing base and HEAD, 32 x86 builds, one full mutant run, and `make check`. QEMU TCG cannot show cache/TLB effects (reviewed, not observed). This is AI-assisted review, not human qualification.
