No blocking defects. Most of the specific risks you listed check out as correct:

- **IAR read once:** yes.
- **Spurious path:** writes no EOIR.
- **CNTPCT sample:** taken before the re-arm, and the `isb` follows the TVAL write.
- **CNTFRQ:** masked to bits [31:0], and the order is TVAL then ENABLE.
- **GICC_CTLR:** must read back exactly `1`.
- **PMR vs priority:** `0x80 < 0xFF`.
- **Device leaf (`paging.c:404`):** EL1-only RW, PXN+UXN, nGnRnE, SH=00, `0x0060000000000407`.
- **GICC_DIR page:** each GIC region is exactly `JE_GIC_PAGE`, so `0x08011000` is never mapped.
- **Hosted test hooks:** only compiled in under the test define.

Below are the gaps that remain, most severe first.

### Medium

1. **`gic.c:252-254`: the ISENABLER0 check doesn't fail closed on extra enables.** It only checks that bit 30 is set. Init never clears other enables (no ICENABLER for other PPIs or SPIs) and never clears stale pending/active state (ICPENDR/ICACTIVER).
   - The comment at `handle_one` (`gic.c:~300`, "only INTID 30 is enabled") describes an invariant that init doesn't create.
   - If firmware left INTID 27 or an SPI enabled, init still returns `JE_OK`. On QEMU only the smoke oracle catches extra bank-0 bits; SPIs are checked by nothing.
   - If INTID 30 was left *active* (warm restart), IAR returns 1023 forever and the proof dies with "unexpected-spurious". That fails closed, but by accident.
   - The hosted test has no case for extra enabled bits, so this is untested kernel behavior.
   - Fix: refuse when `isenabler0 & ~(0xFFFF | timer_bit)` is non-zero, or write ICENABLER first. Also clear INTID 30's pending/active state before enabling it.

2. **`gic.c:303-307`: `ISTATUS==0` after acking INTID 30 is treated as `JE_ERR_IO`, which the proof turns into a fatal "handle-failed".**
   - On real hardware, writing TVAL then `isb` doesn't guarantee the level line has dropped at the GIC before EOIR. The interrupt can re-pend, and the next ack then sees ISTATUS=0.
   - That is a benign race, not an I/O error. QEMU deasserts instantly, so this never shows up there. PR-2b will reuse `handle_one` from the IRQ path, where this becomes a spurious-fatal.
   - Fix: return a distinct status for "acked, condition already cleared" (EOI'd, no tick) and let the caller decide.

3. **`gic.c:300` (and `arm_read_cntpct`): there's no `isb` before `mrs CNTPCT_EL0`.**
   - Per DDI0487, CNTPCT reads can run speculatively and out of order. That applies relative to the IAR Device load before it and the TVAL `msr` after it.
   - So "sample before re-arm" is only guaranteed in program order, not architecturally, and the `delta >= interval` bound rests on that ordering. The window is nanoseconds and QEMU can't show it, but the proof claims the ordering.
   - Fix: `isb; mrs CNTPCT_EL0`, as Linux's `__arch_counter_get_cntpct` does.

### Low

4. **`gic.c:224-225`: the first MMIO access is a write (`GICD_CTLR=0`), before TYPER/IIDR identify the device.** On a wrong-machine or bad-address boot, it writes to an unidentified device. Fix: do the read-only identification (TYPER/IIDR) first, then write.
5. **No rollback on late refusals.** If the PMR or GICC_CTLR readback fails (`gic.c:260-268`), the GICD stays enabled and INTID 30 stays enabled. It's harmless while IRQs are masked and the caller halts. The header's "fails closed" wording should say the state is left partially enabled, or the code should disable GICD_CTLR on error.
6. **`gic.c:283`: `handle_one` checks `interval != 0` but not `<= INT32_MAX`.** This is inconsistent with the guard in `je_timer_arm`. Passing a value of 2³¹ or more re-arms with a negative TVAL, which fires immediately.
7. **`gic.c:309`: re-arming with TVAL measures from the moment of re-arm, so period drift accumulates.** The tick isn't a fixed-rate 100 Hz. Fine for PR-2a's lower-bound claim; if ADR 0012 D7 or PR-2b needs a fixed rate, use `CVAL += interval`.
8. **`gic.c:15`: nothing stops the hosted-test define from reaching a kernel build.** A `#if defined(JE_ARM_GIC_HOSTED_TEST) && !__STDC_HOSTED__` / `#error` guard would make that a compile error rather than relying on a link failure. The kernel path itself is clean today.
9. **`gic.c:74`: the INT32_MAX `_Static_assert` is a tautology** and guards nothing. The meaningful assert is the one in the test (`UINT32_MAX/100 < INT32_MAX`); move it into `gic.c`.

### Honesty / docs (not kernel code, but a project rule)

10. **`HANDOFF.md` and `IMPLEMENTATION.md` are stale against this diff.** They say the hosted GIC test, the `ARM_TIMER_POLL` make selector and the smoke-oracle update are still "planned / owned by the other author", and that the ARM smoke "exits FAIL". All three are in this diff, and you report 8/8 ARM smoke modes passing. These docs need refreshing with the current evidence and commit before merge. The HANDOFF entry also records the repository as `/oss/jeios` instead of `oss/jeios-workspace/jeios`.

I checked this by reading the diff and `paging.c` (Device leaf bits, page-only mapping) and `main.c` (`halt()` uses `wfe`). I did not rebuild or rerun anything. This is AI-assisted review, not independent human qualification.
