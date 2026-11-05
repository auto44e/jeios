I reviewed this against the summary you gave plus the merged `vectors.S` and `trapframe.h`. I did not read the ADR 0012 file itself, and nothing was built or run.

**Verdict:** the overall approach is sound. As specified, though, the proof could pass while the property is broken. There are five design defects:

1. **D7 doesn't prove registers are restored.** A heartbeat that keeps advancing is still consistent with corrupted x16–x18, a leaked stack frame, or a handler that never returns but branches back into the loop. Fix: a register-canary loop and an SP-equality check (Q5).
2. **"SPSR.I clear" doesn't prove the exception was async.** A sync fault taken from unmasked code also has SPSR.I = 0. Fix: use the IRQ-only checks in Q5.
3. **GIC group, AckCtl and FIQEn are unspecified.** A group-1 INTID makes the IAR read return 1022. FIQEn=1 routes the timer to the fatal slot 6.
4. **The timer is re-armed without an `isb` before EOIR.** The level-triggered line can re-pend, giving a near-zero-delta second tick.
5. **Spurious handling only checks `== 1023`.** It should be `intid >= 1020`, and the spurious branch has no test.

## 1. IRQ-return asm

- **Don't use TPIDR_EL1.**
  - It holds one register and you need three.
  - It is the natural per-CPU pointer for SMP later, so spilling into it now creates a hidden conflict.
- **Don't reuse the `mov w16,#idx` stub for slot 5.** It destroys x16 before anything has saved it, which is exactly the frozen-doc hazard.
- **Slot 5 needs its own stub.** Each slot has 0x80 bytes, room for 32 instructions. The IRQ is taken on SP_EL1, which is valid, so spill straight to the stack:
  ```
  VEC_IRQ:  .balign 0x80
      sub  sp, sp, #JE_TF_SIZE
      stp  x16, x17, [sp, #(JE_TF_GPR+128)]
      stp  x18, x19, [sp, #(JE_TF_GPR+144)]
      b    je_arm_irq_common
  ```
- **`je_arm_irq_common`** then:
  - saves the remaining GPRs to the frozen offsets;
  - stores `tf->sp = sp + JE_TF_SIZE`, plus ELR and SPSR;
  - stores `vector = 5` from an immediate, not from gpr[16];
  - writes ESR/FAR as stale, or not at all. Document them as UNKNOWN for IRQ frames and never classify on them.
  - calls `je_arm_irq(const je_arm_trapframe *tf)`, then restores ELR/SPSR from the frame, then x0–x30, then `add sp, sp, #JE_TF_SIZE`, then `eret`.
- **Two hazards to pin:**
  - **Make the handler argument `const`** so the compiler enforces that C can't rewrite SPSR.M or ELR before the `eret`.
  - **Never `ldr sp` from `tf->sp`**: unwind with `add`. Otherwise a C bug could move the stack.
- **SPSel:** taking the IRQ from EL1h uses SP_EL1 (group 1). `eret` restores PSTATE.SP from SPSR.M[0]. Assert SPSR.M == 0b0101 at entry. Any future EL1t code would land in slot 1, which is fatal; that is correct and should be documented.
- **Reentrancy:** hardware sets PSTATE.I on entry and the handler never clears it, so IRQs can't nest. Assert DAIF.I == 1 inside the handler.
- **SP alignment:**
  - AArch64 has no red zone, so the `sub` is safe.
  - Add the invariant "asm keeps SP 16-aligned whenever I is unmasked". Otherwise the stub's `stp` takes an alignment fault and it gets reported as a slot-4 first fault.
- **FP/SIMD:** the handler is general-regs-only, so V registers are untouched and saving them isn't needed.

## 2. GIC/timer register sequence (GICv2, secure=off)

**Map exactly one 4 KiB Device-nGnRnE, PXN|UXN page for each of GICD and GICC**, not 64 KiB.
- GICD registers all sit below 0x1000.
- GICC CTLR, PMR, IAR, EOIR and IIDR also sit below 0x1000.
- Leave GICC_DIR (+0x1000) unmapped, so any accidental EOImode=1 use faults loudly.
- The oracle adds an AT/PAR probe expecting a translation fault at GICC+0x1000.

**Sequence:**

1. Quiesce the timer: `CNTP_CTL_EL0 = IMASK(bit1)`, then `isb`.
2. Distributor: `GICD_CTLR = 0`.
   - Read GICD_TYPER and **refuse if SecurityExtn (bit10) = 1**: that changes the group/IAR semantics.
   - Check that GICD_IIDR/GICC_IIDR are nonzero with ARM implementer 0x43B. A wrong base reads 0, so refuse.
3. **Group:** read back GICD_IGROUPR0 bit30 and require 0 (group 0). Fail closed otherwise. A group-1 interrupt with AckCtl=0 makes GICC_IAR return 1022 in this view.
4. **Priority:** byte-write GICD_IPRIORITYR at 0x41E, for example 0x80, then read it back. Unimplemented low bits read as zero.
5. Don't write GICD_ICFGR or GICD_ITARGETSR for PPIs. They are fixed or banked read-only, and the timer is level-sensitive.
6. **Enable:** `GICD_ISENABLER0 = 1u<<30`, then read back bit30. Yes, ISENABLER0 is banked per CPU and works for PPIs.
7. `GICD_CTLR = 1` (EnableGrp0).
8. **PMR:** `GICC_PMR = 0xFF`, then read it back. An interrupt is signalled only if priority is **strictly numerically less than** PMR. Priority 0xFF with PMR 0xFF never fires; that's the classic silent mutation.
9. **`GICC_CTLR = 1` as a plain write, not read-modify-write:**
   - EnableGrp0 = 1.
   - **FIQEn (bit3) = 0**, or group 0 is delivered as FIQ to the fatal slot 6.
   - EOImode = 0.
10. Arm the timer: **write TVAL first, then `CTL = ENABLE`**, then `isb`. Enabling first with a stale CVAL fires immediately and skews the first delta.
11. `dsb sy`, then `msr daifclr, #2` (I only; never #0xf), then `isb`.

**INTID 30:** that is the non-secure EL1 physical timer, PPI 14. Confirm it with three independent checks:
- the oracle derives it from `qemu ... -machine dumpdtb` (the timer node's second `interrupts` entry), not from a hard-coded value;
- the kernel checks `IAR[9:0] == 30`;
- the kernel checks `CNTP_CTL.ISTATUS == 1` at entry.

Also have the oracle check the DTB `intc` `reg` values against the pinned 0x08000000/0x08010000.

**EL check:** assert CurrentEL == 1. With virtualization=on, CNTHCTL_EL2 would be in play, so refuse in that case.

## 3. ACK/EOI ordering and spurious

**Order:**
1. Read IAR **exactly once** and keep the full value: CPUID [12:10] plus INTID [9:0]. A second read acknowledges again or returns 1023.
2. If INTID ≥ 1020, it is special/spurious: no EOIR, count it, return.
3. Otherwise sample CNTPCT, then check ISTATUS.
4. **Re-arm TVAL, then `isb`.** This makes the deasserted timer output visible before the GIC re-samples the level line.
5. Bump the tick counter.
6. `GICC_EOIR = iar`.
7. Return, then `eret`.

**Why this order is correct:** with EOImode=0, the EOIR write both drops priority and deactivates; no DIR write is needed. Make GICC_CTLR's EOImode bit an explicit 0 and verify it by readback.

**Unknown INTID** (not 30 and below 1020): EOIR it anyway, because a stuck active interrupt blocks everything at that priority and below, then fail closed with a fatal diagnostic. Only INTID 30 is enabled, so reaching this is an invariant break.

**No EOIR for spurious is correct.** In GICv2 an EOIR of 1023 is ignored, so the mutation "EOIR on spurious" is harmless and can't be observed. That's acceptable. The mutation that matters is **"spurious counted as a tick"**; see the test in Q5.

## 4. 100 Hz derivation

| CNTFRQ | interval | fits in TVAL (signed 32-bit)? |
|---|---|---|
| 62,500,000 | 625,000 (exact) | yes |
| 1,000,000,000 | 10,000,000 | yes; limit is 2,147,483,647 |
| 0xFFFFFFFF (max) | 42,949,672 | yes |

- **Mask CNTFRQ to [31:0]** (the upper bits are RES0).
- **Refuse if CNTFRQ is 0 or below 100.** That gives interval 0, which arms TVAL=0 and causes an interrupt storm. Real hardware depends on firmware having set CNTFRQ.
- **Refuse if interval > INT32_MAX.** TVAL is sign-extended into CVAL, so an oversized value fires immediately. With a 32-bit CNTFRQ this can't happen, but the check is cheap.
- **Truncation:** the error is under one count and doesn't matter.
- **Correct the ADR's frequency note.** I believe the 1 GHz default is tied to the *machine type* (virt-9.0+), not the QEMU binary version, so virt-8.2 should stay at 62.5 MHz even on QEMU 9+. Please verify. If it's right, the oracle can pin `CNTFRQ == 62500000` and `interval == 625000` as an independent check. Having the oracle recompute from the kernel-printed CNTFRQ is circular.
- **Prefer TVAL re-arm over CVAL += interval.** CVAL catch-up after a late tick produces deltas below the interval, which breaks the lower-bound oracle.
- **Proof of the `delta ≥ interval` bound:** it holds only if CNTPCT is sampled **before** the re-arm (t_{i+1} ≥ r_i + interval ≥ t_i + interval). Sampling after the re-arm can legitimately fail it, so specify the order.

## 5. False-success and mutation (the core)

| Invariant | Mutation that passes a naive test | Defeating observation |
|---|---|---|
| Handler **returned via `eret`** | `br ELR`/`ret` instead of `eret` | PSTATE.I stays masked after `br`, so the second tick never comes. Require **N ≥ 3 ticks** with a heartbeat increase between each. |
| Interrupted context **restored** | x16–x18 not restored (the frozen hazard); an off-by-8 restore; frame leaked each tick; handler loops back into main | A **register-canary asm spin loop** (below); every tick's `tf->sp` must equal tick 1's; every tick's ELR must fall inside the canary-loop symbol range, checked by the oracle from the ELF. |
| IRQ was **async**, not a misreported sync fault | swapped slot-4/5 stubs; a BRK or abort routed to `je_arm_irq`; ESR used to classify | Require all of: vector == 5 from the slot-5 stub; **ISR_EL1.I (bit7) == 1** at entry; IAR INTID == 30 (a sync fault can't make the GIC ack 30); ISTATUS == 1; SPSR.M == EL1h. Don't treat SPSR.I clear as async proof; it only shows the interrupted context was unmasked. |
| Timer **actually firing and re-armed** | no re-arm, so the level line storms | CNTPCT delta ≥ 625,000 between ticks; the storm gives tiny deltas. |
| **EOI happens** | EOIR omitted; EOIR with the wrong value; EOImode=1 | The interrupt stays active, so tick 2 never arrives (covered by N ≥ 3). |
| **Spurious handled** | 1023 counted as a tick | With I masked and nothing pending, call the dispatch function directly. It must see IAR = 1023, leave ticks unchanged, increment the spurious counter, and leave GICC_RPR at idle (0xFF) before and after. |
| **Priority/PMR correct** | priority ≥ PMR | No ticks; the test must emit an explicit FAIL, not hang. |
| **Bounded wait** | a hang counted as a smoke timeout | The main loop holds a CNTPCT deadline (for example 50 intervals) and prints `TIMER FAIL no-tick` when it passes. |

**Canary loop (the restore proof).** A hand-written asm loop loads distinct patterns into x0–x30, deliberately including x16, x17, x18 and x30. It runs with I unmasked, without `wfi`, so interrupts land at varied PCs. Each iteration it:
- increments a heartbeat in memory;
- compares every register against its pattern;
- branches to a FAIL marker on any mismatch.

C code can't do this, because the compiler won't reliably keep values in x16–x18.

**No printing in the handler.**
- Interleaved PL011 output would tear the complete-line markers the oracle depends on.
- "A marker printed from inside the handler" proves only entry, not return.
- Instead, the handler writes per-tick evidence into a static array: vector, IAR, ISR, SPSR, ELR, SP, CNTPCT, heartbeat snapshot.
- After N ticks the loop exits, masks I, and main prints the evidence.
- The oracle checks heartbeat[i+1] > heartbeat[i].

## 6. Frozen-contract safety

- **Use a wholly separate entry, not `je_arm_vec_common`.** Slots 4, 6, 7 and the others keep the identical stub and the shared saver, and the double-fault asm stays byte-identical. The trapframe offsets are unchanged.
- **The meaning of gpr[16..18] becomes vector-dependent:** the real values in IRQ frames, the MP1 deviation in fatal frames. The `trapframe.h` comment must say so.
- **Semantic change:** `je_arm_in_handler` no longer covers every entry. A sync fault inside `je_arm_irq` will be reported as a *first* fault with vector 4, not as a double fault. That's acceptable, but it must be documented.
- **Add a negative control to the test build.** After the N ticks, execute `brk`. Expect the existing MP1 fatal marker with vector 4, EC 0x3C, and **no** double-fault marker.
  - This shows the fatal path survives a running IRQ path.
  - It kills the mutation "IRQ path latches `je_arm_in_handler`".
- **FIQ and SError:** keep A and F masked.

## 7. Scope: split it

The provable seam is **IRQs stay masked in the first PR.**

- **PR-2a: GIC and timer, polled.** No vector change.
  - Map the two 4K Device pages, update the MMU oracle, do the ID readback and refusals, do the init in Q2, arm the timer.
  - With DAIF.I masked, poll and observe:
    - ISR_EL1.I == 1;
    - GICD_ISPENDR0 bit30 set;
    - GICC_HPPIR == 30;
    - then acknowledge with an IAR read (== 30), re-arm, EOIR, repeated N times with delta ≥ interval.
  - Also run the spurious test.
  - Together these prove the whole GIC, timer and ACK/EOI chain without touching frozen asm.
- **PR-2b: returning IRQ path.** The slot-5 stub, the returning saver and `eret`, the canary loop, unmasking, the async proofs, and the BRK negative control.
  - The diff touches the frozen vectors design only. If 2b fails, 2a's verified evidence localises the fault to the asm.

## Owner escalation needed

1. **The `vectors.S` slot-5 stub and new saver.** These change the frozen MP1 vectors design (mp1-vectors debate), even though the offsets stay frozen. The `trapframe.h` deviation comment also needs amending.
2. **`je_arm_in_handler` no longer guards every entry.** This changes the double-fault semantics of tested code.
3. **The ADR 0010 D6 reword** ("100 Hz PIT ticks" to "100 Hz timer ticks"). It edits an accepted ADR, so the owner decides, not an agent majority.
4. **Correct the ADR 0012 D3 CNTFRQ note** to the machine-type wording. This is a factual fix, not a vote, once the QEMU behaviour is verified.

This is AI-assisted review only, not independent human qualification.
