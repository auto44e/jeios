# W3 round 3 — ARM64 glue + QEMU harness fixes (issue #52)

Observed 2026-10-30, worktree wt52-armglue-r3, branch wip/52-armglue-r3, base
commit 115e2ac (real parser integrated). Changes are in the working tree only (not
committed). Host: QEMU 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18), aarch64-linux-gnu-gcc.
All results are LOCAL runs, not CI. This is an AI-authored change; the review it
gets is AI-assisted, not independent human review.

Files changed (all mine): kernel/arch/aarch64/boot.S, mk/aarch64.mk,
tools/smoke_arm.py. main.c is unchanged: its comment ("[ptr, ptr+MAX_TOTAL)
disjoint from the image") is still accurate and the `_Static_assert` that keeps
JE_ARM_DTB_GUARD_SPAN equal to JE_FDT_MAX_TOTAL is still there.

## What was done, per review item

1. **P1 boot.S wrap guard.** After the nonzero and alignment checks, and before
   any sum is formed: `movn x0, #0x20, lsl #16` (= ~0x200000 =
   0xFFFFFFFFFFDFFFFF = UINT64_MAX - SPAN), then `cmp x19, x0` and
   `b.hi dtb_fatal` (same FATAL marker). The comment explains why.
   **Pushback, for the record:** the round-1 code could NOT actually be bypassed
   this way. For x0 near 2^64, x0 >= _end, so `b.hs 9f` was taken before the sum
   was ever formed, and the wrapped extent [x0, 2^64) ∪ [0, <2 MiB) cannot reach
   the image at 0x4008xxxx. The new check is still worth having (defense in
   depth): it no longer depends on branch order, and a wrapping DTB extent is
   never a valid placement. It is NOT a fix for an exploitable overlap. The
   comment says this honestly.
2. **P2 --expect-pa-bits N** (default 44). It parses exactly one
   `boot context validated pa_bits=N\r\n` line and fails on any other value or
   count. It applies in the default, vector and bss modes. In --dtb-bad mode the
   validated line is forbidden, so there is no pa_bits check there.
3. **P2 run semantics.** `run_qemu()` uses Popen and polls the serial FILE every
   50 ms.
   - When the final marker is seen (the last entry of the mode's ordered marker
     list; for --dtb-bad that is the exact BAD_TOKEN FATAL line), the harness
     settles for 0.5 s and then terminates QEMU. That is the only PASS-eligible
     path; all marker, CRC and pa_bits checks still run afterwards.
   - QEMU exiting on its own at any point, including during the settle period,
     is a FAIL that prints the return code and the first 4 KiB of QEMU stderr.
   - If the deadline passes without the marker, it fails with "timeout before
     final marker after Ns (waiting for: ...)".
   - In a `finally`, it sends terminate, waits 5 s, then kills and waits, so no
     zombie or orphan is left on any path. `pgrep` confirmed no leftover QEMU
     after all the runs below.
   - QEMU stderr goes to an unlinked temp FILE, not a pipe: an undrained pipe
     could block QEMU.
   - Addition beyond the brief: it also stops early when the run can no longer
     pass. Outside --dtb-bad that is any "FATAL"; in --dtb-bad it is the
     forbidden "validated" marker. This only shortens the wait; the later checks
     still FAIL clearly. Without it, a good image under --dtb-bad would burn the
     full timeout.
   - `--timeout` is now a float, and negative values are rejected.
4. **P2 mk:** if more than one of ARM_BSS_CHECK / ARM_VECTOR_TEST / ARM_DTB_BAD is
   set to `1`, make fails with `$(error ...)`. Only the value `1` counts, so
   `ARM_BSS_CHECK=0 ARM_VECTOR_TEST=1` is still allowed. This matches how each
   toggle was already tested with `ifeq (...,1)`.
5. **P3 --require preflight:**
   - **QEMU version:** parses `QEMU emulator version X.Y.Z`. Anything other than
     8.2.x, or unparseable output, is a FAIL under --require. Without --require
     it prints a WARNING and continues (my decision: newer QEMU still has the
     versioned virt-8.2 machine, and the CRC check would catch a different DTB).
   - **Fixture:** missing / CRC define absent / unparseable was ALREADY a FAIL in
     every mode, never a SKIP. That stays; under --require the message now gets
     a `--require:` prefix. The fixture check now runs before the image-exists
     check.
6. **ARM_SMOKE_TIMEOUT ?= 15**, passed as `--timeout $(ARM_SMOKE_TIMEOUT)`. The
   name is listed as frozen in the mk comment.
   - Test hook added: `--qemu PATH` overrides the binary. It is used only to prove
     self-exit detection with shims; the version preflight still applies to it.

## Verification (exact commands, local)

Clean rebuild (`rm -rf build/aarch64*`), then `make aarch64 <mode>` for each mode;
logs are in /tmp/w3r3/build-*.log:
```
default            rc=0  1 warning: ld "LOAD segment with RWX permissions" (pre-existing)  Image 15833 B
ARM_VECTOR_TEST=1  rc=0  same single RWX warning                                          Image 16225 B
ARM_BSS_CHECK=1    rc=0  same single RWX warning                                          Image 15833 B
ARM_DTB_BAD=1      rc=0  same single RWX warning                                          Image 16057 B
```
`make smoke-aarch64 ARM_REQUIRE=1 [mode]`: **PASS in all four modes**. Each took
about 0.64–0.69 s wall time, because QEMU is now stopped early. Default output:
`pa: pa_bits=44 == expected`, `dtb: crc32=0xcc76dc2b over 0x1d02 bytes == fixture`,
`QEMU aarch64 smoke: PASS`. These were re-run after the last harness edit: still
4/4 PASS.

Negative checks:
```
make smoke-aarch64 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=0
  -> FAIL (timeout before final marker after 0.0s (waiting for: JeiOS/aarch64: initialization complete; halted))  make rc=2
make smoke-aarch64 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=1
  -> PASS rc=0   (NOTE: the brief said "0 (or 1) must FAIL"; 1 s is MORE than
     enough on this host since the boot finishes in ~0.15 s, so 1 correctly PASSes.
     0 is the timeout-FAIL demonstration.)
python3 tools/smoke_arm.py build/aarch64/Image --require --expect-pa-bits 40
  -> FAIL (pa_bits=44 != expected 40) rc=1
make aarch64 ARM_DTB_BAD=1 ARM_VECTOR_TEST=1
  -> mk/aarch64.mk:19: *** ARM_BSS_CHECK, ARM_VECTOR_TEST and ARM_DTB_BAD are mutually exclusive; set at most one to 1.  Stop.  rc=2
make aarch64 ARM_BSS_CHECK=1 ARM_VECTOR_TEST=1 ARM_DTB_BAD=1  -> same error rc=2
make aarch64 ARM_BSS_CHECK=0 ARM_VECTOR_TEST=1                -> builds rc=0
python3 tools/smoke_arm.py --require /nonexistent  -> FAIL (--require: image /nonexistent not built) rc=1
python3 tools/smoke_arm.py /nonexistent            -> SKIP rc=0 (unchanged non-require behaviour)
python3 tools/smoke_arm.py build/aarch64-dtbbad/Image --require   (bad image, default expectations)
  -> FAIL (FATAL marker present) rc=1 in 0.63 s (early stop on FATAL)
python3 tools/smoke_arm.py build/aarch64/Image --require --dtb-bad (good image, dtb-bad expectations)
  -> FAIL (--dtb-bad: expected exactly the BAD_TOKEN FATAL line, got []) rc=1 in 0.63 s
```
Self-exit detection, using shims in /tmp/w3r3/shim/ (not in the repo). `--version`
passes through to the real QEMU so the preflight still passes:
```
--qemu shim/qemu-exit3         (exits 3 immediately on boot)
  -> qemu stderr: shim: simulated abort
     FAIL (QEMU exited on its own with return code 3 (...crash/abort)) rc=1
--qemu shim/qemu-midboot-kill  (exec timeout -s KILL 0.03 qemu-system-aarch64 "$@": real boot, killed mid-way)
  -> FAIL (QEMU exited on its own with return code -9 ...) rc=1
--require --qemu shim/qemu-old (reports "QEMU emulator version 9.1.0")
  -> FAIL (--require: ... version 9.1.0, need 8.2.x ...) rc=1
--qemu shim/qemu-old (no --require) -> WARNING ... then PASS rc=0
```
Fixture preflight: I copied the script to /tmp/w3r3/fx/tools/ next to a
manipulated /tmp/w3r3/fx/tests/fixtures/ and ran it with `--require` against
build/aarch64/Image. The committed fixture was NOT touched:
- fixture missing → rc=1 "--require: cannot read DTB fixture: [Errno 2] ...";
- CRC define removed → rc=1 "QEMU_VIRT82_DTB_CRC32 not found";
- CRC define = `zzz` → rc=1, same message.

`make check` → rc=0. `git diff --check` → clean.

## boot.S guard: disassembly and boundary reasoning

`aarch64-linux-gnu-objdump -d build/aarch64/jeios-arm.elf` (default build):
```
400800a0: b4000933  cbz   x19, 400801c4 <dtb_fatal>
400800a4: f2400a7f  tst   x19, #0x7
400800a8: 540008e1  b.ne  400801c4 <dtb_fatal>
400800ac: 92a00400  mov   x0, #0xffffffffffdfffff   // MOVN #0x20, LSL #16
400800b0: eb00027f  cmp   x19, x0
400800b4: 54000888  b.hi  400801c4 <dtb_fatal>      // unsigned >
400800b8: 900000c0  adrp  x0, 40098000
400800bc: 91008000  add   x0, x0, #0x20             // _end = 0x40098020
400800c0: eb00027f  cmp   x19, x0
400800c4: 540000e2  b.cs  400800e0                  // >= _end: disjoint
400800c8: 90000001  adrp  x1, 40080000 <_start>
400800cc: 91000021  add   x1, x1, #0x0              // _text
400800d0: d2a00402  mov   x2, #0x200000
400800d4: 8b020262  add   x2, x19, x2
400800d8: eb01005f  cmp   x2, x1
400800dc: 54000748  b.hi  400801c4 <dtb_fatal>
```
Reasoning:
- `cmp x19, x0` sets C = (x19 >= bound) and Z = (x19 == bound). `b.hi` branches
  when C=1 and Z=0, i.e. x19 > bound unsigned.
- x0 = UINT64_MAX - MAX_TOTAL = 0xFFFFFFFFFFDFFFFF gives Z=1, so this check
  ACCEPTS it. Its sum would be exactly UINT64_MAX, with no wrap. However, this
  value is not 8-aligned, so the earlier `tst #7` already rejects it overall.
- The largest aligned value that passes is 0xFFFFFFFFFFDFFFF8 (sum
  0xFFFFFFFFFFFFFFF8, no wrap). That value is >= _end, so it is disjoint and
  accepted.
- 0xFFFFFFFFFFDFFFFF + 1 (the first value the check rejects) and
  0xFFFFFFFFFFDFFFF8 + 8 = 0xFFFFFFFFFFE00000 (the first aligned one) are both
  > bound, so they go to dtb_fatal.

**Executed, not just reasoned.** The guard instructions were extracted VERBATIM
from boot.S by awk (from `cbz x19` to the overlap `b.hi`) into a scratch
bare-metal harness, /tmp/w3r3/guard/t.S. In that harness `9:` returns 'A' and
`dtb_fatal` returns 'R'. It is linked with _text=0x40080000 and the real
_end=0x40098020, and was run on `qemu-system-aarch64 -M virt-8.2 -cpu cortex-a72`.
Test values:
```
0, 1, 8, _text-2M, _text-2M+8, _text, _end-8, _end, 0x48000000,
0xFFFFFFFFFFDFFFFF, 0xFFFFFFFFFFDFFFF8, 0xFFFFFFFFFFE00000, 0xFFFFFFFFFFFFFFF8
```
Output `RRAARRRAARARR` matched the expected `RRAARRRAARARR`; no mismatch marker
'!' was printed.

## NOT verified / limits
- No remote CI was run. Everything above is local on one host.
- The real kernel was not booted with an adversarial x0. QEMU direct boot always
  passes 0x48000000 and there is no gdb on this host. The guard boundaries were
  tested by running the identical instruction sequence in the scratch harness,
  not inside the kernel image itself.
- The settle window (0.5 s) is a heuristic. Output arriving later than that after
  the final marker would not be seen. The kernel halts in WFE immediately after
  the terminal marker, so on this target there is nothing that late to miss.
- The QEMU version pin is to the major.minor version (8.2.x). Patch-level DTB
  differences would be caught by the CRC check, not by the preflight.

## Contract (fdt.h) concerns
None this round.
