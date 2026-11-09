# Adversarial review — MP3a-2 ARM EL0 entry/return + SVC (Codex wrote; you review). EMPIRICAL + injection.

You are the adversarial reviewer (different lineage). Codex implemented MP3a-2 of ADR 0015
(docs/adr/0015-mp3a-aarch64-el0-and-svc.md, accepted): the FIRST eret to EL0 on aarch64,
SVC syscalls, lower-EL fault death, and a two-EL0-process execution proof. This is the
highest-stakes code in the project — the first user/kernel boundary, touching the frozen
MP2 asm. TRY TO BREAK IT. The change is commit HEAD (git show HEAD); worktree is the
current dir. Do scratch builds/boots + injection in /tmp (the MP3a-1 review found a REAL
blocker this way — do the same here).

## Lead already VERIFIED (confirm, don't just re-run): EL0 proof PASSES (real EL0 exec,
SVC abi_query x8-op round-trip with x16-x18 canaries + TPIDR=0, per-process isolation
0x1111 vs 0x2222 at the same arena VA, fault-death EC 0x24/0x20 + reap-with-survivor,
sp-mutated BAD_RETURN); 9/9 ARM smoke profiles + MP3a-1 EL1 proof still pass; x86 make
check unchanged; make mutants 49 killed/0 BLOCKED with 6 new EL0 mutants. trapframe.h
change is comment-only (sp semantics; offsets/size/_Static_asserts unchanged).

## Attack these specifically (the catastrophic-if-wrong areas)
1. THE FROZEN EL1h PATH. vectors.S je_arm_irq_common (~168-277) and the slot-5 veneer are
   MP2-frozen. MP3a-2 added EL0 slot-8/9 savers + an EL0 branch in the restore tail. Is the
   EL1h/IRQ restore sequence byte-for-byte behaviorally UNCHANGED for an EL1h frame? Does the
   new EL0 branch (msr SP_EL0 / zero TPIDR_EL0, gated on SPSR.M==EL0t) ever execute on an
   EL1h frame or corrupt it? Inject: force the EL0 branch to run on an EL1h frame and confirm
   it breaks (then confirm the real gate prevents it).
2. THE FIRST EL0 HANDOFF (ADR-flagged biggest risk). TTBR0 + SP_EL1 + the EL0 frame + ERET
   must all agree. Trace exactly: at the moment of eret to EL0, is TTBR0 the process root
   (ASID 1)? Is SP_EL1 the process's kstack top so the NEXT EL0->EL1 trap lands correctly?
   Does the EL0 frame's SPSR have M=EL0t and the right DAIF? Is SP_EL0 set from tf->sp? If
   any is wrong it faults before C runs. Inject each (drop SP_EL1 install, wrong TTBR0, wrong
   SPSR.M) and confirm detection.
3. THE EL0 VALIDATOR SPLIT (sched_arm.c frame_shape_valid). The discriminator must be the
   VECTOR GROUP x SPSR.M, NOT a slot-fixed class. Can an EL1h frame be accepted as an EL0
   return, or vice versa (mode confusion)? Is SPSR.M read only AFTER the position proof (the
   frame is in AP=00 kernel memory EL0 can't write; HW writes M on entry)? Is the phase-1
   captured-EL0-frame ELR/SP_EL0 arena range check actually enforced (= user death), or can a
   user forge an ELR/SP outside the arena and survive? The ARM-USER-ACCEPT-BAD-USER-ELR
   mutant claims to guard this — verify it's load-bearing (delete the real check -> survives).
4. SVC GLUE. op from x8, args x0-x5, status x0, result x1 (always written). Does a non-zero
   SVC immediate / EC!=0x15 / IL!=1 kill the process? Is x0 written only AFTER decode (not
   before, clobbering arg0)? Can a decoded handle/IPC action (not abi_query/yield/exit)
   return success instead of UNSUPPORTED? Is je_syscall_decode linked WITHOUT dragging the
   x86 je_syscall_dispatch (check how syscall.c/decode got into ARM_OBJS — a split TU or a
   define; confirm x86 make check is byte-identical)?
5. FAULT DEATH + REAP. Every lower-EL sync exception (incl. a bad SVC) must terminate ONLY
   the offending process; peers survive; the dead process is reaped. Can a crafted EL0 fault
   halt the kernel or corrupt a peer? Does the reap free the right process's tables/stack
   (MMU-ON now — MP3a-1 was MMU-off)? Is there a double-free or a stale TTBR0 after reap
   (the MP3a-1 review flagged the stale-root hazard — is the unbind present)?
6. MMU-ON vm_backend/paging. MP3a-1 failed closed on MMU-on map/clear/flush; MP3a-2 enables
   them. Is the clear_pte TLBI correct (scope/operand)? Is the cache maintenance for a
   USER_CODE frame (dc cvau + ic ivau) present before EL0 executes it (reviewed-not-observed
   on TCG, but must be present + correct by construction)? Any W^X hole now that user frames
   are published MMU-on? Can je_arm_mmu_map_page(user_code_pa, RW) still be refused?
7. SYSREG HARDENING. CPACR_EL1.FPEN=00 (FP traps), CNTKCTL_EL1 (no EL0 timer reprogram),
   TPIDR_EL0 zeroed per entry, SCTLR readback (not a boot.S edit). The proof shows FP/CNTVCT
   kill — but are the sysregs set on EVERY EL0 entry or once? If once, can a later context
   observe a stale value?
8. Anything else: an off-by-one in the EL0 saver's SP_EL0 capture, a GPR not restored, a
   marker that can't fail, a mutant that would survive, an EL0 frame whose GPRs aren't zeroed
   (kernel register leak to ring 3).

## Read/run
git show HEAD; kernel/arch/aarch64/{vectors.S, sched_arm.c, el0_test.c, main.c, vm_backend.c,
paging.c}, kernel/core/syscall.c, tools/smoke_arm.py, mk/aarch64.mk. Build+boot:
make aarch64 ARM_EL0_TEST=1 ; make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1. Inject in
/tmp scratch to prove detectors load-bearing. Record TCG-unobservable items as
"reviewed, not observed".

## Output
BLOCKER / SHOULD-FIX / NIT, each file:line + exact scenario (+ your empirical/injection
result). For items 1,2,3 state definitively whether the frozen EL1h path is intact and
whether an EL1h<->EL0 mode confusion is possible. End with SAFE TO INTEGRATE / FIX FIRST.
