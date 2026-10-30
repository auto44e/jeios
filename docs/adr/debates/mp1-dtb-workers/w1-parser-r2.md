# Worker notes — issue #52, W1 round 2

Observation time: 2026-10-30T00:09:17Z  
Repository: JeiOS  
Branch: `wip/52-parser-r2`  
Inspected base commit: `bdf446ad3ab4b96a560bd56da46b30587d67ceba`

## State

- Implemented: `kernel/core/fdt.c` only.
- Tested: ARM64 compile/link and ARM64 QEMU smoke passed. All earlier hosted
  suites in `ASAN_OPTIONS=detect_leaks=0 make check` passed.
- Planned: lead integration and the updated independent hosted suite, whose
  round-2 test changes are not present in this worktree.
- Blocked: the local round-1 `tests/test_fdt.c` still contains the stale
  expectation at line 2287; it expects `BAD_RANGE`, while the updated contract
  requires `RANGE_LIMIT`.

## Implemented

1. When the root has only one of `#address-cells` and `#size-cells`, reject at
   the first child `BEGIN_NODE` with `BAD_CELLS` and that token offset, before
   any child semantic walk.
2. Permit zero-size struct/string blocks at their valid `totalsize` boundary;
   only non-empty struct/string blocks require bytes through their block end in
   the copied input. Preserve the required memreserve terminator byte checks.
3. Report `/reserved-memory` candidate overflow at the offending `reg` PROP
   token, as required by the updated contract.

No frozen header or other worker-owned file was changed. No contract concern or
deviation was identified.

## Exact verification

- `git diff --check` — PASS.
- `make build/0/test_fdt && ASAN_OPTIONS=detect_leaks=0 ./build/0/test_fdt` —
  build PASS; executable stops at the known stale round-1 assertion:
  `tests/test_fdt.c:2287: want BAD_RANGE got RANGE_LIMIT (err_off 884)`.
  This is the expected local-suite disagreement from the task; no other
  failure was reported before the suite aborted.
- `make aarch64` — PASS. Required ARM flags were used:
  `-mstrict-align -mgeneral-regs-only`; final image size was 15,833 bytes. The
  existing linker warning about an RWX LOAD segment was emitted.
- `ASAN_OPTIONS=detect_leaks=0 make check` — all suites before FDT passed:
  core, boot, allocator, VM, uframe, syscall, handle, reduced-width handle,
  and endpoint. The check stopped at the same expected stale FDT assertion
  above. Leak scanning was disabled because of the documented managed
  environment limitation.
- `make smoke-aarch64` — PASS under QEMU 8.2.2 with GCC 13.3.0. The log
  showed DTB copy, parse (`ram=1`, `rsv=0`, `ignored=0`, `dynamic=0`,
  `initrd=0`), boot-context validation (`pa_bits=44`), and normal halt.
  This was rerun against the final rebuilt image after the ordering check.

- After the final ordering adjustment, the exact
  `make build/0/test_fdt && ASAN_OPTIONS=detect_leaks=0 ./build/0/test_fdt`
  rerun produced the same single expected stale-suite failure at line 2287;
  no additional failure appeared.

## Not verified

- The updated independent hosted suite is not present in this round-2 worktree;
  its replacement for the stale line-2287 expectation was not run.
- Remote ARM boot CI was not run or queried; network access was not used.
- No commit, push, or git configuration change was made.

## Next bounded task

The lead should run the updated `tests/test_fdt.c` suite after integration.
Pass criteria: the three contract cases pass, including exact `err_off`
values, with no new failures; then retain the ARM build/smoke evidence above
and separately record remote ARM CI status.
