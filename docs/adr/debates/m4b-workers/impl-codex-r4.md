# M4 PR-B worker notes — IMPLEMENTOR round 4

Observation time: 2026-10-31T04:28:50+00:00
Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4b-impl`
Branch: `wip/m4b-impl`
Inspected HEAD: `4edc78486b5524bea5fb745c9a40d9a3f7ffd867`
Working tree: uncommitted. No commit, push, network access, or git-config
change was performed.

## State

- Implemented: the round-4 fixes requested for M4 PR-B issue #53: D-DEV child
  quota, the child ring-3 quota probe, bounded parent progress waits and
  early-death checks, parent EXIT(0) snapshot before reaping, the PROCESS ABI
  static assertion, scratch relocation, and the `vm_s3_destroy` scheduler-link
  invariant.
- Tested: strict hosted/build checks, sanitizer-configured checks with leak
  scanning disabled, default smoke, integration fault modes, and all three
  requested x86 profiles pass.
- Planned: lead integration with parallel workers' changes, then the required
  different-lineage adversarial review and owner merge decision.
- Blocked: nothing observed in this worktree.

## Files changed

- `kernel/core/main.c`
- `kernel/core/endpoint_kern.c`
- `kernel/core/sched.c` (allowed because the requested VM-destroy invariant
  needed a scheduler accessor)
- `kernel/arch/x86_64/user_demo.S`
- `WORKER_NOTES.md`

Frozen `include/jeios/*.h` files were not edited. No unrelated files were
changed.

## Implementation and decision record

1. The generic `vm_s3_build()` remains `JE_EP_POOL_CAP` so existing M3c fixture
   processes retain their established quota. Only the M4b child is built with
   `vm_s3_build_quota(PROG_PROC_EXIT7, JE_EP_CHILD_QUOTA_MAX)`, which is the
   ADR 0010 D-DEV child limit of zero. Using the generic builder for the child
   would let the M4b child create channels and would violate the frozen quota
   boundary.

2. The child stub performs exactly one `ENDPOINT_CREATE` before `exit(7)` and
   records RAX at arena offset `0x1768`. The kernel observes the record while
   the PROCESS is DYING, before automatic reaping destroys the VM, and requires
   status `7` (`H_QUOTA`) plus unchanged endpoint-pool occupancy. The rejected
   alternative was checking after `DEAD`; that is too late because VM teardown
   makes the scratch page unavailable.

3. The two parent-progress waits now poll the parent PROCESS record as well as
   the progress word. DYING/DEAD before the expected progress is a clear fatal
   condition, and each wait has a `100000000`-iteration bound. The rejected
   alternative was an unbounded wait, which turns a parent fault or scheduler
   failure into an opaque QEMU timeout.

4. After parent progress reaches 2, the kernel polls the parent record until it
   observes DYING, snapshots its reason/code, and requires `EXIT` with code 0
   before allowing reaping. DEAD before this snapshot is rejected. This tests
   the parent's own death rather than inferring success from `used_count == 0`.

5. `endpoint_kern.c` now includes the ABI and pins the local PROCESS object
   type literal (`3u`) to `JE_ABI_OBJ_PROCESS` with `_Static_assert`. The
   frozen headers were left unchanged.

6. The parent’s 4-byte close-status store moved from `0x1714` to the verified
   free, 8-byte-aligned slot `0x1760`. The old address overlapped the adjacent
   8-byte evidence word; the kernel did not rely on the old offset, and the
   assembly comments now document both new slots.

7. `vm_s3_destroy` now rejects an index still referenced by any user scheduler
   slot before PROCESS teardown. The narrow `je_sched_has_process(vm, index)`
   accessor scans scheduler slots without mutating them. No header change was
   needed because this is a fixture-local kernel assertion used through an
   explicit declaration in `main.c`.

## Commands and observed results

Initial sandbox shell attempts failed with:
`bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted`.
The following repository reads and commands used the approved escalated
execution path. `rg` was unavailable (`rg: command not found`); `grep` and
`sed` were used instead.

Preparation:

- Read `AGENTS.md`, `README.md`, `docs/HANDOFF.md`, `docs/ROADMAP.md`,
  `docs/DESIGN.md`, `docs/AGENT_START.md`, `docs/CODING_STANDARD.md`,
  ADR 0010, `/tmp/m4b/map.md`, and both round-3 review files:
  `/tmp/m4b/rev3-impl-claude.md` and `/tmp/m4b/rev3-tests-codex.md`.
- Inspected the frozen HEAD versions of `include/jeios/proc.h`,
  `include/jeios/abi.h`, `include/jeios/handle.h`, and
  `include/jeios/endpoint.h` with `git show HEAD -- ...`.
- `git diff --check`: PASS.
- `make all`: PASS; output was `Nothing to be done for 'all'`.

Verification:

- `make check`: PASS. Strict freestanding C11 compilation used
  `-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes`,
  baseline `-march=x86-64`; all hosted tests, ELF checks, ABI isolation and
  dev-inject isolation passed.
- `ASAN_OPTIONS=detect_leaks=0 make check`: PASS. Leak scanning was disabled
  because of the managed environment; all hosted tests and ELF/ABI checks
  passed.
- `make smoke`: PASS. Observed the M3c-s7 marker, both M4b markers, and
  `JeiOS: initialization complete; halted`.
- `make integration`: PASS. Fault modes 1 through 11 all reported PASS,
  including the full normal M4b path in the relevant mode.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx
  --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent`: PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present`: PASS, including both M4b markers.

The QEMU harness terminated each emulator after the expected terminal marker;
no fatal diagnostic occurred in the positive smoke profiles.

## Not verified / unresolved

- No remote CI result was observed.
- No independent human review or hardened/release qualification is claimed;
  this is AI-assisted implementation evidence.
- ARM execution was not run for this x86-only change.
- The lead must rerun the integrated tree after incorporating parallel worker
  changes and update the shared handoff/status records.
- No claim is made for SMP, all hardware, all drivers, application ABIs, or
  hardened isolation.

Next bounded task: lead integration and different-lineage adversarial review.
Pass criteria: the integrated hosted suites, smoke, integration, all required
x86 profiles, and documentation/evidence review remain green without weakening
the frozen ADR 0010 contracts.
