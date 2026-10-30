# W1 round 4 parser worker notes

Observation time: 2026-10-30T01:31:23Z (UTC)
Repository: JeiOS
Branch: `wip/52-parser-r4`
Inspected commit: `44187ac6b2038f74176f774c007a40a0b349f610`

## State

- Implemented: `kernel/core/fdt.c` only. The parser now checks `device_type`
  strings only on depth-2 nodes, checks `/reserved-memory` child `status` at
  its PROP, defers memory-node `status` validity to the first check at
  END_NODE with the status PROP offset, and ignores unconsumed string values.
  `/reserved-memory` cell values are recorded safely and validated at the
  existing first-child/END_NODE decision point. Reserved-memory child `reg`
  decoding remains at END_NODE. Also fixed NULL `err_idx` reporting, removed
  the dead `first_kind` guard, stopped reusing `struct_end` for initrd
  validation, and removed the redundant RAM size check.
- Tested: all checks listed below passed locally. The supplied adversarial
  probe matched its pinned outcomes, including probes 1a-1g, 2a-2c, and the
  END_NODE-timing probe 3.
- Planned: lead integration with the other issue #52 work and the added
  round-4 tests from the other worker.
- Blocked: no local blocker. Remote ARM boot CI and independent human critical
  review were not performed by this worker.

## Exact verification

- `make -B build/0/test_fdt && ASAN_OPTIONS=detect_leaks=0 ./build/0/test_fdt`
  — PASS.
- `gcc -Iinclude -I/tmp/fdtprobe -std=c11 -O0 -Wall -Wextra -Werror
  -Wconversion -Wshadow -Wstrict-prototypes -Wno-unused-function
  -fsanitize=address,undefined -fno-sanitize-recover=all
  /tmp/fdtprobe/probe.c kernel/core/fdt.c -o /tmp/fdtprobe/probe-r4 &&
  ASAN_OPTIONS=detect_leaks=0 /tmp/fdtprobe/probe-r4` — PASS. The probe's
  unused builder helpers required the narrow `-Wno-unused-function` flag;
  parser warnings remained strict.
- Host parser matrix:
  `for opt in -O0 -Os -O2; do gcc -Iinclude -std=c11 "$opt" -Wall -Wextra
  -Werror -Wconversion -Wshadow -Wstrict-prototypes -ffreestanding
  -fno-builtin -fno-pie -fno-stack-protector
  -fno-asynchronous-unwind-tables -m64 -march=x86-64 -mno-red-zone
  -mgeneral-regs-only -c kernel/core/fdt.c
  -o "/tmp/fdtprobe/fdt-r4-${opt#-}.o" && echo "${opt}: compile OK" &&
  nm -u "/tmp/fdtprobe/fdt-r4-${opt#-}.o"; done`
  — all compiles passed; all undefined-symbol lists were empty, including
  no `memcpy`/`memset`.
- ARM parser matrix:
  `for opt in -O0 -Os -O2; do aarch64-linux-gnu-gcc -Iinclude -std=c11
  "$opt" -Wall -Wextra -Werror -Wconversion -Wshadow
  -Wstrict-prototypes -ffreestanding -fno-builtin -fno-pie
  -fno-stack-protector -fno-asynchronous-unwind-tables -mstrict-align
  -mgeneral-regs-only -mcpu=cortex-a72 -c kernel/core/fdt.c
  -o "/tmp/fdtprobe/fdt-r4-arm-${opt#-}.o" && echo "${opt}: compile OK" &&
  aarch64-linux-gnu-nm -u "/tmp/fdtprobe/fdt-r4-arm-${opt#-}.o"; done`
  — all compiles passed; all undefined-symbol lists were empty.
- `make aarch64` — PASS; ARM image linked, 16257 bytes. The existing linker
  RWX LOAD-segment warning was emitted.
- `make smoke-aarch64` — PASS on QEMU 8.2.2; DTB was copied, parsed,
  boot-context validation passed, and the terminal marker was observed.
- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS; all host suites, ELF
  checks, and isolation checks passed. Leak scanning was disabled because
  this managed environment may not permit LeakSanitizer task inspection.
- `make smoke` — PASS; the existing x86-64 QEMU smoke completed all markers.
- `git diff --check` — PASS.

## Evidence gaps and deviations

- Plain `make check` without `ASAN_OPTIONS=detect_leaks=0` was not rerun in
  this round; the documented managed-environment exception was used for the
  required check.
- Existing `tests/test_fdt.c` exercises the NULL `je_boot_ctx_check` argument
  paths but does not assert the new `JE_BOOT_ERR_FACTS` value when `ctx` or
  `facts` is NULL and `err_idx` is non-NULL. The implementation was compiled
  and covered by the full suite, but that exact sentinel assertion remains
  planned for the round-4 test worker.
- No remote CI result is claimed. No independent human qualification is
  claimed; this is AI-assisted local verification only.
- `AGENTS.md` asks for handoff/status documentation with implementation
  changes, but this worker was explicitly restricted to `kernel/core/fdt.c`;
  `docs/HANDOFF.md` and other non-owned files were not edited. The lead should
  carry the integrated status update.

Next bounded task: integrate this parser diff with the issue #52 lead branch,
run the expanded round-4 FDT tests and remote ARM job, and preserve the
implemented/tested/planned/blocked distinctions above.
