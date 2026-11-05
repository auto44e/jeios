`make smoke-aarch64 ARM_TIMER_TEST=1 ARM_REQUIRE=1` now passes on QEMU 8.2.2, and it rejects every doctored log I tried. But my asm review found two real gaps in the canary proof, and I confirmed both by running deliberately broken copies of the asm. Nothing is committed and I did not edit `vectors.S`.

## What I added
- **`tools/smoke_arm.py --timer-test`**: it can't be combined with any other mode, and the Makefile already passes it through. It checks the timer-test block itself rather than trusting the kernel's own PASS line, in exact order right after the timer line:
  - exactly 4 ticks numbered 1..4;
  - every tick has vector 5, intid 0x1e and `isr == 0x80`;
  - SPSR is EL1h with I clear and D, A, F set;
  - `sp` is the same on every tick, 16-aligned, and inside `[__stack_bottom, __stack_top)`;
  - `cntpct` strictly increases, each step at least 625000;
  - the heartbeat strictly increases;
  - then `PASS ticks=4` and the exact 5-line BRK control: vector 4, EC 0x3c, ESR 0xf200b2b2, ELR equal to `je_arm_irq_brk_site` (whose Image word is checked to be `BRK #0xb2b2`), SPSR with all DAIF bits masked, and `vbar` equal to the installed value.

  Any `double fault` text fails the run, and `irq` lines in any other mode fail it too.
- **`main.c`, print only**: the tick line now prints `elr=`. The handler already recorded ELR but never printed it, so the ELR check had nothing to read. This is a small edit to the implementor's file, so please confirm it.
- **`tools/test_smoke_arm_timer.py`** (new): feeds the saved good serial log through the unmodified smoke script using a fake QEMU. The good log passes, and all 27 doctored logs fail for the expected reason. These include a frozen heartbeat, a vector=4 tick, a changing `sp`, a double-fault line (with and without `FATAL`), an ELR outside the range, a wrong ISR or SPSR, a short `cntpct` step, a wrong BRK ELR, and a line after the terminal.

## ELR range check
`je_arm_canary_loop` has no `.size`, so I take the range from the Image bytes instead. The symbol's first word must be `sub sp,sp,#128`. The valid range runs from the instruction after `msr daifclr,#2` up to and including the first `msr daifset,#2`, which is `[0x40084b40, 0x40084dfc]` in this build. All 4 ticks landed inside it. The failure path comes after that `daifset`, so a tick that interrupted the failure path is rejected.

## Asm review
Confirmed:
- The slot-5 stub saves the real x16–x19 before anything else uses them.
- The vector number is written as an immediate (`mov x1,#5`), not taken from `gpr[16]`.
- The in-handler guard is set after all GPRs are saved and cleared before the restore and `eret`.
- The stack is unwound with `add sp`; nothing loads `sp` from the frame.
- An SPSR mode other than EL1h branches to `je_arm_double_fault`.
- Slots 0–4 and 6–15 are unchanged in the source.

On the x16–x18 hazard specifically, the loop never writes those registers, so they are genuinely owned. Broken copies that cleared x16, x17 or x18 before `eret` each failed with `FATAL irq canary mismatch reg=16`, `17` and `18`.

Two gaps for the lead:

1. **x30 is effectively never checked.** It is the loop's comparison scratch register, so damage from the handler gets overwritten. A broken copy that sets `mov x30,#0` before `eret` still **passes**. x29 and x30 damage is also hidden inside the heartbeat and tick-count save/restore windows. Suggested fix: compare against immediates with no scratch register, e.g. `cmp xN,#(0x800+N),lsl #12` with distinct non-zero values. Only the heartbeat window would stay hidden, and that should be documented.
2. **The 4th tick is only partly checked.** The loop exits as soon as it sees 4 ticks, without a full check pass afterwards. A broken copy that clears x0 only on the 4th return still **passes**. Suggested fix: when the count reaches 4, mask I, reload x29/x30, and run one complete x0..x30 check before exiting.

## Hosted test
I didn't add one. `je_arm_irq` lives in `main.c` alongside inline asm and console code, and there's no hook to swap in a fake `je_gic_handle_one`. Its OK/BUSY/spurious handling is therefore proven only on QEMU, and the BUSY and spurious branches never ran in the live test. Moving `je_arm_irq` into its own `irq.c` would make a hosted test possible.

## Verification (local only; remote CI not run)
- All 11 ARM modes pass with `ARM_REQUIRE=1`: default, TIMER_POLL, MMU_FAULT ×4, VECTOR_TEST, DTB_BAD, BSS_CHECK, PHYSALLOC and TIMER_TEST.
- `make check`, which includes the ASAN/UBSAN tests, passes, as does `git diff --check`.
- QEMU TCG timing is a lower bound only.
- If a 5th tick arrived in the few instructions between the count check and `daifset`, the strict `ticks=4` check would fail the run. That hasn't happened, and it errs on the side of failing.

`docs/HANDOFF.md` has a new timestamped entry with the files, commands, broken-copy results and next task. This is AI-assisted review, not human qualification.
