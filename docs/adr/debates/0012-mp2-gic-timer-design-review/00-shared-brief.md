# MP2 PR-2 design review — aarch64 GICv2 + architected timer + returning IRQ path

Reviewing a DESIGN (ADR 0012) for JeiOS, a fresh C11 microkernel, BEFORE any code.
Target: QEMU virt-8.2, cortex-a72, GICv2 (gic-version=2), EL1, MMU ON (identity
TTBR0), virtualization=off, secure=off. Find design defects NOW: anything that
will fault silently, hang, ship a security/correctness hole, be impossible to
build as specified, or make a false "it works" claim. Be concrete to the
register/bit/barrier. A design that merely sounds right is not enough.

## Verified ground truth (merged)

- boot.S masks all DAIF and NOTHING unmasks it. No interrupt handling exists.
  vectors.S: 16 slots (4 groups {SP_EL0, SP_ELx(EL1h), lowerEL64, lowerEL32} x
  {Sync,IRQ,FIQ,SError}), each `mov w16,#idx; b je_arm_vec_common`. The common
  saver is NON-RETURNING: it latches je_arm_in_handler (never cleared), stashes
  the vector index + guard ptr in gpr[16..18] (NOT the interrupted x16-x18),
  builds a je_arm_trapframe (FROZEN offsets in trapframe.h: gpr[31], sp@248,
  elr@256, spsr@264, esr@272, far@280, vector@288, size 304), calls
  je_arm_exception(tf), halts. je_arm_double_fault is pure stack-free asm.
- The frozen vectors rationale (docs/adr/debates/mp1-vectors.md) EXPLICITLY says a
  returning IRQ handler must (a) preserve x16-x18 properly and (b) add a
  restore+eret epilogue; MP1 deliberately added neither ("untested restore is
  dead code a later PR would trust").
- MMU is ON: identity TTBR0, W^X kernel map, the ONLY Device-nGnRnE page is PL011
  at 0x09000000. mmu_bringup (main.c) builds the region list; the smoke MMU
  oracle (tools/smoke_arm.py check_mmu) counts leaves + checks per-class bits +
  AT/PAR probes. Adding GIC MMIO = two more Device regions + oracle update.
- Timer counter to mirror (x86): volatile uint64_t je_timer_ticks, 100 Hz PIT,
  je_arch_timer_eoi does out8(0x20,0x20); IRQ0 -> je_sched_interrupt(f,true).
  sched.c is x86-coupled, NOT linked on ARM; scheduling is the NEXT PR, not this.
- Build: -mgeneral-regs-only -mstrict-align -Werror -Wconversion. mk/aarch64.mk
  toggles (ARM_VECTOR_TEST/ARM_DTB_BAD/ARM_MMU_FAULT/ARM_BSS_CHECK) are mutually
  exclusive with separate build dirs. smoke_arm.py: ordered complete-line
  markers, independent ELF-based oracles, terminal-line-must-be-last.

## The design under review (ADR 0012, summarized)

- D2 GIC discovery: PIN GICD=0x08000000, GICC=0x08010000 (each 0x10000) as a
  support-tier assumption like PL011, + runtime readback of GICD_TYPER/IIDR as
  evidence. Map both Device-nGnRnE; update the MMU oracle. (Flex: DTB parse later.)
- D3 timer: CNTP (EL1 physical). Read CNTFRQ_EL0 at runtime (NOT hardcoded;
  virt-8.2 ~62.5MHz, QEMU>=9.0 =1GHz), interval = CNTFRQ/100. Arm CNTP_TVAL_EL0 =
  interval, CNTP_CTL_EL0 = ENABLE=1,IMASK=0. Timer PPI INTID = 30, confirmed from
  GICC IAR not assumed. (Flex: CNTV.)
- D4 returning IRQ path: a SEPARATE path for EL1h IRQ (slot 5); sync (slot 4)
  stays fatal. Preserve real x16-x18 (spill via TPIDR_EL1 or restructure). Do NOT
  latch je_arm_in_handler. IRQs taken with DAIF.I masked by HW, not unmasked in
  handler. Build frame, call returning je_arm_irq(tf), restore x0..x30+SP+ELR/
  SPSR, eret. FIQ/SError stay fatal.
- D5 ACK/EOI/spurious: read GICC_IAR; INTID 1023 spurious -> no EOIR, count it;
  else handle then write exact IAR to GICC_EOIR (no EOImode split). Timer: confirm
  CNTP_CTL.ISTATUS, re-arm CNTP_TVAL, bump tick, THEN EOIR.
- D6 ticks: a volatile uint64_t 100 Hz counter (mirror je_timer_ticks). Reword
  ADR 0010 D6 "100 Hz PIT ticks" -> arch-neutral "100 Hz timer ticks". CLOCK_TICKS
  (EL0 syscall) NOT wired on ARM this PR.
- D7 proof (ARM_TIMER_TEST build; default stays IRQ-silent): unmask IRQ, observe
  N ticks, each asserting vector==5, IAR INTID==timer PPI, SPSR.I clear at entry,
  CNTPCT_EL0 strictly monotonic (delta>=interval, NOT precision). A main-loop
  heartbeat advances between ticks -> proves eret returned + context resumed.
  Oracle recomputes PPI/interval + monotonicity independently.

## Questions to answer directly

1. IRQ-return asm: is spilling x16-x18 via TPIDR_EL1 (or a restructure) correct
   and reentrancy-safe given the CPU masks DAIF.I on entry? What is the MINIMAL
   correct returning-saver + eret that doesn't disturb the frozen fatal path?
   Does the existing index-in-w16 stub scheme survive for slot 5, or must the IRQ
   slot stub differ? Any hazard with SP_ELx/SPSel on return?
2. GIC/timer register sequence: exact GICD/GICC init for ONE PPI (GICD_CTLR,
   GICD_ISENABLER for INTID 30, GICD_IPRIORITYR, GICC_PMR, GICC_CTLR). PPIs are
   banked per-cpu (INTID 16-31) — does enabling via GICD_ISENABLER0 work, and is
   the priority/PMR relationship right (PMR must be > the IRQ priority to let it
   through)? CNTP arm/re-arm + ISTATUS correct? Is INTID 30 right for CNTP on this
   machine, and how is it confirmed from IAR?
3. ACK/EOI ordering + spurious: is handle-then-EOIR correct for GICv2? Any
   deactivation subtlety without EOImode? Is the spurious-1023 no-EOIR right?
4. The 100 Hz derivation: CNTFRQ/100 integer division — rounding/zero hazards?
   Does a ~62.5MHz vs 1GHz CNTFRQ both yield a sane interval? TVAL is a 32-bit
   signed down-counter — can interval overflow it at 1GHz (1e7 fits in 32 bits —
   check)?
5. False-success / mutation: for each claimed invariant (IRQ actually taken &
   returned; timer actually firing; EOI actually happening; spurious handled)
   name the mutation that passes a naive test and the observation that defeats
   it. Especially: how do we PROVE the handler RETURNED (not just "a marker
   printed from inside the handler")? How do we prove the IRQ was async (vector
   5, SPSR.I clear) and not a synchronous fault misreported?
6. Frozen-contract safety: does editing the saver for the IRQ path risk the
   tested fatal/double-fault path? Is the trapframe-offset freeze respected?
   Should the IRQ path share je_arm_vec_common or be a wholly separate entry?
7. Scope: is GIC+timer+IRQ-return the right single PR, or should it split
   (e.g. GIC-identify+map, then timer+IRQ)? Where is the provable seam?

Answer each number. Flag anything needing owner escalation (frozen vectors/
trapframe, the ADR 0010 reword, tested paths). If a decision is wrong, say so and
give the correct one.
