# W1 worker notes — issue #52 parser

Observation: 2026-10-29T23:37:27Z (UTC; wall-clock time recorded by the worker)
Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt52-parser`
Branch: `wip/52-parser`
Inspected base commit: `a55c2ffe47a1362b62ca6dc821d820265f23f88b`

## State

- Implemented: `kernel/core/fdt.c` now provides all five frozen-contract entry
  points: status names, copy planning, bounded parsing, boot-facts checking,
  and bitwise IEEE CRC-32.
- Tested: targeted host/ARM builds and the external scratch driver passed as
  listed below. Full `make check` was also run with leak scanning disabled for
  this environment.
- Planned: lead integration with the completed hosted FDT suite and ARM glue/
  remote boot-CI changes owned by other workers.
- Blocked: no local ARM QEMU smoke was run; the ARM glue still belongs to the
  other worker and the parser is not wired into boot in this worktree.

## Exact verification

1. `make build/0/test_fdt` — passed; placeholder test executable compiled
   with the repository warning and sanitizer flags.
2. `make aarch64` — passed; ARM parser compiled with
   `-mstrict-align -mgeneral-regs-only` and the ARM image linked. The existing
   linker emitted its pre-existing RWX LOAD-segment warning; the command still
   completed successfully.
3. `gcc -Iinclude -Itests -std=c11 -Wall -Wextra -Werror -Wconversion
   -fsanitize=address,undefined /tmp/w1_scratch.c kernel/core/fdt.c
   -o /tmp/w1_scratch && /tmp/w1_scratch` — passed. The scratch driver
   checked fixture status and all required values:
   cells 2/2, one RAM range `[0x40000000,0x10000000]`, no kept reservations,
   no initrd, totalsize 1048576, used 7426, fixture CRC, known CRC
   `0xCBF43926`, and copy length 65536.
4. `gcc -Iinclude -Itests -std=c11 -Wall -Wextra -Werror -Wconversion
   -fsanitize=address,undefined /tmp/w1_scratch.c kernel/core/fdt.c
   -o /tmp/w1_scratch && ASAN_OPTIONS=detect_leaks=0 /tmp/w1_scratch` —
   passed. The same scratch driver also ran every fixture truncation length
   below the live extent, a bad-magic mutation, and a 17-level hostile
   nesting blob under ASan/UBSan — all rejected without sanitizer findings.
5. `ASAN_OPTIONS=detect_leaks=0 make check` — passed. Leak scanning was
   disabled because this environment blocks LeakSanitizer `/proc` inspection;
   this is not evidence of a leak-clean run.
6. `git diff --check` — passed.

Not run: `make smoke`, `make smoke-aarch64`, remote CI, or the not-yet-landed
non-placeholder `tests/test_fdt.c`.

## Implementation notes and unresolved concern

The implementation uses fixed automatic parser state, no allocation, no
recursion, explicit zeroing instead of `memset`, checked `uint64_t` block and
range arithmetic, byte-wise big-endian reads, and a single structure-token
walk. It deliberately does not validate zero padding bytes because the frozen
design debate selected “zero padding not required.”

Concern for lead review: the frozen header says more than 16
`/reserved-memory` reservations that are fully outside RAM are tolerated, but
the parser has only `JE_FDT_MAX_RSV` fixed child-reservation scratch slots
while preserving the required post-walk ordering and arbitrary node order. A
17th fixed child reservation currently returns `JE_FDT_RANGE_LIMIT` before it
can be proven outside RAM. Memreserve entries are independently bounded by
`JE_FDT_MAX_RSVMAP`; kept reservations are bounded by `JE_FDT_MAX_RSV` as
required. This is a bounded implementation concern for owner/lead resolution,
not a hidden relaxation of the contract.

No files outside the worker-owned `kernel/core/fdt.c` and this uncommitted
`WORKER_NOTES.md` were edited. No commit, push, network access, or git-config
change was performed.
