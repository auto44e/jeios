Review result: request changes. The current tests are strong on common register-value mutations, but several PR-2a properties remain demonstrably vacuous.

Observed locally:

- `make check`: PASS; `test_gic_bits` reports 1287 checks.
- Default and `--timer-poll` QEMU smoke: PASS.
- These passes do not close the gaps below.

Ranked findings:

1. **P1 — IRQ masking is not tested at all**

`tests/test_gic_bits.c:222-240` hardcodes `ISR_EL1` reads to zero, and no test calls `je_arm_read_isr_el1()`. `tools/smoke_arm.py:977-1012` also emits/checks no `ISR_EL1.I` or DAIF state.

A build that unmasks IRQs, or otherwise enters the vector path during the poll proof, is not rejected by the oracle. This directly misses the PR-2a “IRQs masked / polled only” boundary.

2. **P1 — GIC MMU attributes and the GICC guard page are not proven**

`tools/smoke_arm.py:767-777` recomputes the page count and addresses, but `tools/smoke_arm.py:795-800` probes only PL011 as Device memory.

It does not independently verify:

- GICD and GICC are Device-nGnRnE;
- `GICC + 0x1000` / `GICC_DIR` is unmapped.

Thus a wrong mapping type, or an accidentally mapped GICC guard page, can pass the smoke oracle while the leaf count remains correct.

3. **P1 — CNTPCT-before-rearm ordering is not asserted**

`tests/test_gic_bits.c:531-543` checks ordering of IAR, CNTP_CTL, TVAL, ISB, and EOIR, but never locates the `CNTPCT_EL0` read event.

Moving the counter sample after `CNTP_TVAL_EL0` still passes because `F.cntpct_step` is zero and the returned value is unchanged. ADR 0012 explicitly requires sampling CNTPCT before re-arm.

4. **P1 — The fake timer does not model re-arm/ISTATUS behavior**

`tests/test_gic_bits.c:89-94` and `:222-250` keep `F.istatus` as an independent constant. Writing TVAL or CTL does not change ISTATUS, create a deadline, clear the timer condition, or affect pending state.

The test proves “a TVAL write occurred,” not that the write actually re-arms the timer or deasserts the level condition. The QEMU delta check gives useful progress evidence, but not fake-backend semantic coverage.

5. **P1 — Upper CNTFRQ behavior is explicitly allowed to be wrong**

`tests/test_gic_bits.c:492-505` accepts either success or failure for the 300 GHz case. A non-masking implementation that examines the full 64-bit value and rejects it therefore passes.

The test’s own assertion at `:68-72` acknowledges that `interval > INT32_MAX` is unreachable after the required 32-bit mask. Consequently, the upper-bound refusal is not actually exercised, and the permissive branch masks a real failure.

6. **P2 — Post-enable GICD_CTLR readback is not independently covered**

`tests/test_gic_bits.c:433-434` uses `gicd_ctlr_stuck` for both the initial disable readback and the later enable readback. The mutation reaches only the first check.

Deleting the post-enable `GICD_CTLR == 1` readback/check can pass the hosted suite. The good-path final fake state is not equivalent to verifying the actual post-write readback.

7. **P2 — Spurious “no write” coverage only counts 32-bit writes**

`tests/test_gic_bits.c:565-569` checks `eoir_writes`, `EV_SW`, and `EV_W32`, but ignores `EV_W8` and does not check `F.violations`.

A spurious path that writes `GICC_EOIR` through the byte-write hook can pass these assertions: the fake records a violation, but the test never fails on it.

8. **P2 — IAR-exactly-once is not checked on every path**

Timer and spurious cases assert one IAR read (`tests/test_gic_bits.c:525`, `:567`). Unexpected INTID and `ISTATUS==0` cases do not (`:572-614`).

Additionally, `je_gic_hosted_read32()` at `:165` returns the same IAR on every read and has no acknowledge side effects. A double-read mutation can therefore survive those paths.

9. **P2 — Smoke does not check timer priority or group**

The smoke oracle correctly checks fixed values for:

- PMR `0xff`: `tools/smoke_arm.py:955-956`;
- GICC_CTLR exactly `1`, including FIQEn/EOImode: `:957-960`;
- GICD_CTLR `1`: `:961-962`;
- ISENABLER0 bit 30 and no unrelated PPIs: `:963-967`;
- CNTFRQ/interval: `:968-970`.

However, it does not check priority `0x80`, the strict relation `0x80 < 0xff`, or group-0 state. The hosted test covers priority/group reasonably, but the smoke oracle does not.

10. **P2 — DTB cross-check under-validates the timer flags**

`tools/smoke_arm.py:919-923` checks only `flags & 0xf == 4`. It accepts a wrong PPI CPU mask, despite the pinned DTB value being `0x104`.

The DTB check should validate the complete flags cell or explicitly justify which upper bits are flexible.

11. **P2 — Refusal cleanup is not asserted**

`tests/test_gic_bits.c:381-405` checks that later stages were not reached, but does not require rollback/quiescence after PMR or GICC_CTLR failures. `STOP_AFTER_GICC` is effectively empty.

A refusal can leave GICD and INTID 30 enabled and still pass. If “fail closed” includes safe post-failure state, this needs explicit assertions.

12. **P3 — Fake violations are not universally fatal**

`violation()` only increments a counter (`tests/test_gic_bits.c:109-114`). Several paths—spurious, unexpected INTID, polling, and most refusals—do not assert `F.violations == 0`.

The MMU leaf-count recomputation itself is real: `tools/smoke_arm.py:716-734` independently models region pages plus table frames, and `:767-777` compares the result. Its limitation is that count correctness does not prove attributes, exact table placement, or the unmapped guard page.

Assessment: the fixed-value smoke checks are meaningful, and the hosted suite catches many obvious GIC/timer mutations. But this is not yet a complete mutation-resistant PR-2a proof. The minimum fixes are the IRQ-mask assertion, GIC Device/guard-page MMU probes, CNTPCT ordering test, stricter CNTFRQ case, and stronger fake write/violation accounting.
