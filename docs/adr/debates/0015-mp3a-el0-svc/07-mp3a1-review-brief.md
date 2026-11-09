# Adversarial review — MP3a-1 ARM per-process TTBR0 + EL0 leaves (Codex wrote; you review)

You are the adversarial reviewer (different lineage). Codex implemented MP3a-1 of
ADR 0015 (docs/adr/0015-mp3a-aarch64-el0-and-svc.md, accepted): per-process ARM
TTBR0 roots that share the kernel mappings (EL1-only) + a private EL0 user arena,
PROVEN AT EL1 via AT S1E0R/W probes (NO eret to EL0 yet — that's MP3a-2). TRY TO
BREAK IT. The change is the single commit HEAD (`git show HEAD`); the worktree is
the current dir.

## What it had to do (ADR 0015 D1 + the brief)
- New kernel/arch/aarch64/vm_backend.c implementing the arch-neutral je_vm_ops
  (include/jeios/vm.h), mirroring kernel/arch/x86_64/vm_backend.c: per-process TTBR0
  root that COPIES the kernel root's L0 descriptors byte-for-byte (kernel EL1-only,
  AP[1]=0, slots in L0[0..31]) + a private arena branch. Kernel-root L0: APTable=01,
  UXNTable=1; arena L0: PXNTable=1 (hierarchical EL0 deny).
- EL0 leaf classes JE_MAP_USER_CODE/RODATA/DATA (AP[1]=1, nG=1) with exact-match
  audit literals + a per-process audit.
- Per-arch JE_VM_ARENA_BASE: aarch64 0x0000100000000000 (2^44), x86 0x40000000
  unchanged.
- arm_dispatch_install installs TTBR0 + shared ASID 1 + TLBI ASIDE1IS (SP_EL1 install
  deferred to MP3a-2, stated).
- AT S1E0 probe; a JE_ARM_USER_TEST toggle + an EL1 proof + a smoke --user-test oracle.

## VERIFIED by the lead already (confirm, don't redo):
- The EL0 leaf literals are CORRECT (I derived them independently; my formula also
  reproduces the 3 frozen kernel literals): USER_CODE=0x0020000000000fc3,
  USER_RODATA=0x0060000000000fc3, USER_DATA=0x0060000000000f43. _Static_asserts pin
  them + assert AP[1] set on all three and UXN=0 only on USER_CODE.
- The EL1 proof passes: kernel VA AT S1E0R/W -> PAR.F=1; code RO-exec; data RW;
  rodata RO; same arena VA -> different PA under p1 (0x4010b000) vs p2 (0x4011b000);
  per-process audit PASS; TLBI operand 0x0001000000000000 recorded. All 8 existing
  ARM smoke profiles + x86 make check + make mutants (0 BLOCKED) still pass.

## Attack these specifically
1. **Is the isolation PROOF actually sound, or does it just "not crash"?** The proof
   decides success from AT S1E0 PAR values (kernel/trusted state), good. But: are the
   probes hitting the RIGHT addresses (a real kernel text VA, a real arena VA for the
   CORRECT process whose root is currently installed)? Could the "different PA"
   isolation result be an artifact (e.g. both probes run under the same root, or the
   PAs differ for a reason other than distinct per-process mappings)? Is the audit-PASS
   marker load-bearing (would a broken audit actually fail the run)?
2. **Kernel-L0 sharing consistency.** The design requires proc_L0[i] == kern_L0[i]
   for the kernel slots, and that user entries never land in a kernel L0 slot. Does
   the code enforce BOTH (a per-process audit that the copied kernel L0 entries are
   identical AND that the arena's L0 index is NOT a kernel slot)? What if the arena VA
   2^44 and a kernel VA share an L0 index — do they? (2^44 is L0 index 32 at 4KiB/48-bit;
   kernel is at 0x40080000 = L0 index 0. Confirm they're disjoint L0 slots.)
3. **Hierarchical deny.** APTable=01 on the kernel-root L0 restricts EL0 at the table
   level. But these kernel L0 descriptors are COPIED into each process root — is the
   APTable/UXNTable actually SET on the copies, and does it break the kernel's OWN
   EL1 access (it must not — APTable=01 means "no EL0 write below", EL1 unaffected;
   confirm the encoding doesn't also restrict EL1)? Does setting PXNTable=1 on the
   arena L0 correctly prevent EL1 from executing user code (PXN) while allowing EL0
   exec of USER_CODE (UXN=0 at the leaf)?
4. **ASID / TLB.** Shared user ASID 1, TLBI ASIDE1IS on every install. The kernel
   uses ASID 0. Is the ASID actually encoded into TTBR0_EL1[63:48] on install? If two
   processes share ASID 1, does switching from p1 to p2 under the SAME ASID without a
   broad enough flush risk a stale p1 translation serving p2 (the brief said flush on
   every install — is the TLBI the right scope: ASIDE1IS flushes ALL ASID-1 entries,
   which is correct here)? This is the "reviewed not observed on QEMU" hazard — is the
   flush at least PRESENT and correct by construction?
5. **Did it touch the frozen kernel bring-up?** The existing je_arm_mmu_audit and the
   kernel map must be UNCHANGED (the per-process audit is separate). Confirm the MP2
   kernel audit path and je_arm_mmu_build are not altered. Confirm je_arm_trapframe is
   untouched. Confirm no eret-to-EL0 / SVC / user frame leaked in (that's MP3a-2).
6. **The table cap bump + L3 pre-build.** JE_ARM_MAX_TABLES was 128. Is the new value
   sufficient (kernel + 2 process roots + arena branches + any L3 pre-build) with
   headroom, and does the pre-enable alias-scan still terminate in reasonable TCG
   time? Any unbounded loop or off-by-one in the per-process table allocation?
7. **W^X across the alias.** A user CODE/RODATA frame — does it also have a writable
   kernel (shared-mapping) alias? The kernel root maps RAM... does the arena's backing
   frame have a WRITABLE kernel leaf anywhere (which would be a W^X hole: EL0-exec via
   the arena + EL1-writable via the kernel map)? How is that prevented — are user
   frames drawn from a region the kernel does NOT map writable, or is a kernel alias
   demoted? (x86 demotes the identity alias to RO/NX for USER_CODE/RODATA — does ARM?)
8. Anything else: an off-by-one in va_index for the 2^44 arena, a wrong PAR decode in
   the oracle, a marker that can't fail, a mutant that would survive.

## What to read / run
`git show HEAD`; kernel/arch/aarch64/vm_backend.c (new, ~673 lines), paging.c (the
EL0 classes + per-process audit), sched_arm.c (arm_dispatch_install), main.c (the
JE_ARM_USER_TEST proof), tools/smoke_arm.py (the --user-test oracle), include/jeios/vm.h.
You may build + run in /tmp scratch: `make aarch64 ARM_USER_TEST=1`,
`make smoke-aarch64 ARM_USER_TEST=1 ARM_REQUIRE=1`, and mutate a check to confirm it's
load-bearing (e.g. make the audit always-pass, or point a probe at the wrong root, and
confirm the proof then FAILS). Do scratch work in /tmp, never the worktree.

## Output
BLOCKER / SHOULD-FIX / NIT, each file:line + the exact scenario (+ your empirical
result where you ran one). For item 7 (W^X alias) state definitively whether a user
code/rodata frame can be reached writable from the kernel. End with SAFE TO INTEGRATE
/ FIX FIRST.
