# Fix-gate review evidence — 2026-11-06 UTC

Baseline: `e4bd9c24d69de4fc6798d733b1f2e6398522748f`.
This records hosted observations, not native/hardware qualification.

## Baseline commands

- `make check`: exit 2; LeakSanitizer could not inspect process data.
- `ASAN_OPTIONS=detect_leaks=0 make check`: exit 0; ASan/UBSan retained.
- `make mutants`: exit 0; runner reported 6 killed, 0 equivalent, 0 survived,
  0 blocked, 4 skipped. This is the current runner's report, subject to F01.
- A leftover handle-attenuation mutation was observed during final worktree
  inspection and restored to the exact baseline. Its cause was not established;
  do not use this observation alone as proof of a particular cleanup failure.
  No production-source changes are included in the response PR.
- Docker and QEMU executables were unavailable; neither was run locally.

## ARM table probes

Copy `tests/test_paging_bits.c` to a temporary C file and insert one of the
following blocks immediately before `test_enable_gating();` in its main function.
Use a separate copy/executable for each probe.

Self-cycle (observed INVALID, process exit 0):

```c
uint64_t *rp = (uint64_t *)(uintptr_t)je_arm_mmu_root(&je_arm_mmu_context);
uint64_t old = rp[0];
rp[0] = je_arm_mmu_root(&je_arm_mmu_context) | 3u;
assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_ERR_INVALID);
rp[0] = old;
```

Foreign late sibling (expected INVALID; observed ASan SEGV, exit 1):

```c
uint64_t *rp = (uint64_t *)(uintptr_t)je_arm_mmu_root(&je_arm_mmu_context);
rp[511] = UINT64_C(0x30000000) | 3u;
assert(je_arm_mmu_audit(&je_arm_mmu_context, good, NGOOD) == JE_ERR_INVALID);
rp[511] = 0;
```

Compile/run each temporary file, replacing PROBE with its path:

```sh
gcc -Iinclude -DJE_ARCH_AARCH64 -DJE_ARM_PAGING_HOSTED_TEST \
  -Ikernel/arch/aarch64 -std=c11 -O1 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  PROBE.c kernel/arch/aarch64/paging.c kernel/core/alloc.c \
  kernel/core/boot.c -o PROBE
ASAN_OPTIONS=detect_leaks=0 timeout 10 ./PROBE
```

The foreign-sibling trace enters `audit_alias_scan` at paging.c:456 via its
recursive call at 466 and `audit_table` at 544. The earlier valid leaf triggers
alias traversal before the outer walk validates the later sibling pointer.
Both walkers have a level bound; this is not unbounded recursive depth.

## False mutation kill

Run from the repository root; all writes remain in a temporary directory:

```python
import importlib.util
import pathlib
import tempfile
spec = importlib.util.spec_from_file_location("mutants", "tools/mutants.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
with tempfile.TemporaryDirectory() as directory:
    m.ROOT = directory
    root = pathlib.Path(directory)
    (root / "source.c").write_text("ORIGINAL")
    test = root / "always-fails"
    test.write_text("#!/bin/sh\nexit 1\n")
    test.chmod(0o700)
    print(m.run([str(test)], cwd=directory).returncode)
    print(m.apply_one({"file": "source.c", "find": "ORIGINAL",
                       "replace": "MUTATED", "run": "always-fails"}))
    print((root / "source.c").read_text())
```

Observed: `1`, `('KILLED', '')`, `ORIGINAL`. The test fails before any mutation,
yet the runner reports a kill. A pristine baseline preflight is required.
