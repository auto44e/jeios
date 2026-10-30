# W1 round 3 parser handoff

Observation time: 2026-10-30T00:47:47+00:00 (workspace timezone is UTC).
Repository: JeiOS worktree `wt52-parser-r3`.
Branch: `wip/52-parser-r3`.
Inspected commit: `5a3c14b6068983c4656fe9302e4d7988c10824d4`.
This file is intentionally uncommitted.

## State

- Implemented: `kernel/core/fdt.c` only.
- Tested: strict host compilation, ARM object compilation, host and ARM
  undefined-symbol checks, x86 smoke, ARM build and ARM QEMU smoke.
- Planned: lead integration and remote ARM boot CI; neither was run from this
  worker worktree.
- Blocked: the checked-in round-2 `tests/test_fdt.c` cannot pass unchanged
  because it has no `CONFLICT` name-table entry and retains one round-2
  expectation for duplicate tracking. The test file was not edited.

## Implementation

Implemented the frozen `include/jeios/fdt.h` round-3 behavior:

- `CONFLICT` status name.
- Immediate duplicate rejection for tracked nodes (root, all depth-2 nodes,
  and depth-3 `/reserved-memory` children), including disabled/classification-
  independent properties and second `/chosen` or `/reserved-memory` nodes.
- Consumed `status` and `device_type` string termination/empty checks.
- `/chosen` and `/reserved-memory` own status is ignored; memory nodes and
  `/reserved-memory` children retain status-based semantic skipping.
- Early root and `/reserved-memory` cell validation at the first child or
  childless `END_NODE`.
- `/chosen` and `/reserved-memory` `device_type = "memory"` conflict rejection.
- Reservation candidate validity before candidate-count enforcement.
- `je_boot_ctx_check()` count clamps before array indexing.
- Property-specific initrd diagnostics, with missing halves reported at the
  `/chosen` `END_NODE`.
- Removed whole-node stack assignment and other whole-struct constructs that
  could lower to freestanding `memcpy`/`memset`.

## Exact verification

`ASAN_OPTIONS=detect_leaks=0 make build/0/test_fdt`

- Result: exit 0; rebuilt the hosted FDT test binary.

`ASAN_OPTIONS=detect_leaks=0 ./build/0/test_fdt`

- Result: fails before parser assertions at `tests/test_fdt.c:3117` with
  UBSan: null pointer passed as argument 2 to `strcmp`. The test's local
  `k_names[]` table has the old 18 statuses and lacks the contract's appended
  `CONFLICT` entry. `tests/test_fdt.c` was not changed.

Non-persistent diagnostic command (streamed `sed` input; no test file edit):

```sh
sed -e 's/"BAD_RANGE", "OVERLAP",/"BAD_RANGE", "OVERLAP", "CONFLICT",/' \
    -e 's/abort();/return;/' tests/test_fdt.c |
  gcc -Iinclude -DJE_TEST_MODE=0 -Itests -x c - -std=c11 -O1 -g \
    -Wall -Wextra -Wconversion -Wshadow -Wstrict-prototypes \
    -Wno-return-type -fsanitize=address,undefined -fno-sanitize-recover=all \
    kernel/core/fdt.c -o /tmp/jeios-test_fdt-r3-all &&
  ASAN_OPTIONS=detect_leaks=0 /tmp/jeios-test_fdt-r3-all
```

- Result: all remaining suite output was PASS except this old expectation:
  `tests/test_fdt.c:1024`: `EXPECT(JE_FDT_OK)` got `DUPLICATE`,
  `err_off 240`; the following line 1025 context check also failed because
  the output was zeroed after that same failure.
- Mapping: round-3 rule 3 tracks every depth-2 node, so the old fixture's
  repeated `reg` on a non-memory depth-2 node is now a duplicate. No
  unmapped parser failure was observed.

`ASAN_OPTIONS=detect_leaks=0 make check`

- Result: exit 2 at `./build/0/test_fdt`; all preceding host suites passed.
  The failure is the stale round-2 FDT name table described above.

Strict host parser compile and undefined-symbol check:

```sh
gcc -Iinclude -std=c11 -O0 -Wall -Wextra -Werror -Wconversion -Wshadow \
  -Wstrict-prototypes -ffreestanding -fno-builtin -c kernel/core/fdt.c \
  -o /tmp/jeios-fdt-host-final.o
nm -u /tmp/jeios-fdt-host-final.o
```

- Result: compile passed; `nm -u` printed no symbols.

ARM parser object matrix, using the `mk/aarch64.mk` ARM flags plus each
optimization level:

```sh
for opt in -O0 -Os -O2; do
  aarch64-linux-gnu-gcc -Iinclude -std=c11 "$opt" -Wall -Wextra -Werror \
    -Wconversion -Wshadow -Wstrict-prototypes -ffreestanding -fno-builtin \
    -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
    -mstrict-align -mgeneral-regs-only -mcpu=cortex-a72 \
    -c kernel/core/fdt.c -o "/tmp/jeios-fdt-${opt#-}-final.o"
  aarch64-linux-gnu-nm -u "/tmp/jeios-fdt-${opt#-}-final.o"
done
```

- Result: all three compiles passed; all three `nm -u` lists were empty.

Host stack usage:

```sh
gcc -Iinclude -std=c11 -O0 -Wall -Wextra -Werror -Wconversion -Wshadow \
  -Wstrict-prototypes -ffreestanding -fno-builtin -fstack-usage \
  -c kernel/core/fdt.c -o /tmp/jeios-fdt-host-final-stack.o
grep je_fdt_parse /tmp/jeios-fdt-host-final-stack.su
```

- Result: `kernel/core/fdt.c:549:15:je_fdt_parse 2728 dynamic,bounded`.

`make aarch64`

- Result: PASS. Final image size 16081 bytes. The linker emitted the existing
  RWX LOAD-segment warning; no undefined symbols.

`make smoke-aarch64`

- Result: PASS on QEMU 8.2.2 / `cortex-a72`; DTB parse and boot-context
  validation markers completed.

`make smoke`

- Result: PASS on the existing x86 QEMU smoke profile, including all M3
  regression markers.

`git diff --check`

- Result: PASS.

## Not verified here

- Remote ARM boot CI was not invoked.
- The stale checked-in FDT test was not modified, per worker ownership rules.
- No claim of independent human review or release qualification is made.
- Build artifacts and `/tmp` objects/logs are verification outputs only and are
  not part of the source change.

## Next bounded task

The lead should integrate `kernel/core/fdt.c`, update the round-3 FDT tests in
the lead-owned test work, and run remote ARM boot CI. Pass criteria are a
clean updated FDT suite, `make check`, sequential `make aarch64`,
`make smoke-aarch64`, and remote normal/vector ARM boot evidence.
