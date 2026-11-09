# MP3a reconciled design (both lineages agreed unless noted)

## D1 — Kernel VA regime: SHARED per-process TTBR0, DEFER the TTBR1 relink
Each process gets its own TTBR0 root that:
- shares the kernel's mappings (global, EL1-only, AP[1]=0) — copied byte-for-byte
  from the kernel root's L0 descriptors (kernel slots stay in L0[0..31]; audit
  proc_L0[i]==kern_L0[i]);
- adds a private user-arena branch (AP[1]=1, nG=1).
Switch TTBR0 (+ shared user ASID 1, TLBI ASIDE1IS;DSB;ISB on every install) + the
per-process SP_EL1 on dispatch. EPD1 stays 1 (TTBR1 UNKNOWN never walked); NO relink,
NO linear map. Isolation comes from AP/UXN/PXN/nG + per-process roots within one
translation regime (the kernel is mapped while EL0 runs; exceptions enter EL1 via
VBAR_EL1/SP_EL1 without a translation-regime switch). This is x86 parity (every x86
root maps the kernel supervisor-only; ADR 0007 D3). Rejected: TTBR1-high-half-now
(largest tested-path blast radius: boot.S relink + load==link, linker.ld, paging_enable.S,
identity assumptions, all MMIO/DTB/PL011 pointers, every MP1/MP2 FAR/ELR smoke
baseline) — no isolation gain; and one-shared-root (breaks D3 distinct-roots).

CRITICAL: the ARM user arena CANNOT stay at 0x40000000 (kernel linked at 0x40080000,
inside it). Move the ARM arena to VA >= 2^44 (e.g. 0x0000_1000_0000_0000 = L0[32]) so
it's provably disjoint from the kernel identity map (audit forces kernel leaves
VA==PA, PA<2^44) AND stays valid under a future high-half kernel (user ABI stable
across the later relink). JE_VM_ARENA_BASE (vm.h) becomes per-arch (x86 value
unchanged); JE_SVC_REPORT_VADDR (svc.h) becomes arena-relative.

## D2 — SVC ABI: SVC #0, x8=op, x0-x5=args, x0=status, x1=result
- x8 (not x16/x17 = IP0/IP1 veneer scratch, also used by the ARM IRQ veneer).
- Op in a register (mirror x86), NOT the SVC immediate; a non-zero SVC immediate
  kills the process (analogue of ring-3 int 0x81 -> #GP). Check ESR.EC==0x15, IL==1.
- Result x1 ALWAYS written (0 when none; mirrors x86 rdx=0 on failure). Snapshot
  x8+x0-x5 before writing anything; write status/result last; park path never writes x0.
- je_syscall_decode is reused as-is (op+args[6]->{status,result}); a thin ARM glue
  marshals gpr[]->args[6] and back. Do NOT generalize x86 je_syscall_dispatch.
- MP3a ops: abi_query, yield, exit. The ARM glue default: returns UNSUPPORTED for any
  other decoded action (handle/IPC), never success.
- Restart (MP3c, deferred): rewind ELR by 4 (not x86's 2), only for vector-8/EC-0x15/
  IL-1 frames; an EL0 IRQ's ELR is the interrupted PC, never rewound.
- SVC is ALWAYS live at EL0 (no DPL gate to leave closed) -> the first PR that ERETs
  to EL0 MUST route every lower-EL synchronous exception (incl. SVC) to process death
  (the ADR 0007 #4 hazard).

## D3 — user trap frame: [PENDING TIE-BREAK DEBATE] separate type (Codex) vs dual-use sp (Claude)
Converged regardless: the EL0 validator must require SPSR.M==0 (EL0t), M[4]=0 (no
AArch32), re-sanitize SPSR to (spsr&NZCV)|0x340, frame at exactly kstack_top-304,
ELR 4-aligned in [base,end), SP_EL0 in (base,end]; wrong position/mode/cookie = fatal,
bad ELR/SP = user death. The EL1h `sp==frame_end` check becomes EL1h-only. Slots 8/9
need dedicated veneers (spill x16-x19, read SP_EL0). Restore tail for EL0: msr SP_EL0,
zero TPIDR_EL0, then ERET. A fresh EL0 frame zeroes ALL GPRs (no arg in x0 leak).

## Owner-gated items needing the ADR + sign-off (frozen/tested-path)
1. ADR 0011 D2 schedule: DEFER the TTBR1 relink to a later MP3 slice (D2's Flex names
   this PR as the revisit point) — record the deferral + the trigger (IPS>44 / KASLR /
   software-PAN-via-EPD0).
2. The MP2 paging audit's "table attributes must be 0" rule REVERSES for the kernel
   root's L0 (APTable=01, UXNTable=1) + the arena L0 (PXNTable=1) — hierarchical deny
   per ADR 0007 D3 "never on a leaf bit alone". These bits only restrict EL0 so they
   can't mask the EL1 W^X proofs.
3. New EL0 leaf classes + their exact-match audit literals (nG=1): code
   0x0020000000000FC3, rodata 0x0060000000000FC3, data 0x0060000000000F43 (Claude's
   values — VERIFY bit-by-bit before trusting).
4. The arch-neutral je_vm_audit hardcodes x86 PML4[0]/PDPT[0..1]/PD[0] indices
   (vm.c) -> needs an arch topology hook; M3a-tested code, x86 behavior byte-identical.
5. The vectors.S restore tail (MP2-tested) gains the EL0 branch.
6. The sched_arm.c validator split (EL1h vs EL0t).
7. trapframe.h `sp` wording (or a new type) — per the tie-break.
8. Per-arch arena constants (vm.h JE_VM_ARENA_BASE, svc.h JE_SVC_REPORT_VADDR).
9. The runtime mapper can't create tables + JE_ARM_MAX_TABLES=128; pre-building L3
   coverage for 256 MiB RAM needs ~134 tables -> cap bump; measure the pre-enable
   alias-scan TCG runtime.

## MP3a scope (staged)
- MP3a-1 (verified at EL1, like x86 #4 step 2): the ARM je_vm_ops backend, L3
  coverage, per-process roots, kernel-L0 attributes + audit change, vm.c topology
  hook, per-arch arena, TTBR0/ASID install. Proof: AT S1E0R/S1E0W probes from EL1
  (kernel text/data/devices -> PAR.F=1; arena -> per-kind perms), cross-root
  consistency audit, mutants.
- MP3a-2: slot 8/9 veneers + restore tail, validator split, EL0 frame setup, process
  death for every lower-EL sync exception, EL0-exposing sysreg hardening (CPACR_EL1
  FPEN=00, CNTKCTL_EL1, TPIDR_EL0 zero, PMUSERENR/MDSCR, SCTLR readback), the 3-op
  SVC ABI, exit + reap-with-survivor via lifecycle.c. Proof: two EL0 procs; EL0
  r/w/x of kernel mem -> exact ESR/FAR + process death; same VA different content per
  proc; bad SP_EL0/ELR kills; FP/CNTVCT/SVC#1 kill; x16-x18 survive an SVC (canary);
  TPIDR_EL0 no leak.
- Deferred: MP3b (user copy ELR-keyed fixup + handles), MP3c (IPC + blocking restart),
  later (per-process ASIDs, TTBR1 relink, separate exception stack, software-PAN).

## Biggest risk
(1) The first user-origin exception handoff: TTBR0/SP_EL1/frame/ERET must all agree
or it faults before C runs -> looks like a boot hang. (2) TLB maintenance for
non-global user leaves QEMU can't see (leaf_changed_sync hardcodes ASID 0): use the
shared user ASID + flush on every install; capture TLBI operands in the hosted seam
(je_arm_tlbi_hosted_last_operand); record anything unobservable as "reviewed, not
observed".


## D3 RESOLVED (tie-break debate, both lineages -> DUAL-USE-SP, option c)
`tf->sp` (offset 248) = whichever SP the interrupted PSTATE used: SP_EL1 for EL1h
frames, SP_EL0 for EL0t frames, selected by SPSR.M. ONE 304B layout, no new type.
Rationale: the scheduler holds the frame as an opaque void* and the restore tail
(after `mov sp,x0`) must read the NEXT frame's SPSR.M@264 to decide whether to
`msr SP_EL0` regardless — a cross-privilege switch (EL0 IRQ resuming an EL1h idle
frame, or vice versa) is normal, so (b)'s "separate restore tail by vector" can't
work; (b) would fork the MP2-tested je_arm_irq C chain or cast between two 304B
structs (= (c) plus a strict-aliasing hazard under -O2 without -fno-strict-aliasing).

ORCHESTRATOR RESOLUTIONS of the two residual disagreements:
- DISCRIMINATOR (Codex right, Claude conceded direction): NOT an immutable per-slot
  class. A user process legitimately holds an EL0 frame OR an EL1h frame while
  servicing a kernel entry. Discriminator = the validated VECTOR GROUP (4-7 => EL1h,
  8-11 => EL0t) cross-checked against SPSR.M; a mismatch is fatal. SPSR.M is read
  from fixed offset 264 ONLY after the position proof (frame + full 304B lie inside
  the slot's mapped kernel stack, AP=00 so EL0 can't write it; HW writes M on entry
  so a user can't forge it). SPSR.M never trusted alone.
- ELR/SP_EL0 RANGE CHECK on captured EL0 frames (I side with Codex, addressing
  Claude's flakiness worry): VALIDATE ELR in [arena_base,arena_end) 4-aligned and
  SP_EL0 in (arena_base,arena_end] 16-aligned on EL0 return. This is exact x86
  parity -- ADR 0007 D6 requires "validate canonical RIP/RSP within the arena on
  EVERY user return"; dropping it would be weaker than the frozen x86 contract.
  Invalid ELR/SP_EL0 => USER DEATH (not sched_fatal). Claude's flakiness concern is
  handled by making the negative PROOF deterministic (drive preemption/entry at a
  known PC), NOT by dropping a trust-boundary check.

Accessors (both agreed): add je_arm_tf_kernel_sp() (fatal unless EL1h) and
je_arm_tf_user_sp() (fatal unless EL0t); a `make check` grep bans raw `->sp` outside
the saver/validator/evidence code. Copy-fixup (MP3b) stays EL1h-only, gated on
ESR.EC==0x25 && M==EL1h && vector==4. A fresh (phase-0) EL0 frame: ELR==image entry,
sp==arena stack top, ALL GPRs zero. Restore-tail addition after
`ldp x0,x1,[sp,#JE_TF_ELR]`: `tst x1,#0xf; b.ne 1f; ldr x2,[sp,#JE_TF_SP];
msr SP_EL0,x2; 1:` (only when the resumed frame is EL0t).

## FINAL validator split (authoritative)
SHARED (fatal on fail): position proof (frame+304 inside slot kstack) BEFORE any
deref; M[4]==0 (AArch64); vector-group<->SPSR.M agreement; pad generation cookie.
EL1h-only (fatal): sp==frame_end; (spsr&~NZCV)==0x345; ELR in kernel text; phase-0
trampoline+gpr[0].
EL0t-only: frame exactly at kstack_top-304 (fatal; MP3a keeps I masked in SVC
handlers, no preemptible syscalls yet); force spsr=(spsr&NZCV)|0x340 on resume
(never fatal; DIT/SSBS/TCO absent on cortex-a72, record as target-profile
assumption); phase-0 ELR==entry & sp==arena top & GPRs zero (fatal); phase-1
captured: ELR in [base,end) 4-aligned & SP_EL0 in (base,end] 16-aligned (USER DEATH,
not fatal).
