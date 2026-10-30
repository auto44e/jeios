# W3 round 4 — harness (tools/smoke_arm.py, mk/aarch64.mk comment) — 2026-10-30

State: implemented + locally tested (shims + real QEMU 8.2.2). Not committed. Remote ARM CI NOT observed.
Base commit: a4712a8 (wip/52-armglue-r4). Files changed: tools/smoke_arm.py, mk/aarch64.mk (comment only).

## Changes
1. P1 (false PASS): markers now count only on COMPLETE lines (split on \n, one trailing \r stripped).
   During polling the terminal marker must equal a whole complete line (no substring, no line still
   being written). After it is seen QEMU keeps running for a full SETTLE_S (0.5 s), then is terminated
   AND reaped, and the WHOLE serial file is re-read; all validation runs on that final log:
   - trailing partial (unterminated) line -> FAIL;
   - terminal line must appear exactly once and be the LAST complete line (anything after it -> FAIL);
   - FATAL rule unchanged in intent (non-dtb-bad: any FATAL in raw log -> FAIL; dtb-bad: FATAL lines,
     CR/LF-normalised, must be exactly [bad_line]);
   - ordered markers still substring-ordered over the log, but only after the partial-tail reject, so
     every match lies on a complete line. Non-terminal markers stay prefix/substring (esr=/spsr= are
     mid-line fragments by design).
   Early-stop path (FATAL outside dtb-bad / VALIDATED in dtb-bad) also kills, re-reads, and runs the
   same validation; a defensive final fail() fires if validation somehow passed after an early stop.
2. P2 timeout: deadline = start + --timeout taken before Popen and covers settle. Terminal seen with
   < SETTLE_S left -> FAIL "timeout before settle complete" (PASS needs the full window; settle is
   never extended past the deadline, and is not shortened into a PASS). Only the terminate/reap
   (<= 5 s, then SIGKILL) runs outside the deadline — documented in the docstring.
   Help text, module docstring and mk/aarch64.mk comment updated to match.
3. P2: --timeout uses argparse type timeout_arg: float, math.isfinite, > 0; else argparse error (exit 2).
   The old "< 0" check was removed (subsumed).
4. P3: tool_version() and qemu_version() both use timeout=VERSION_TIMEOUT_S (10). tool_version on
   timeout prints "(unknown)" (evidence only; preflight qemu_version already gated --require).

## Decisions / deviations (for the reviewer)
- "Unexpected marker after terminal": implemented as ANY complete line after the terminal line,
  not only known JeiOS markers. Verified real logs end exactly with the terminal line + \r\n
  (od of build/aarch64/serial.log tail), so this is strict but not flaky on current images.
- Old code's 0.6 s-after-settle FATAL: a write that lands AFTER QEMU is killed is unobservable by any
  harness. The fix's guarantee is "everything written up to the kill is judged"; I proved that with a
  shim that writes FATAL from its SIGTERM handler (termfatal) — old harness PASSes it, new FAILs.
  latefatal (FATAL 0.3 s after terminal, inside settle) FAILs in both old and new.
- realqemu --timeout 0.1 now FAILs (settle can't fit) — matches the brief; reviewer saw old PASS ~0.68 s.
- hangver: a binary that answers the preflight --version but hangs the evidence --version now costs
  10 s and still PASSes with "qemu: (unknown)". Not turned into a FAIL (brief only asked for a bound).
- No contract (include/jeios/fdt.h) concerns this round.

## Commands run and results (shims in /tmp/w52/w3r4-shims, never in repo; old harness = git show HEAD:tools/smoke_arm.py in /tmp/w52/w3r4-old)
Shim matrix: python3 <harness> build/aarch64/Image --qemu /tmp/w52/w3r4-shims/shim-<m> --timeout 3 --require
(each shim replays a real serial log captured from make smoke-aarch64; output /tmp/w52/w3r4-shimrun.txt)
```
good         old   0.62s QEMU aarch64 smoke: PASS
good         new   0.65s QEMU aarch64 smoke: PASS
nocrlf       old   0.62s QEMU aarch64 smoke: PASS
nocrlf       new   3.07s QEMU aarch64 smoke: FAIL (timeout before final marker after 3.0s (waiting for: JeiOS/aarch64: initialization complete; halted))
latefatal    old   0.61s QEMU aarch64 smoke: FAIL (FATAL marker present)
latefatal    new   0.62s QEMU aarch64 smoke: FAIL (FATAL marker present)
termfatal    old   0.62s QEMU aarch64 smoke: PASS
termfatal    new   0.63s QEMU aarch64 smoke: FAIL (FATAL marker present)
latemarker   old   0.63s QEMU aarch64 smoke: PASS
latemarker   new   0.62s QEMU aarch64 smoke: FAIL (unexpected output after terminal line: ['JeiOS/aarch64: vectors installed'])
partialtail  old   0.61s QEMU aarch64 smoke: PASS
partialtail  new   0.61s QEMU aarch64 smoke: FAIL (trailing partial (unterminated) serial line: 'JeiOS/aarch64: EXC')
slow         old   1.63s QEMU aarch64 smoke: PASS
slow         new   1.58s QEMU aarch64 smoke: PASS
hang         old   3.08s QEMU aarch64 smoke: FAIL (timeout before final marker after 3.0s (waiting for: JeiOS/aarch64: initialization complete; halted))
hang         new   3.07s QEMU aarch64 smoke: FAIL (timeout before final marker after 3.0s (waiting for: JeiOS/aarch64: initialization complete; halted))
hangver      old  60.01s
hangver      new  10.65s QEMU aarch64 smoke: PASS
```
(hangver old: blank = hung, killed by outer `timeout 60`.)
Same shims replaying the dtb-bad and vector-test logs (new harness only, /tmp/w52/w3r4-shimrun2.txt):
```
good         dtb-bad: QEMU aarch64 smoke: PASS
good         vector : QEMU aarch64 smoke: PASS
nocrlf       dtb-bad: QEMU aarch64 smoke: FAIL (timeout before final marker after 3.0s (waiting for: JeiOS/aarch64: FATAL dtb rejected status=BAD_TOKEN off=0x0000000000000040))
nocrlf       vector : QEMU aarch64 smoke: FAIL (timeout before final marker after 3.0s (waiting for: JeiOS/aarch64: exception handled; halted))
termfatal    dtb-bad: QEMU aarch64 smoke: FAIL (--dtb-bad: expected exactly the BAD_TOKEN FATAL line, got ['JeiOS/aarch64: FATAL dtb rejected status=BAD_TOKEN off=0x0000000000000040', 'JeiOS/aarch64: FATAL at kill'])
termfatal    vector : QEMU aarch64 smoke: FAIL (FATAL marker present)
latemarker   dtb-bad: QEMU aarch64 smoke: FAIL (unexpected output after terminal line: ['JeiOS/aarch64: vectors installed'])
latemarker   vector : QEMU aarch64 smoke: FAIL (unexpected output after terminal line: ['JeiOS/aarch64: vectors installed'])
partialtail  dtb-bad: QEMU aarch64 smoke: FAIL (trailing partial (unterminated) serial line: 'JeiOS/aarch64: EXC')
partialtail  vector : QEMU aarch64 smoke: FAIL (trailing partial (unterminated) serial line: 'JeiOS/aarch64: EXC')
```
Timeout cases (/tmp/w52/w3r4-timeout.txt; shim-slow writes the log after 1.0 s):
```
slow --timeout 1.2  1.12s QEMU aarch64 smoke: FAIL (timeout before settle complete: terminal marker seen with 0.145s of --timeout 1.2s left, need 0.5s)
slow --timeout 1.6  1.58s QEMU aarch64 smoke: PASS
realqemu --timeout 0.1  0.20s QEMU aarch64 smoke: FAIL (timeout before settle complete: terminal marker seen with 0.000s of --timeout 0.1s left, need 0.5s)
realqemu --timeout 0.4  0.19s QEMU aarch64 smoke: FAIL (timeout before settle complete: terminal marker seen with 0.299s of --timeout 0.4s left, need 0.5s)
--timeout=nan:
smoke_arm.py: error: argument --timeout: must be finite and > 0, got 'nan'
rc=2
--timeout=inf:
smoke_arm.py: error: argument --timeout: must be finite and > 0, got 'inf'
rc=2
--timeout=-inf:
smoke_arm.py: error: argument --timeout: must be finite and > 0, got '-inf'
rc=2
--timeout=0:
smoke_arm.py: error: argument --timeout: must be finite and > 0, got '0'
rc=2
--timeout=-1:
smoke_arm.py: error: argument --timeout: must be finite and > 0, got '-1'
rc=2
--timeout=abc:
smoke_arm.py: error: argument --timeout: not a number: 'abc'
rc=2
make ARM_SMOKE_TIMEOUT=0:
smoke_arm.py: error: argument --timeout: must be finite and > 0, got '0'
make: *** [mk/aarch64.mk:120: smoke-aarch64] Error 2
rc=2
```
=> ARM_SMOKE_TIMEOUT=0 is now a PARSE ERROR (argparse exit 2, make Error 2), not a run.

Real QEMU 8.2.2 (logs /tmp/w52/w3r4-real-*.log):
- make smoke-aarch64 ARM_REQUIRE=1                    -> rc=0, PASS (pa_bits=44, crc32=0xcc76dc2b)
- make smoke-aarch64 ARM_REQUIRE=1 ARM_BSS_CHECK=1    -> rc=0, PASS
- make smoke-aarch64 ARM_REQUIRE=1 ARM_VECTOR_TEST=1  -> rc=0, PASS
- make smoke-aarch64 ARM_REQUIRE=1 ARM_DTB_BAD=1      -> rc=0, PASS
make check -> rc=0. git diff --check -> clean. python3 -m py_compile tools/smoke_arm.py -> ok.

## NOT verified
- Remote ARM CI (GitHub Actions) not run/observed.
- Not run under any Python other than local python3 (Python 3.12.3).
- make smoke (x86) not run — x86 harness untouched.
