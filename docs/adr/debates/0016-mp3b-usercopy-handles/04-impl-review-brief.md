# MP3b adversarial review (empirical, with injection) — the load-bearing gate

You are an ADVERSARIAL reviewer. TRY TO BREAK the MP3b implementation. Do NOT rubber-stamp.
JeiOS freestanding ARM64 kernel; code in git worktree `/tmp/wt-mp3b` (branch `wip/mp3b`,
HEAD 5b338b7). You may build, boot in QEMU, read code, and INJECT faults (edit→build→boot→
REVERT; leave the tree clean, or work in scratch copies). Work only in /tmp/wt-mp3b or /tmp.

## What MP3b did (owner-approved design in docs/adr/0016-mp3b-aarch64-usercopy-and-handles.md)
Part A: ARM usercopy with a RETURNABLE, exact-ELR-keyed copy-fault fixup. A usercopy fault is
a kernel-mode (EL1h) data abort (EC=0x25, vector 4). Normally that halts (`je_arm_exception`
`_Noreturn`) and the double-fault guard `je_arm_in_handler` is SET during the whole EL0 SVC.
The approved design (Option A') makes EXACTLY the copy-site fault returnable:
- new `kernel/arch/aarch64/usercopy_arm.S`: TWO leaf routines — `je_copy_from_user_run`
  (LDTRB user read + STRB kernel write) and `je_copy_to_user_run` (LDRB kernel read + STTRB
  user write); exact labels `je_copy_user_load_site`/`je_copy_user_store_site`/
  `je_copy_user_fixup`; x0-x3+x30 only (x16-x18 are frame scratch and must survive).
- `kernel/core/usercopy.c`: portable scope + prevalidation reused; ARM `je_arm_copy_fault(tf)`
  predicate recovers ONLY IF vector==4, SPSR.M==EL1h(0x5), EC==0x25, IL==1, ISS[10:0] EXACTLY
  0x007(load)/0x047(store) (translation L3), exact-ELR == the direction's site, FAR in
  [scope.user_lo,user_hi), scope active + owner==current (core + ARM current-user), AND live
  TTBR0_EL1.BADDR == je_arm_user_root[slot]. Else returns 0 (stays fatal).
- vectors.S + main.c: `je_arm_in_handler` is now TRI-valued (0 none / 1 in-SVC / 2 in-copy-check).
  The guard-hit branch runs a stack-free prefilter (ELR ∈ {load,store site} AND guard==1), sets
  guard=2, builds the frame, calls the C fault dispatch; on reject -> fatal (_Noreturn); on
  accept ELR:=fixup then a DEDICATED restore tail (asserts x0==sp, elr==fixup, (spsr&~NZCV)==
  0x3c5, sets guard:=1 NEVER clears, restores GPRs, add sp,#304, eret). Does NOT use
  je_arm_restore_selected and does NOT touch frame_shape_class. je_arm_exception stays _Noreturn.
Part B: ARM-local SELF-only handle glue (new `kernel/arch/aarch64/syscall_arm.c`) wiring
HANDLE_SELF/CLOSE/QUERY/RIGHTS/DUP + COPY_PROBE through je_syscall_decode + the SELF-only handle
ops; non-SELF type => WRONGTYPE. x86 syscall.c is BYTE-UNCHANGED (ARM builds it with
-DJE_SYSCALL_HOSTED_TEST which excludes the x86 dispatch).
Part C: positive proof (copy_probe lengths 0/256/257, handle round-trip, COPY_FIXUP_RECOVERED
count=2 guard=1) + 8 one-build-per-stage negative FATAL proofs (ARM_COPY_FAULT=exact-elr/
far-bound/owner/direction/permission/l0/live-root/restore-selected) + 6 mutants.

## Build/run (from /tmp/wt-mp3b)
- `make aarch64 ARM_EL0_TEST=1`; raw boot (virt-8.2,cortex-a72,-smp 1,256M,-serial file) must end
  `EL0 proof PASS reviewed-cache-tlb=not-observed`.
- Negative stage N: `make aarch64 ARM_COPY_FAULT=<name>` -> boot `build/aarch64-copyfault-<name>/Image`
  must FATAL (not PASS). Names: exact-elr far-bound owner direction permission l0 live-root restore-selected.
- `make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60`; `make check`; `make mutants`.

## ATTACK THIS — the questions that matter most
1. **Can a NON-copy EL1h data abort EVER be mis-recovered (turned returnable)?** This is the
   whole safety of touching the frozen no-return invariant. Try: a real kernel bug (stray deref
   of a mapped user addr) during the SVC; a fault at an instruction NEAR but not equal to a site;
   a second fault inside the copy-fault check itself (must be a double fault via guard==2); a
   fault when guard==0 or a non-SVC context. Confirm each stays fatal/double-fault. Inject to prove.
2. **Is the 3-state guard correct under every path?** Does guard ever get stuck at 2, or cleared
   to 0 mid-SVC (the B2 hazard from design review)? After a recovered copy, is guard exactly 1 and
   is the double-fault guarantee intact for the REST of the SVC? Try a second, non-copy fault after
   a recovered copy in the same SVC — must be a double fault, not a C re-entry.
3. **Does the returnable path corrupt frame scratch / registers?** x16-x18 hold the vector/guard
   scratch. Confirm the copy routines + the dedicated restore tail preserve them and x30, so the
   leaf `ret` returns correctly. Check the canary registers in the proof survive.
4. **Is each negative stage LOAD-BEARING, not just marker-present?** For EACH of the 6 mutants and
   the 8 stages, confirm the guarded check actually makes the difference: with the check removed the
   build must PASS-wrongly or hang (proving the stage catches a real regression). The lead already
   confirmed exact-elr + permission FATAL; you independently confirm translation-only (a permission
   fault at the exact site must stay fatal — the most security-critical), live-root (a wrong root
   must not become a silent cross-process copy), owner, direction, guard-3-state, restore-selected.
5. **Is the SELF-only handle glue fail-closed and x86 truly untouched?** Can any non-SELF type reach
   a non-WRONGTYPE path? Does the handle round-trip actually attenuate rights + reject a stale handle?
   Confirm x86 syscall.c/usercopy.S codegen is unchanged (build x86, the lead saw sources untouched).
6. **Vacuity sweep (MP3a-2 lesson):** is any positive marker emitted without the property holding?
   Could COPY_PROBE_PASS print if copy_from/to_user moved WRONG bytes? Could COPY_FIXUP_RECOVERED
   print without a real fault+recovery? Is the fixup count genuinely incremented only on recovery?

## Output
VERDICT: SHIP or FIX-FIRST. If FIX-FIRST, each item with the exact injection that exposed it and
observed-vs-expected serial. Quantify effort (boots, injections). A clean "I tried X/Y/Z and
could not break it" is valuable ONLY if you actually ran the injections. This is AI-assisted
review, not human qualification.

## Slack (owner directive)
Post substantive findings directly to the agent group channel:
curl -s -X POST "SLACK_WEBHOOK_URL_INJECTED_AT_DISPATCH" -d '{"Text":"[claude] <finding>"}'
