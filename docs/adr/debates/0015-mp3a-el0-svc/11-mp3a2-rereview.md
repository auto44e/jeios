# MP3a-2 fix-pass re-review: **FIX FIRST**

The B1 and B2 code fixes are correct, but the proof can't catch a B2 regression and the docs say it can. I ran 80 scratch builds and boots under `/tmp/rv3a2r`; the worktree is untouched. This is AI-assisted review, not human qualification.

**Your verified claims, confirmed:**
- The EL0 proof passes, including 8 back-to-back runs under parallel load.
- 13 non-EL0 ARM profiles pass, which is more than your 7: default, BSS_CHECK, VECTOR_TEST, DTB_BAD, MMU_FAULT text/data/rodata/guard, TIMER_POLL, TIMER_TEST, SCHED_TEST, USER_TEST, PHYSALLOC.
- x86 `make check` passes.
- `make mutants` gives 49 killed, 1 equivalent, 0 survived, 0 blocked. It needs a `.git` directory; in a plain `git archive` copy every mutant reports BLOCKED.

## (a) B1: closed, and the proof catches a regression
- **Single record.** The frame is now recorded once (`sched_arm.c:1097`). Re-adding the second record gives `FATAL sched frame not consumed` on the first EL0 IRQ.
- **A `b .` spinner is preempted.** I made process 1 spin `b .` forever after its first yield. Process 2 still ran 19 times, all 8 negative stages died and were reaped, process 2 exited, and the spinner was then re-selected by 1400+ IRQs with no fatal.
- **A bad user SP at IRQ time is user death.** SP_EL0 = 0x1234, or arena+0x2008 (misaligned), while spinning gives `EXPECTED_DEATH reason=2` through the IRQ path. No kernel fatal; all later stages run.
- **Fast-timer stress.** With ticks 10×, 100× and 1000× faster (611 preemptions inside the spin at 10 µs), the kernel proof still prints PASS. The smoke run fails only because it pins the timer interval.

## (b) B2: closed in code; the proof cannot catch a regression
- **Only the new line fixes it** (`sched_arm.c:887-890`). With it reverted, each of these gives `FATAL no live EL0 successor`:
  - one process only;
  - process 2 calls exit(7) first;
  - a lone survivor preempted by an IRQ.
- **With the fix, none of them fatal.** One process ran 34k times with 400+ IRQ self-re-selections; the exit(7) case did 22k lone yields.
- **Why the proof misses it.** `je_arm_el0_test_note_svc` (`sched_arm.c:1033`) runs *before* the yield is dispatched (`:1037-1042`). On the survivor's first lone yield it prints `LONE_SURVIVOR_YIELD` and PASS, then calls `proof_halt()` (`el0_test.c:529-541`). So `arm_switch_to_ready()` never runs with a lone survivor, and reverting line 887 still passes.
- **Stage to add (I built and ran it).** Gate PASS on at least 2 lone yields (so one lone yield is actually dispatched and returns) and at least 1 lone IRQ self-re-selection (from == to == survivor in `note_irq_switch`). Have the oracle require both markers, and add a mutant that reverts lines 887-890.
  - With the fix: PASS.
  - With line 887 reverted: `FATAL no live EL0 successor`.

## (c) No new code hole from the fix
- **EL1h path:** `vectors.S` is unchanged by the fix and all 13 profiles pass.
- **Mode confusion:** a bad SVC frame is now fatal. Forcing SPSR.M to EL1h, or the vector to 5, gives `FATAL sched EL0 SVC frame invariant`.
- **Reap:** I asserted three things at all 9 reaps, at normal, 10×, 100× and 1000× timer speed, and none ever fired:
  - TTBR0 is the current process's root;
  - the reaped root is not live;
  - the reaped root is not shared with a live slot.
- **Peers and frees:** peer data is unchanged after every reap, and there is no double free.
- **The new READY line** only changes RUNNING to READY. ZOMBIE and SLEEPING are untouched, slot 0 is excluded, and switching to the current process again is harmless.

## BLOCKER (proof and docs, not code)
- **B2-P.** As in (b): the proof passes with the B2 fix reverted.
  - `docs/HANDOFF.md:37-38` ("proves … the one-survivor yield path") is false.
  - `docs/IMPLEMENTATION.md:62-63` is false.
  - `HANDOFF.md:63` ("three … named load-bearing targets") is accurate for 1 of 3.

## SHOULD-FIX
- **S3-a. DROP-SP-EL0-RESTORE still dies at the test harness's `reap-mismatch`**, not at the register round-trip check that `tools/mutants.json:641` names. Slot 1 starts with SP_EL0 = 0 and is killed with `reason=2` at its first trap.
  - Separately, the proof never checks that a user's SP survives a trap. Making the restore write a constant arena_end instead of `tf->sp` (`vectors.S:302`) still passes.
  - **Fix (tested).** I combined four changes:
    - poison SP_EL0 with a valid in-arena value next to the TPIDR poison (`el0_test.c:630`);
    - the user checks SP == arena_end at entry;
    - each process then sets its own distinct SP;
    - the user compares SP after every SVC.

    Results:
    - HEAD with these changes: passes 4 of 4.
    - With the DROP-SP-EL0 mutant: `FATAL el0-test register-round-trip` 4 of 4.
    - With the constant-SP restore: `FATAL el0-test register-round-trip`.

## NIT
- **Mutant check status (S3):**

  | Mutant | Where it dies now | Load-bearing? |
  |---|---|---|
  | MODE-CONFUSION | `FATAL el0-test mode-negative`; with the probe removed, the `kernel-sp mode` SP accessor | Yes, closed |
  | ACCEPT-BAD-USER-ELR | the oracle's stage-4 check; removing that expectation lets it PASS | Yes, closed |
  | NO-SP-EL1-INSTALL | `FATAL sched observed_running` (EL0 frame-position check, `sched_arm.c:355-356`) | Yes, but the killer text at `mutants.json:760` names the wrong check |

  For NO-SP-EL1-INSTALL, removing the position check makes it die at the register round-trip check instead.
- **Unexercised mode checks.** The vector↔SPSR.M agreement check and the M[4] check (`sched_arm.c:333-337`) still aren't exercised: deleting either passes the proof. Extend the mode probe to cover vector 9 with EL1h, vector 5 with EL0t, and M[4]=1.
- **CNTKCTL readback is incomplete** (`sched_arm.c:634-635` checks only bits 1:0).
  - On HEAD, an EL0 write to `cntp_ctl_el0` correctly traps.
  - With EL0PTEN (bit 9) left set, the readback passes, EL0 disables the preemption timer, and a `b .` hangs the system with no fatal.
  - Check mask 0x303 and add a negative stage that writes `cntp_ctl_el0`.
- **S5 (debug comms channel): closed.** EL0 reads of MDCCSR_EL0 and writes of DBGDTRTX_EL0 now trap (EC 0x18). The readback catches a dropped write, but no stage exercises the channel: removing both write and readback passes.
- **S4 (TLB flush ASID): correct by review** (`vm_backend.c:397`). Reverting it to ASID 0 passes everything, and the hosted test covers only ASID 0. Reviewed, not observed on QEMU.
- **S6 (EL1h phase-0 check): restored** (`sched_arm.c:386`) and equivalent to MP3a-1. Removing it still passes SCHED_TEST; that gap predates this change.
- **S7 (SP accessors): present and passing.** The `make check` grep misses `(*tf).sp`, `tf[0].sp` and `tf-> sp`; I planted all three and it still passed.
- **S2 (register round-trip): otherwise closed.**
  - Clobbering x16–x18, or zeroing all GPRs, gives `FATAL el0-test register-round-trip`.
  - Not writing status/result gives `FATAL el0-test svc-proof-result`.
  - Clobbering x9 and x27 on return still passes, because only x0, x1 and x16–x18 are checked after the SVC.
- **Carry-overs:**
  - A hardening refusal is still reported as "frame validation" (`sched_arm.c:925`).
  - Zombies are reaped only on SVC entry; a spinning survivor never reaps its exited peer.
  - TPIDRRO zeroing is untested.
  - `-DJE_SYSCALL_HOSTED_TEST` is still used on a kernel object (`mk/aarch64.mk:228`).
  - The new `NO_DEVICE_HOOKS` acknowledgement is set on the same mk line, so it guards nothing.
  - The test-only op 0x7a returns UNSUPPORTED with result 0x5aa5, and the real abi_query decode is no longer exercised at EL0.

**Reviewed, not observed on QEMU:** cache/TLB effects and the no-tick (BUSY) EL0 path.

**FIX FIRST, all small:**
1. Add the lone-survivor PASS gate and the line-887 revert mutant.
2. Correct the HANDOFF and IMPLEMENTATION claims and the two `mutants.json` killer texts.
3. Preferably add the SP stage.

All run results are in `/tmp/rv3a2r/out*/`, and a summary is in `/tmp/rv3a2r/MP3A2-REREVIEW.md`.
