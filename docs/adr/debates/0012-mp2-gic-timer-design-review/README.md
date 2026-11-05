# ADR 0012 design review — transcript and resolution

Two-lineage design review BEFORE any MP2 PR-2 code, 2026-11-05. ADR 0000 part-4
attachment. Codex = implementor's-eye (asm + register sequence); Claude =
adversarial correctness / false-success.

## Convergent findings (both lineages, folded into ADR 0012)

1. SPLIT at "IRQs stay masked": PR-2a (GIC+timer, POLLED, no vector change) +
   PR-2b (returning IRQ path). Pinned as D-SPLIT.
2. GIC gaps the draft missed: interrupt GROUP (IGROUPR0 bit30 must be group 0,
   else IAR returns 1022), FIQEn=0 in GICC_CTLR (else the timer routes to the
   FATAL FIQ slot 6), PMR strictly-less-than-priority (0xFF/0xFF never fires),
   SecurityExtn refusal, EOImode=0. Pinned in D4.
3. TPIDR_EL1 spill is WRONG (one reg, three values, SMP per-CPU ptr). Correct: a
   dedicated slot-5 stub spilling x16-x18 to the stack frame. Pinned in D5.
4. The proof must prove RETURN + register restore, not "a tick happened": a
   register-canary asm loop (seeds x16-x18), tf->sp equal across ticks, ELR in
   the loop's symbol range, NO printing in the handler, heartbeat between ticks,
   ISR_EL1.I==1 (not SPSR.I) for async, N>=3 ticks, a BRK negative control.
   Pinned in D8.
5. Re-arm TVAL then isb before EOIR; sample CNTPCT BEFORE re-arm for the
   delta>=interval bound; spurious is INTID>=1020 not ==1023. Pinned in D6.
6. D1/D7 contradiction: default configures but does NOT unmask; only the test
   build unmasks. Resolved in D1.
7. CNTFRQ note: the 1 GHz is machine-type-tied (virt-9.0+); virt-8.2 stays 62.5
   MHz. Verified from the dumped DTB (below). Oracle pins CNTFRQ==62500000.

## Hardware facts verified from the QEMU virt-8.2 DTB (dumpdtb, 2026-11-05)

- GICv2 (arm,cortex-a15-gic): GICD reg [0x08000000, 0x10000], GICC [0x08010000,
  0x10000] — matches the pinned addresses.
- Timer (arm,armv8-timer) interrupts: CNTP = PPI 14 -> INTID 30 (level, flag
  0x104); secure-phys PPI13->29; virt PPI11->27; hyp PPI10->26. No
  clock-frequency property -> read CNTFRQ_EL0 at runtime.

## Owner escalations (all approved 2026-11-03/05)

Editing the frozen vectors.S for the slot-5 returning path; the transient-guard
semantic change to je_arm_in_handler (tested double-fault path); the ADR 0010 D6
"PIT"->"timer" reword; the pinned-GIC-address support-tier assumption.

Nature: AI-assisted design review; NOT independent human qualification.
