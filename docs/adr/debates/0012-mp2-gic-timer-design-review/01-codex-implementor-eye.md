Verdict: ADR 0012 is close, but not ready as written. The returning path must be a distinct slot-5 entry, GIC group/security setup must be explicit, and the proof needs stronger mutation resistance.

1. IRQ-return asm

No: spilling only x16–x18 through one `TPIDR_EL1` is insufficient. One 64-bit sysreg cannot hold three registers, and overwriting `TPIDR_EL1` also corrupts any existing kernel use unless it is explicitly reserved and restored.

Recommended slot 5:

```asm
/* slot 5: no x16 clobber */
b       je_arm_irq_entry
```

Do not use the existing:

```asm
mov w16, #5
b   je_arm_vec_common
```

because it destroys interrupted x16.

`je_arm_irq_entry` should:

```asm
sub sp, sp, #304
stp x0,  x1,  [sp, #0]
...
stp x28, x29, [sp, #224]
str x30,      [sp, #240]

add x0, sp, #304
str x0, [sp, #248]

mrs x0, ELR_EL1
mrs x1, SPSR_EL1
stp x0, x1, [sp, #256]

mrs x0, ESR_EL1
mrs x1, FAR_EL1
stp x0, x1, [sp, #272]

mov x0, #5
str x0, [sp, #288]
str xzr, [sp, #296]

/* transient handler guard, after GPR preservation */
...
mov x0, sp
bl  je_arm_irq
```

Then restore `ELR_EL1`, `SPSR_EL1`, the saved SP, all GPRs, clear the transient guard, and execute `eret`.

This is async-reentrant-safe because hardware sets PSTATE.I on entry and the handler never unmasks it. It is not safe against a fault during the initial frame stores; the design must explicitly require a valid, writable, 16-byte-aligned EL1 stack, or add a genuinely stackless precheck/dedicated exception stack.

For return, validate `SPSR_EL1.M[3:0] == 0x5` (EL1h), restore the saved SP before `eret`, and let `eret` restore SPSel from SPSR. Do not manually switch SPSel in the epilogue. `M=0x5` selects SP_EL1; `M=0x4` would select SP_EL0. ([developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/spsr_el1))

2. GIC/timer sequence

The ADR omits two required configurations: interrupt group and trigger mode.

For the single-core, secure-off, GICv2 profile, use this sequence while DAIF.I remains set:

```text
GICD_CTLR                  = 0
GICC_CTLR                  = 0

GICD_IGROUPR0[30]         = 1          // Group 1
GICD_ICFGR1[29]            = 0          // level-sensitive PPI
GICD_IPRIORITYR7[23:16]    = 0x80       // INTID 30 priority
GICD_ISENABLER0            = 0x40000000 // bit 30

GICC_PMR                   = 0xff       // allows priority 0x80

GICD_CTLR                  = 0x2        // Enable Group 1, if DS=1
GICC_CTLR                  = 0x2        // Enable Group 1, if DS=1
dsb sy
isb
```

Addresses:

```text
GICD_CTLR           0x08000000 + 0x000
GICD_IGROUPR0       0x08000000 + 0x080
GICD_ISENABLER0     0x08000000 + 0x100
GICD_IPRIORITYR7    0x08000000 + 0x41c
GICD_ICFGR1         0x08000000 + 0xc04

GICC_CTLR           0x08010000 + 0x000
GICC_PMR            0x08010000 + 0x004
GICC_IAR            0x08010000 + 0x00c
GICC_EOIR           0x08010000 + 0x010
```

PPI 30 is correct for the non-secure EL1 physical timer on this profile. PPIs 16–31 are private/banked, and `GICD_ISENABLER0` bit 30 is the correct enable bit. ([developer.arm.com](https://developer.arm.com/docs/den0024/a/aarch64-exception-handling/the-generic-interrupt-controller), [developer.arm.com](https://developer.arm.com/docs/ddi0438/c/generic-interrupt-controller/gic-programmers-model/distributor-register-summary))

Priority is numerically inverted: `0x80` is higher priority than `0xff`, so PMR `0xff` permits it. The condition is strictly “interrupt priority higher than PMR.” ([developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch32-system-registers/icc_pmr))

Do not blindly assume `GICC_CTLR=0x2`; first read `GICD_CTLR.DS`. If security extensions were present (`DS=0`), the non-secure CPU-interface Group-1 enable is a different bit layout. For this exact `secure=off` target, require readback consistent with `DS=1`; otherwise fail closed.

`GICD_TYPER/IIDR` readback proves presence/identity, not by itself “GICv2.” The QEMU command-line profile supplies the version; the runtime checks should verify the legacy GICC layout and expected fields.

CNTP:

```asm
msr CNTP_CTL_EL0, xzr       // disable while programming
msr CNTP_TVAL_EL0, interval
mov x0, #1                  // ENABLE=1, IMASK=0
msr CNTP_CTL_EL0, x0
isb
```

On each timer IRQ, verify `ENABLE=1`, `IMASK=0`, and `ISTATUS=1`; write `CNTP_TVAL_EL0` again, `isb`, increment the counter, then EOI. `CNTP_TVAL_EL0` programs `CNTP_CVAL_EL0 = CNTPCT_EL0 + TimerValue`, and the value is interpreted as signed 32-bit. ([developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/cntp_tval_el0))

3. ACK/EOI and spurious

Handle-then-EOIR is correct:

```c
uint32_t iar = mmio_read32(GICC_IAR);
uint32_t intid = iar & 0x3ffu;

if (intid == 1023u) {
    ++spurious_count;
    return;                 /* absolutely no EOIR */
}

if (intid != 30u)
    fatal_unexpected_interrupt(intid);

verify_cntp_istatus();
rearm_cntp();
++je_timer_ticks;
mmio_write32(GICC_EOIR, iar);  /* exact IAR, not just intid */
```

The exact IAR matters because SGI acknowledgements may contain CPU-interface information. EOIR with EOImode clear performs both priority drop and deactivation; if EOImode is set, EOIR only drops priority and a separate deactivation operation is required. Require `GICC_CTLR.EOImode == 0`. ([developer.arm.com](https://developer.arm.com/docs/ddi0595/d/external-system-registers/gicc_ctlr), [developer.arm.com](https://developer.arm.com/docs/ddi0471/b/b-interrupt-signaling/b1-interrupt-signaling-in-the-gic-400-with-physical-interrupts-only))

Spurious 1023 is not an acknowledged interrupt, so no EOIR is correct. Treat 1022 as unexpected/fatal for this secure-off profile rather than silently treating it as ordinary spurious.

4. 100 Hz derivation

For the target frequencies:

```text
62,500,000 / 100 = 625,000
1,000,000,000 / 100 = 10,000,000
```

Both are valid positive signed-32-bit TVAL values.

Required checks:

```c
if (freq < 100)
    fatal();

interval = freq / 100;       // or explicitly rounded
if (interval == 0 || interval > 0x7fffffff)
    fatal();
```

The ADR’s “not divisible” failure condition is too strict. Divisibility is not required; truncation introduces less than one counter tick of per-period error. Either document truncation, use `(freq + 50) / 100` with overflow checking, or add a phase accumulator for long-term exactness.

The actual overflow limit is:

```text
CNTFRQ_EL0 / 100 <= 0x7fffffff
CNTFRQ_EL0 <= 214,748,364,700 Hz
```

The two QEMU values are nowhere near that limit. Since TVAL is signed, reject `0x80000000` and above; those values represent negative delays and can fire immediately. ([developer.arm.com](https://developer.arm.com/docs/ddi0595/e/aarch64-system-registers/cntp_tval_el0))

5. False-success and mutation resistance

- IRQ “taken and returned”: omit `eret` or print only inside `je_arm_irq`; a naive test still passes. Defeat this with a mainline heartbeat incremented after `wfi`, plus an assembly sentinel test that seeds x16/x17/x18 and verifies them after return.

- Timer actually firing: directly call the C handler or increment the tick in mainline. Defeat this with all of:
  - vector index exactly 5;
  - saved `SPSR.I == 0`;
  - IAR INTID exactly 30;
  - `CNTP_CTL_EL0.ISTATUS == 1` at entry;
  - `CNTPCT_EL0` sampled from the system register with successive delta ≥ interval;
  - mainline execution resumes between ticks.

- Async versus synchronous fault: `SPSR.I == 0` alone is insufficient; a synchronous fault can also have I clear. The decisive combination is vector 5 + IAR 30 + execution after `wfi`. A synchronous fault must enter slot 4. Keep `je_arm_irq` private to the slot-5 assembly path so C cannot fabricate the frame.

- EOI actually happening: omit EOIR and a one-tick test may pass. Require multiple timer firings. With EOImode 0, the timer cannot repeatedly re-enter after re-arm while still active unless the previous EOIR occurred. Also check post-return `GICC_RPR`/`GICC_HPPIR` where useful.

- Spurious handling: a serial marker cannot prove that no MMIO EOIR write happened. Add a GIC MMIO wrapper with an EOIR-write counter and a hosted fake-GIC test: reading 1023 must increment the spurious counter while leaving the EOIR-write count unchanged. Then inject a real timer interrupt and prove normal ACK/EOI still works.

- x16–x18 restore: seed distinctive values immediately before `wfi`; compare immediately after `wfi`. This is mandatory. A heartbeat alone can miss exactly the frozen MP1 defect.

6. Frozen-contract safety

Yes, editing the saver risks the tested fatal path. The safe shape is:

- leave `je_arm_vec_common`, `je_arm_double_fault`, and slots 0–4 and 6–15 unchanged;
- make slot 5 a separate branch to `je_arm_irq_entry`;
- keep all trap-frame offsets and size exactly unchanged;
- write vector `5` into `tf->vector` after the full GPR save;
- use a transient handler guard so a synchronous fault inside `je_arm_irq` enters the fatal double-fault path;
- clear that guard only on the successful return path.

“Do not latch `je_arm_in_handler`” should mean “do not leave it permanently set,” not “never set any guard.” Otherwise a synchronous fault inside the IRQ handler enters the fatal common path as if no handler were active.

This touches frozen, previously tested assembly and therefore needs owner escalation. The trap-frame ABI itself must not be changed. The ADR 0010 CLOCK_TICKS reword also needs the owner-gated documentation update.

7. Scope

I would split the implementation into two PRs:

1. GIC support-tier bring-up:
   - map `[0x08000000, 0x08020000)`;
   - update the MMU oracle;
   - identify/read back GICD/GICC;
   - configure Group 1, level-sensitive PPI, priority, PMR;
   - keep DAIF masked and do not alter vectors.

2. CNTP plus returning IRQ:
   - slot-5 saver/restore/`eret`;
   - timer arm/re-arm;
   - IRQ unmask + `wfi`;
   - multi-tick return proof and negative spurious/EOI tests.

The provable seam is after PR-1: GIC MMIO read/write and configuration readbacks pass while no interrupt can be delivered. The second PR then consumes that fixed GIC contract.

There is also an ADR contradiction to resolve before implementation: D1 says PR-2 unmasks IRQs and uses WFI, while D7 says the default build remains IRQ-silent until PR-3. I recommend default builds configure but do not arm/unmask the timer; only `ARM_TIMER_TEST` enables DAIF.I clearing and WFI.
