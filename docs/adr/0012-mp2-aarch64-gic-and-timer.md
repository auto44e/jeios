# ADR 0012: MP2 aarch64 GICv2, architected timer, and the returning IRQ path

Status: **accepted** (2026-11-05). Owner-approved scope + four decisions, then a
two-lineage design review (transcripts under
`docs/adr/debates/0012-mp2-gic-timer-design-review/`) that split the work and
corrected the register-level sequence. Target: QEMU `virt-8.2` / `cortex-a72`,
GICv2, EL1, MMU on (identity TTBR0, ADR 0011), `virtualization=off`,
`secure=off`. Scheduling / context switching is a LATER PR; this ADR adds NO
scheduler, NO second thread, NO EL0.

Register values are pinned against Arm DDI0487, the GICv2 spec (IHI0048), the
cortex-a72 TRM, and the actual QEMU `virt-8.2` DTB (dumped 2026-11-05: GICD
0x08000000/0x10000, GICC 0x08010000/0x10000; timer CNTP = PPI 14 → **INTID 30**,
level-sensitive; no `clock-frequency` property, so CNTFRQ_EL0 is read at runtime).

## Decision 0 — split into two stacked PRs (D-SPLIT, from the design review)

Both review lineages independently recommended splitting at the seam **"IRQs
stay masked"**, because it isolates the frozen-asm change from the device
bring-up:

- **PR-2a — GIC + timer, POLLED, IRQs masked, NO vector change.** Map the GIC,
  identify it, configure the one PPI and the CPU interface, arm the timer, and
  PROVE the whole GIC/timer/ACK/EOI chain by POLLING with DAIF.I still set
  (`ISR_EL1.I==1`, `GICD_ISPENDR0` bit 30, `GICC_HPPIR==30`, then `GICC_IAR`
  read == 30, re-arm, `GICC_EOIR`, repeated N times with CNTPCT delta ≥
  interval), plus the spurious test. Touches no frozen asm.
- **PR-2b — the returning IRQ path.** The dedicated slot-5 stub + returning
  saver + `eret`, IRQ unmask (`msr daifclr, #2`), the register-canary proof, the
  async proofs, and a BRK negative control. The diff touches the frozen vectors
  design only; if it fails, PR-2a's evidence localizes the fault to the asm.

## Decision 1 — scope and order (D1)
PR-2a: GICv2 bring-up + CNTP timer, polled, provable with IRQs masked. PR-2b: the
returning EL1h-IRQ path, unmasking, and the timer-fires-and-returns proof. The
**default build configures the GIC + timer but does NOT unmask IRQs**; only the
`ARM_TIMER_TEST` build clears DAIF.I. (This resolves the D1/D7 contradiction the
review found: no silent default-boot IRQ behavior until the scheduler PR needs
the tick.)

## Decision 2 — GIC discovery: pinned addresses + readback + refusals (D2)
GICD = **0x08000000**, GICC = **0x08010000**, each mapped as **one 4 KiB
Device-nGnRnE PXN|UXN page** (all needed registers live below 0x1000; leave
GICC_DIR at +0x1000 UNMAPPED so an accidental EOImode path faults loudly — the
MMU oracle adds an `AT`/`PAR` translation-fault probe there). Both pages are
added to the W^X map; the smoke MMU leaf/audit oracle is updated for the two
extra Device leaves. Identify + fail closed:
- `GICD_TYPER.SecurityExtn` (bit 10) must be 0 (`secure=off`); a 1 changes group
  / IAR semantics → refuse.
- `GICD_IIDR` / `GICC_IIDR` implementer must be ARM (0x43B) and nonzero (a wrong
  base reads 0) → refuse.
**Flex (recorded, owner):** parse the DTB `/intc` + `/timer` nodes in the shared
FDT parser when a second ARM target appears (the oracle already cross-checks the
pinned bases + PPI against `-machine dumpdtb`).

## Decision 3 — timer: CNTP at 100 Hz from CNTFRQ_EL0 (D3)
EL1 physical timer (**CNTP**; `virtualization=off`, so CNTP is clean at EL1; a
`CurrentEL != 1` or a virtualization-on configuration refuses). Read
**`CNTFRQ_EL0`** at runtime, mask to [31:0]. `interval = CNTFRQ_EL0 / 100`
(truncation error < 1 count, accepted). **Refuse if CNTFRQ_EL0 < 100** (interval
0 → interrupt storm) **or interval > INT32_MAX** (TVAL is a signed 32-bit
down-counter; an oversized/negative value fires immediately). virt-8.2 CNTFRQ is
62.5 MHz → interval 625000 (both fit). The timer PPI is **INTID 30** (CNTP, PPI
14), confirmed from the GICC IAR, never assumed blindly. Arm: `CNTP_CTL_EL0 =
IMASK` (quiesce) → `isb` → `CNTP_TVAL_EL0 = interval` → `CNTP_CTL_EL0 =
ENABLE(1),IMASK(0)` → `isb` (TVAL BEFORE enable, else a stale CVAL fires
immediately). **Flex:** CNTV instead of CNTP (needs EL2/CNTHCTL — not this
machine).

## Decision 4 — GICv2 init sequence for one PPI (D4, pinned; review-corrected)
With DAIF.I still masked:
1. `CNTP_CTL_EL0 = IMASK; isb` (quiesce the source).
2. `GICD_CTLR = 0`. Read `GICD_TYPER` (refuse SecurityExtn=1) and the IIDRs
   (refuse implausible).
3. **Group:** this profile is single-security group 0. Read back `GICD_IGROUPR0`
   bit 30 and require **0** (group 0) — a group-1 INTID with AckCtl=0 makes
   `GICC_IAR` return 1022, a silent mis-ack.
4. **Priority:** byte-write `GICD_IPRIORITYR` for INTID 30 (offset 0x400+30 =
   0x41E) = 0x80, read back (unimplemented low bits read 0).
5. Do NOT write `GICD_ICFGR`/`ITARGETSR` for the PPI (banked/fixed; the timer is
   level-sensitive per the DTB flags).
6. **Enable:** `GICD_ISENABLER0 = (1u<<30)`, read back bit 30 (ISENABLER0 is
   banked per-CPU and is correct for PPIs 16–31).
7. `GICD_CTLR = 1` (EnableGrp0).
8. **PMR:** `GICC_PMR = 0xFF`, read back. An IRQ signals only if its priority is
   **strictly numerically less than** PMR — 0x80 < 0xFF passes; priority 0xFF
   with PMR 0xFF would NEVER fire (the classic silent mutation).
9. `GICC_CTLR = 1` as a plain write: **EnableGrp0=1, FIQEn(bit3)=0** (else group
   0 is delivered as FIQ to the fatal slot 6!), **EOImode=0** (EOIR both drops
   priority and deactivates; no DIR write). Verify by readback.
10. Arm the timer (D3).
Addresses used (all < 0x1000 from each base): GICD_CTLR 0x000, GICD_IGROUPR0
0x080, GICD_ISENABLER0 0x100, GICD_IPRIORITYR 0x400, GICD_ICFGR1 0xC04,
GICD_TYPER 0x004, GICD_IIDR 0x008; GICC_CTLR 0x000, GICC_PMR 0x004, GICC_IAR
0x00C, GICC_EOIR 0x010, GICC_HPPIR 0x018, GICC_IIDR 0x0FC.

## Decision 5 — the returning IRQ path (D5, owner: edit the frozen vectors)
A DEDICATED slot-5 stub (NOT the shared `mov w16,#idx; b je_arm_vec_common`
stub, which clobbers the interrupted x16 before it is saved). The IRQ is taken
on a valid SP_EL1, so the stub spills straight to the frame:
```
VEC_IRQ (slot 5):  .balign 0x80
    sub  sp, sp, #JE_TF_SIZE
    stp  x16, x17, [sp, #(JE_TF_GPR+128)]   /* real interrupted x16,x17 */
    stp  x18, x19, [sp, #(JE_TF_GPR+144)]   /* real x18 (+x19) */
    b    je_arm_irq_common
```
`je_arm_irq_common` saves the remaining GPRs to the frozen offsets, stores
`tf->sp = sp + JE_TF_SIZE` (an IRQ from EL1h is on SP_EL1), ELR/SPSR, writes
`vector = 5` from an immediate (not gpr[16]); ESR/FAR are UNKNOWN for an IRQ and
are never used to classify. It calls `je_arm_irq(const je_arm_trapframe *tf)`
(the `const` makes the compiler forbid C rewriting SPSR.M/ELR before `eret`),
then restores ELR/SPSR, x0..x30, unwinds the stack with **`add sp, sp,
#JE_TF_SIZE`** (never `ldr sp` from the frame), and `eret`. `eret` restores
PSTATE.SP from SPSR.M; the handler asserts `SPSR.M == 0b0101` (EL1h) at entry and
does NOT manually touch SPSel. The sync path (slot 4), FIQ (6), SError (7) and
all other slots keep their byte-identical frozen stub + `je_arm_vec_common` +
`je_arm_double_fault`. Trap-frame OFFSETS stay frozen; `trapframe.h`'s deviation
comment is amended to say gpr[16..18] hold the REAL interrupted values in IRQ
frames (and the MP1 deviation only in fatal frames).

**Reentrancy guard semantics change (owner-gated, tested path):**
`je_arm_in_handler` no longer covers every entry. The IRQ path sets a TRANSIENT
guard after the GPR save and CLEARS it on the successful return — so a
synchronous fault INSIDE `je_arm_irq` still takes the fatal double-fault path.
IRQs cannot nest (HW masks DAIF.I on entry; the handler never unmasks; the
handler asserts `DAIF.I==1`). A BRK negative control in the test build proves the
fatal path still fires (vector 4, EC 0x3C, no double-fault marker) with a running
IRQ path.

## Decision 6 — ACK / EOI / spurious (D6, pinned to GICv2)
Per IRQ: read `GICC_IAR` **exactly once**, keep the full value (CPUID[12:10] +
INTID[9:0]). If **INTID ≥ 1020** (special/spurious, incl. 1023/1022): count it,
write NO EOIR, return. If INTID != 30: EOIR it anyway (a stuck-active interrupt
blocks that priority and below) then FATAL (only 30 is enabled, so this is an
invariant break). Otherwise, in order: sample `CNTPCT_EL0`, confirm
`CNTP_CTL_EL0.ISTATUS==1`, re-arm `CNTP_TVAL_EL0`, `isb` (make the deasserted
level line visible before the GIC re-samples), bump the tick counter, then write
the **exact IAR** to `GICC_EOIR`. CNTPCT is sampled BEFORE the re-arm so the
`delta ≥ interval` lower bound holds (t_{i+1} ≥ r_i + interval ≥ t_i + interval).

## Decision 7 — je_sched_ticks / CLOCK_TICKS wording (D7, owner reword)
A `volatile uint64_t` ARM tick counter, same semantics as x86 `je_timer_ticks`
(monotonic, 100 Hz, never decreases). **ADR 0010 D6 is reworded** from "100 Hz
PIT ticks" to arch-neutral "100 Hz timer ticks (`je_sched_ticks()` on x86; the
architected-timer counter on aarch64)". No ABI value changes; CLOCK_TICKS is an
EL0 syscall (MP3), NOT wired on ARM in this PR.

## Decision 8 — proofs, fail-closed, mutation-resistant (D8)
PR-2a (polled, IRQs masked): all GIC readback refusals above; then poll N≥3
"pending→IAR==30→re-arm→EOIR" cycles asserting `CNTPCT` delta ≥ interval each,
plus a spurious test (a dispatch that sees IAR==1023 must leave ticks unchanged,
bump the spurious counter, and leave `GICC_RPR==0xFF` idle). A bounded CNTPCT
deadline prints `TIMER FAIL no-tick` rather than hanging if nothing pends.

PR-2b (the returning path, `ARM_TIMER_TEST` build): a hand-written **register-
canary asm loop** runs with I UNMASKED and no `wfi`, seeding distinct patterns in
x0..x30 and checking them each iteration. Because a single loop cannot check its
OWN working registers, the proof runs in TWO phases (owner a+b, 2026-11-05):
- **(a)** the primary loop seeds all GPRs but uses x29/x30 as its scratch/LR, so
  it genuinely checks **x0..x28** every iteration AND in a final I-masked pass
  (race-free on the last tick). This covers the frozen-MP1 hazard registers
  x16,x17,x18 — the whole reason the proof exists. (A check of x29/x30 here would
  read a saved-good copy and be a FALSE proof, so it is deliberately omitted.)
- **(b)** a second `je_arm_canary_hi_loop` then seeds x29,x30, uses the
  already-proven x0..x2 as scratch, unmasks I for a few more ticks, and checks
  x29,x30 each iteration + in a final I-masked pass. This closes the x29/x30 gap
  that (a) structurally cannot.
Together (a)+(b) cover all 31 GPRs. Mutation-confirmed: corrupting x16 on the IRQ
return is caught by (a) (`FATAL irq canary mismatch reg=16`); corrupting x30 is
caught by (b) (`FATAL irq canary-hi mismatch reg=30`) — a gap (a) alone misses.
C cannot write this (it won't keep values in x16-x18). The handler does NO
printing (it would tear the complete-line markers); it records per-tick evidence
(vector, IAR, ISR_EL1, SPSR, ELR, SP, CNTPCT, heartbeat) into a static array, and
main prints it after N ticks. Proven, each defeating a named mutation:
- **returned via `eret`** (not `br`/`ret`): N≥3 ticks with the heartbeat
  advancing between each (a `br` leaves I masked → no 2nd tick).
- **context restored**: every tick's `tf->sp` equals tick 1's; every tick's ELR
  lies in the canary-loop symbol range (oracle checks from the ELF); the canary
  registers match after every IRQ.
- **async, not a misreported sync fault**: vector==5 (from the slot-5 stub) AND
  `ISR_EL1.I==1` at entry AND IAR INTID==30 AND `ISTATUS==1` AND SPSR.M==EL1h.
  (SPSR.I clear alone is NOT proof — a sync fault from unmasked code has it too.)
- **timer re-armed** (not storming): CNTPCT delta ≥ interval.
- **EOI happened**: covered by N≥3 (an un-EOI'd active interrupt blocks tick 2).
- **BRK negative control**: after N ticks, a `brk` yields the MP1 fatal marker
  (vector 4, EC 0x3C, no double-fault), proving the fatal path survives.
The oracle pins `CNTFRQ==62500000`/`interval==625000` (machine-type-fixed for
virt-8.2) and cross-checks the GIC bases + PPI 30 from `-machine dumpdtb`, so it
is not circular with the kernel's own prints. A halted emulator with no markers
is failure.

## Why / predictions / flex (ADR 0000 form)
- **Why:** every later ARM gate (scheduling, preemption, drivers) needs a working
  interrupt + timer; the CPU has never taken an async interrupt. Polled-first
  (2a) proves the device chain without risking the frozen asm; the returning
  path (2b) is then a localized, heavily-proven asm change.
- **If other choices were made:** spilling x16-x18 to TPIDR_EL1 is wrong (one
  reg, three values, and it's the SMP per-CPU pointer); FIQEn=1 would route the
  timer to the fatal FIQ slot; PMR==priority would make it silently never fire;
  CVAL+=interval catch-up would break the delta lower bound.
- **Flex (revisit):** DTB GIC/timer discovery (D2); CNTV (D3); default-boot IRQ
  unmask (flips on when the PR-3 scheduler needs the tick); a separate IRQ stack;
  GICv3 as a later tier. SMP/PSCI (needs TLBI/… IS + per-CPU GIC redistributors),
  SVE, power states remain later gates.

## Evidence required
Core commit, toolchain + QEMU versions, the pinned `virt-8.2` invocation, Image
hash, serial log, exact ordered markers. PR-2a: GIC identified + refusals; the
polled N-cycle ACK/EOI chain with CNTPCT deltas; the spurious test; MMU oracle
updated for the two GIC Device leaves. PR-2b: the register-canary restore proof,
the async proofs (vector 5 / ISR.I / IAR 30 / ISTATUS / EL1h), the heartbeat
return proof, and the BRK negative control. MP1 + MMU + allocator markers still
pass; AMD64 gates retained. QEMU TCG models no real interrupt latency or caches —
timing is not a precision claim and cache/barrier maintenance is reviewed, not
observed. AI-assisted review is not independent human qualification.
