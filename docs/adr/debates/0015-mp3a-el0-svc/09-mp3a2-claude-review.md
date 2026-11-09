# MP3a-2 adversarial review: **FIX FIRST**

I found 2 blockers and 7 should-fixes. I ran 29 scratch builds and boots under `/tmp/rv3a2` (the runner is `tools/inject.py`; each injection has its own JSON spec and `.out` result). The worktree is untouched. This is AI-assisted review, not independent human qualification.

**Your verified claims, confirmed:**
- The EL0 proof passes.
- These ARM smoke profiles pass: default, `USER_TEST`, `SCHED_TEST`, `TIMER_TEST`, `MMU_FAULT=text`. I did not re-run all 9.
- x86 `make check` passes, and x86 `lifecycle.o` code is identical to HEAD~1.
- I did not re-run the full `make mutants`. I re-ran 4 of the 8 EL0 mutants by hand; all are killed, but see S3 for what actually kills them.

## Items 1, 2 and 3

1. **The frozen EL1h path is intact.**
   - I diffed the `vectors.o` disassembly (default, timer and sched builds) between HEAD~1 and HEAD. The only change is two inserted instructions: `tst x1,#0xf` and `b.eq je_arm_restore_el0`. Everything else is identical: slot-5 veneer, saver, `vec_common`, `double_fault`.
   - For an EL1h frame (mode bits 0b00101) the branch is never taken, and ERET overwrites the flags the `tst` changes.
   - Forcing the EL0 tail on *every* EL1h frame still passes the timer test (injG1) and sched test (injG2). The tail only adds writes to SP_EL0 and the two TPIDR registers (unused at EL1h) and sets SP_EL1 exactly as the EL1h path does.
   - An inverted gate is caught (injG3).
2. **The first EL0 handoff is correct.** At ERET:
   - TTBR0 is the process root with ASID 1.
   - SP_EL1 is the kstack top (the frame is validated at exactly top−304).
   - SPSR is the user's flags plus 0x340 (EL0t; D/A/F masked, I clear).
   - SP_EL0 is `tf->sp`.
   - All GPRs are zero (enforced by a fatal check).

   Each injected mismatch is caught:

   | Injection | Result |
   |---|---|
   | Kernel root used for user processes (injK4) | EL0 instruction abort at the arena |
   | Two processes' roots swapped (injK2) | Isolation oracle fails |
   | SPSR set to EL1h in the synthesized frame (injK3) | `FATAL sched frame validation` |
   | Leaked GPR in a fresh frame (injP1) | `FATAL sched frame validation` |
   | No SP_EL1 install (injH) / no SP_EL0 restore (injM3) | Caught, but only indirectly (see S1) |

3. **EL1h↔EL0 mode confusion is not possible by construction.**
   - SPSR.M is read only after the position proof.
   - The vector group must agree with SPSR.M; EL1h frames must also match the 0x345 policy with ELR in kernel text; EL0 frames are normalized.
   - The restore gate reads the same validated SPSR.
   - Caveat: the agreement check is never exercised. Deleting it entirely leaves the proof green (injL1), and so does deleting the AArch32-mode check (injL2).
   - The phase-1 ELR/SP_EL0 range check is enforced as user death at SVC entry (injE: user sets SP to 0x1234, then SVC → reason=2) and on resume (stages 3 and 4).
   - The `ACCEPT-BAD-USER-ELR` mutant is load-bearing (injM1: stage 4 dies as a fault with EC 0x20 instead of a bad return).
   - **At IRQ entry it is a kernel FATAL, not user death** (B1).

## BLOCKER

**B1. An IRQ taken from EL0 always halts the kernel.** (`sched_arm.c:983` and `:999`)
- The user branch of `je_arm_sched_irq` calls `record_current_frame` a second time, which hits the "frame not consumed" fatal.
- injA (timer left running): `FATAL sched frame not consumed` on the first tick.
- injA2 (delete line 999): the **full proof passes with preemption live**.
- injA3 (A2, plus a user that sets SP to 0x1234 and spins): `FATAL sched observed_running`. A user-controlled SP_EL0 at IRQ time goes through `frame_shape_valid` and becomes a fatal. The same applies to the no-tick path at line 963.
- The EL0 SPSR policy leaves I unmasked, so any IRQ source enabled while EL0 runs halts the kernel. The proof only passes because it disables the timer (`el0_test.c:398`).
- `IMPLEMENTATION.md` calls slot 9 "built and reviewed" and says "MP3a is now complete". Without the timer there is also no preemption, so an EL0 `b .` hangs the system.

**B2. A lone surviving process halts the kernel by calling yield.** (`sched_arm.c:822-824`)
- `arm_switch_to_ready` picks the next process while the current one is still RUNNING, so it can never re-select itself. x86 marks the current thread READY first (`sched.c` ~331).
- injD (one process): `FATAL sched no live EL0 successor` on its first yield.
- injD3 (process 2 calls exit(7), is reaped, then process 1 yields): the same FATAL.
- So reap-with-survivor fails in the ADR's minimum 2-process scope, and once B1 is fixed it will also fire on every tick.

## SHOULD-FIX

- **S1. The SVC path kills the process instead of halting on kernel-invariant breaks** (`sched_arm.c:907-908`).
  - A BAD frame (position or vector/mode mismatch) is turned into a user death. The ADR table says fatal, and `je_arm_el0_fault` (line 890) does treat it as fatal.
  - injF (SPSR.M forced to EL1h) and injF2 (vector forced to 5) both give `TERMINATED reason=2` with no fatal.
- **S2. The proof cannot see register or return-value round-trips.**
  - The ABI-query op, ABI version and OK status are all 0, and x0/x1 are already 0 before the SVC. The user code never reads x0/x1/x16–x18 after the SVC returns.
  - Each of these still passes the proof:
    - x16–x18 clobbered on EL0 return (injB);
    - all x0–x30 zeroed on EL0 return (injB2);
    - status/result never written back (injC).
  - ADR D4 lists "x16–x18 survive an SVC" as a required proof item, and the docs claim it.
- **S3. Mutant claims are overstated.**
  - `MODE-CONFUSION` dies at the first dispatch because EL0 frames are rejected; it does not test an EL1h frame being accepted.
  - `NO-SP-EL1-INSTALL` and `DROP-SP-EL0-RESTORE` are killed only by the test harness's `reap-mismatch` fatal, which fires after the S1 downgrade. They are not killed by a named check.
- **S4. The per-page TLB flush for user pages uses the wrong ASID** (`vm_backend.c:396-397` → `paging.c:929/957`).
  - `tlbi vae1is` is issued with ASID 0, so it misses the non-global ASID-1 user entries.
  - Today it is safe only because `vm.c:155-156` follows with a full `vmalle1is` flush. The comment still says the leaves are global.
  - This is the ADR-flagged hazard and is latent for MP3b. Reviewed, not observed on QEMU.
- **S5. EL0 can still use the debug comms channel** (`sched_arm.c:597`).
  - Writing `mdscr_el1 = 0` leaves its EL0-trap bit (TDCC, bit 12) clear.
  - injJ: `mrs MDCCSR_EL0` and `msr DBGDTRTX_EL0` both executed at EL0 without trapping. On hardware the TX-full flag persists, which is a cross-process channel like TPIDR.
  - Also, removing the CNTKCTL/PMUSERENR/MDSCR writes still passes the proof (injN2), because QEMU's reset values already trap.
- **S6. A frozen EL1h phase-0 check was deleted.** HEAD~1 line 246, `phase == 0 && frame_address != top - size`, is gone from the shared prefix (it belonged near line 292). Restore it.
- **S7. ADR D3 accessors are missing.** `je_arm_tf_kernel_sp`/`je_arm_tf_user_sp` and the `make check` ban on raw `->sp` were not implemented. Raw writes remain at `el0_test.c:325` and `main.c:1361` and `:1380`.

## NIT

- `vectors.S:258` and `:291` add two instructions to the frozen restore in every build, not only EL0 builds. They are inert; the docs should say "behaviorally identical" rather than "unchanged".
- The smoke oracle (`smoke_arm.py:1150-1152`) asserts FAR=0x40000000 for EC 0x07/0x18/0x15, where FAR is architecturally UNKNOWN. It is stale from stage 2: injI2 moved the stage 0–2 address and stages 5–7 followed it.
- The oracle doesn't check the fault syndrome detail. injI shows they really are permission faults on mapped kernel pages, with the write flag set only for the write case, but a regression there would pass.
- `-DJE_SYSCALL_HOSTED_TEST` is used for a kernel build (`mk/aarch64.mk:227`). It does link only `je_syscall_decode` and `je_syscall_recv_staging_cap`, with no undefined symbols.
- `JE_ARM_NO_DEVICE_HOOKS` in the arch-neutral `lifecycle.c` silently skips waiter release; add a tripwire before ARM gets endpoints.
- `paging.c:862` and `:868` hard-code a 64-byte line stride and invalidate the I-cache by identity VA. That is correct for cortex-a72; record it as a target-profile assumption.
- The VALE1IS→VAE1IS change edits MP2-tested code without flagging it, and its comment is now stale.
- A hardening-readback refusal is reported as "frame validation" (line 853).
- Zombies are reaped only on SVC entry. TPIDRRO_EL0 zeroing is untested.

## Other confirmations

- The SVC glue snapshots arguments before writing x0. Non-MP3a ops return UNSUPPORTED (injQ1), a non-zero SVC immediate kills (stage 7), and exit works.
- MMU-on W^X ordering is correct: the staging alias is removed, and the cache maintenance done, before the user page is published.
- The reap path clears the stale root before freeing and has no double free.

**Reviewed, not observed on QEMU TCG:** the cache maintenance and TLB/ASID effects, DCC persistence on hardware, and the IL≠1 check on SVC.

**Minimal fix set:**
1. Delete `sched_arm.c:999`.
2. Turn a bad user SP/ELR at IRQ/no-tick record time into user death.
3. Let `arm_switch_to_ready` re-select a live current process (x86 parity).
4. Make BAD frames fatal at SVC entry.
5. Make the proof check post-return registers with distinguishable non-zero values, and add a timer-live stage and a single-survivor stage.

Then re-run the proof and mutants.
