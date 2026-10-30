# W3 round 5 — tools/smoke_arm.py (harness) — worker notes

Observed 2026-10-30, worktree wt52-armglue-r5, branch wip/52-armglue-r5, base e59d580.
Only `tools/smoke_arm.py` changed (git status: ` M tools/smoke_arm.py`). Not committed.
Implemented by a single AI agent (W3); AI-assisted, not human-reviewed.

## Fixes (Codex rev5 findings)

1. **P1 stderr ignored on PASS.** `run_qemu` now returns `(log, early, stderr)`;
   stderr is re-read after terminate+reap (bounded: > 64 KiB is itself a FAIL).
   `check_stderr()` runs first in `main()` on every path. Each non-empty stderr
   line must `fullmatch`
   `qemu-system-aarch64: terminating on signal 15 from pid (\d+)(?: \([^\n]*\))?`
   AND the pid must equal `os.getpid()` (the harness sent the SIGTERM); at most one
   such line. Anything else -> FAIL, full stderr printed. The accepted notice is
   printed as evidence (`stderr: ... (expected kill notice)`).
   Exact real-QEMU text, observed by hand-killing real QEMU 8.2.2
   (Debian 1:8.2.2+ds-0ubuntu1.18) with `kill -TERM` from bash:
   `qemu-system-aarch64: terminating on signal 15 from pid 1561788 (/bin/bash)\n`
   (od -c verified: single line, LF only). Under the harness it reads `(python3)`.
2. **P1 substring markers.** Markers are now `(desc, line->bool)` matched against
   WHOLE normalized lines, in order (each strictly after the previous match's line).
   Fixed lines: `exact()` equality. Value-carrying lines: `valued()` = `re.fullmatch`
   of fixed text + `0x[0-9a-f]{16}` fields, anchored at the line start.
   `COPIED_RE` / `VALIDATED_RE` are now applied by `fullmatch` per line (were
   `findall` over the raw log, spoofable by a prefix).
3. **P3 LF/CRLF.** No marker carries `\r\n` any more; `split_lines` strips one
   trailing `\r` once, and everything compares on those lines. LF-only logs PASS.

## Deviations from the literal instruction (all stricter) — reviewer please check

- Valued markers use full-line regex (prefix + exact-width hex, nothing after)
  instead of bare `line.startswith(prefix)`. Rationale: startswith would accept
  trailing junk; and the vector-test ESR/SPSR line needs the SPSR checked at the
  END of the same line (`esr=...f200a55a elr=<hex> spsr=...800003c5`), which a
  prefix cannot express without losing the old SPSR assertion.
- Kill-notice pid must equal the harness pid (instruction said `<digits>`).
- ZERO kill notices is allowed (QEMU may be SIGKILLed by the 5 s reap fallback, or
  a non-QEMU shim may print nothing). More than one is a FAIL.
- Vector-test lines are now pinned in full: `triggering deliberate BRK #0xa55a ...`,
  the whole `EXCEPTION ... il=0x...1 iss=0x...a55a` line, and
  `vector self-test PASS (<anything>)`. If main.c's text changes, this harness
  must change with it (coupling to whoever owns kernel/arch/aarch64/main.c).
- Program-name prefix is the literal `qemu-system-aarch64:` (QEMU uses the argv[0]
  basename). A `--qemu /some/path/qemu-system-aarch64` still matches; a QEMU
  binary under another basename would FAIL (fail-closed).
- Unchanged and still substring-based on purpose (stricter direction): the
  `FATAL` anywhere check, the `--dtb-bad` forbidden-marker checks, and the
  early-stop trigger (only shortens a run that already cannot PASS).

## Commands run and results

Build: `make aarch64` -> `aarch64 Image: 16257 bytes`.

Real QEMU 8.2.2, all four modes (logs /tmp/w52/w3r5-real-*.log):
```
make smoke-aarch64 ARM_REQUIRE=1                    -> rc=0 PASS, stderr: kill notice (python3)
make smoke-aarch64 ARM_REQUIRE=1 ARM_BSS_CHECK=1    -> rc=0 PASS
make smoke-aarch64 ARM_REQUIRE=1 ARM_VECTOR_TEST=1  -> rc=0 PASS
make smoke-aarch64 ARM_REQUIRE=1 ARM_DTB_BAD=1      -> rc=0 PASS
```

Shims: `/tmp/w52/w3r5-shims/qemu-shim` replays the fresh real serial log of each
mode (`real-<mode>.log`, copied from build/aarch64*/serial.log after the real runs)
with mutation `SHIM_SERIAL` and stderr behaviour `SHIM_STDERR` (on SIGTERM it
prints the notice with `$PPID` = harness pid). Driver: `/tmp/w52/w3r5-shims/run.sh
MODE SERIAL STDERR` (runs `python3 tools/smoke_arm.py build/aarch64/Image --qemu
<shim> --timeout 5 --require [mode flag]`). Full output:
/tmp/w52/w3r5-shimrun.txt. Summary, identical in all four modes
(default/bsscheck/vectest/dtbbad):

| serial / stderr | result |
|---|---|
| copy / kill (valid CRLF log) | PASS |
| copy / none | PASS |
| copy / noname (notice without `(name)`) | PASS |
| **lf / kill (LF-only valid log, P3)** | **PASS** |
| **copy / fatal (`FATAL qemu internal error` on stderr, P1 #1 repro)** | **FAIL unexpected QEMU stderr** (stderr printed) |
| copy / warn (`qemu-system-aarch64: warning: ...`) | FAIL unexpected QEMU stderr |
| copy / wrongpid (notice from pid 1) | FAIL |
| copy / sig9txt (notice says signal 9) | FAIL |
| copy / twice (two notices) | FAIL |
| copy / suffix (notice + ` extra`) | FAIL |
| **spoof / kill (`fake-prefix: ` on every line but terminal, CRC/pa_bits intact, P1 #2 repro)** | **FAIL missing marker (whole line): entry EL1** |
| spooflf / kill (same, LF-only) | FAIL |
| midline / kill (`noise ` before the bss line) | FAIL missing bss marker |
| crcmid / kill (` x` after dtb copied line) | FAIL missing dtb copied |
| tailjunk (default, bsscheck: junk after fixed `dtb parsed` line) | FAIL |
| tailjunkv (default, vectest, dtbbad: junk after valued `vectors installed` line) | FAIL |

Round-4 regression shims (/tmp/w52/w3r4-shims/shim-*, `--timeout 3 --require`;
output /tmp/w52/w3r5-r4regress.txt): outcomes identical to round 4's "new" column —
good/slow/dtbbad-good/vectest-good PASS; nocrlf, latefatal, termfatal, latemarker,
partialtail, hang and their dtbbad/vectest variants FAIL with the same messages.

`make check` -> rc=0 (log /tmp/w52/w3r5-check.log). `git diff --check` -> clean.

## NOT verified

- Remote ARM CI: not observed. All results above are local.
- QEMU versions other than 8.2.2; the 5 s SIGKILL reap fallback with real QEMU
  (only the zero-notice case was shim-tested).
- `make smoke` (x86) not run — this change does not touch x86 code.
- No pyflakes/linters available locally (`python3 -m pyflakes` not installed);
  checked with `python3 -m py_compile` and by execution only.
