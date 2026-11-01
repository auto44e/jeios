# PR-F Stage 1 worker notes

## PR-F Stage 2 worker notes — observation 2026-11-01T03:06:15Z

Repository `auto44e/jeios`; branch `wip/m4f-impl`; inspected commit
`5ffd26c854a3603de838452c79e81d7996dbd7ff`; no commit created.

### Decisions and debugging record

- Used the frozen D12 identity scan. The supervisor id is retained as a plain
  `{index, generation}` value; the monitor calls `je_proc_status` before any
  reap and treats every non-LIVE state as loss unless a stable DONE report plus
  kernel corroboration proves the normal path.
- Selected a kernel-written `JE_SVC_LOSS_GO` word at the fixed report-page
  address for mode 12. This keeps crt0’s unused GPRs zero and makes the
  deliberate fault a data-plane test rather than an ABI change.
- The first boot exposed a real recycled-page-table defect: the backend
  assumed `je_pages_alloc` zeroed reused frames, and a stale PT entry made the
  first child build return `JE_ERR_INVALID`. `vb_link_tables` now clears all
  four table pages with bounded loops before publishing the new topology.
- The first supervisor run stalled after publishing ARM because the go word
  symbol was at `0x4001003c`, while the frozen contract is `0x40010040`.
  Increasing the linker padding from 20 to 24 bytes fixed the placement.
- The first success teardown was rejected because the report stored the
  spoofer’s complement-encoded low 32 bits instead of its low mask, and the
  kernel compared the full low 32 bits. The report now stores the low 16-bit
  mask; the kernel checks the same field and separately validates the
  complement.
- The corroboration fault scan initially made the FAULT count unreachable
  behind an EXIT-only guard. It now counts DYING/FAULT records before
  validating the EXIT spoofer record.
- The baseline is an ownership predicate, not a count comparison: it snapshots
  every allocator page-state word, endpoint quiescence, all PROCESS record
  identity fields, and all VM state/root identities. Free baseline process
  slots require FREE after teardown with nondecreasing generation and no
  residual link/ref/charge/supervisor state.

### Exact verification observed

`make smoke` — PASS. The rebuilt NX image printed:

```text
M4f-s1: supervisor scenario completed
M4f: zero-leak identity baseline verified
QEMU smoke: PASS
```

`make iso TEST_MODE=12` followed by direct QEMU boot — PASS. The terminal output
included:

```text
M4f: supervisor lost (reason=0x0000000000000000); no recovery path in this profile
M4f: zero-leak identity baseline verified
```

`ASAN_OPTIONS=detect_leaks=0 make check` — PASS. Hosted/sanitizer tests,
ELF/image validation, `test_sched_killall: PASS (66 checks)`, and
`test_mkimage: PASS (562 checks)` completed successfully.

`make integration` — PASS. Modes 1–9, fault-local mode 10, exception-fixup
mode 11, and supervisor-loss mode 12 completed successfully.

CI-equivalent smoke profiles — PASS:

```text
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent
python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx present
```

Each printed `QEMU smoke: PASS`; the no-NX profiles retained the legacy
terminal and the NX profile printed the M4f success and zero-leak markers.

`git diff --check` — PASS after the final code and documentation edits.

### State and next task

State: **implemented / tested / planned / not blocked**. Implemented files
include the service images, Makefile/image registration, supervisor monitor,
baseline predicate, page-table reuse fix, smoke/integration harnesses, and
documentation updates. Remote CI and independent human qualification remain
unobserved; no hardened or release qualification claim is made.

Observation time: `2026-10-31T23:05:35+00:00`  
Repository: `auto44e/jeios`  
Branch: `wip/m4f-impl`  
Inspected commit: `b3ae3b7b136346290bda91baadc4909ad203b3fb`  
Commit status: no commit created; the tree is intentionally left dirty for lead integration.

Final verification observation: `2026-10-31T23:06:38+00:00`; the same
uncommitted tree passed the final `make check`, `make smoke` and
`git diff --check` runs below.

## Decision and implementation

- `je_sched_kill_all_user` uses a validation pass followed by a mutation pass.
  This was chosen because the contract requires a fail-stop error with no
  changes when any user slot is inconsistent. A one-pass implementation could
  kill earlier children before discovering a later bad slot.
- The supervisor is spared only by exact `(index, gen)` identity. The
  implementation validates its run link and process state before skipping it,
  so a malformed supervisor slot cannot hide an invariant failure.
- LIVE READY and BLOCKED_IPC users are targets. READY plus `wait_pending` is
  intentionally not special-cased: the existing unchanged `mark_dying`
  function is the single idempotent waiter/reservation release owner.
- RUNNING, SLEEPING and ZOMBIE user slots, generation-zero identities and
  run-link mismatches return `JE_ERR_INVALID`. IF-set and non-kernel-current
  calls retain the existing `abort_scheduler` fail-stop behavior required by
  the public contract.
- The report reader is marked `__attribute__((unused))` because Stage 1 must
  not add supervisor/monitor glue. It probes `read_user_pte` before any raw
  dereference, uses the existing CR3-switch pattern, reads seq/body/seq, and
  restores CR3 before validation and every return.
- The image predicate is also marked unused and has only a commented
  `JE_IMAGE_SUPERVISOR` hook. Calling it for current images would reject them,
  so the hook remains inert until Stage 2 adds the supervisor image.

## Alternatives rejected

- A `mark_dying` supervisor-loss hook was not added. ADR 0010 D12's
  owner-approved 2026-10-31 note freezes identity scanning because forced
  teardown also reaches `mark_dying`; a hook would create false loss events.
- A DYING-only scan was not used. The frozen note requires any status other
  than LIVE to count as loss because slot 0 is the sole reaper and must scan
  before reaping.
- The existing `je_copy_from_user` path was not used for the report. It
  requires a current user thread, while slot 0 is kernel context. The reader
  therefore follows the required PTE-probe plus `vm_s3_scratch` CR3 pattern.
- A new scheduler mark-dying helper was not factored out. Keeping the tested
  `mark_dying` body byte-identical reduced risk and directly satisfies the
  frozen behavior requirement.

## Hosted-test workflow

`sched.c` is not independently linkable as a small host library because most
of its functions refer to architecture and kernel-only entry points. The
hosted test includes the production scheduler translation unit with private
linkage exposed only in that test translation unit, uses linker section GC to
discard unrelated scheduler entry points, links production `kernel/core/proc.c`,
and supplies minimal arch/waiter fakes. It does not model kill-all separately.

The test uses `CHECK`, not `assert`, and survives `-DNDEBUG`. It verifies:

- two live non-supervisor users are killed and counted while the supervisor
  remains LIVE/READY;
- pre-existing DYING users are skipped and not counted;
- READY plus `wait_pending` is killed and releases through the waiter owner;
- RUNNING, SLEEPING and ZOMBIE states, and generation zero, return
  `JE_ERR_INVALID` with byte-for-byte scheduler/PROCESS state unchanged.

## Exact verification evidence

Command:

```text
make build/0/test_sched_killall
```

Observed:

```text
gcc ... tests/test_sched_killall.c kernel/core/proc.c ... -o build/0/test_sched_killall
```

Command:

```text
ASAN_OPTIONS=detect_leaks=0 ./build/0/test_sched_killall
```

Observed:

```text
test_sched_killall: PASS (66 checks)
```

Command:

```text
ASAN_OPTIONS=detect_leaks=0 make check
```

Observed result: exit `0`; all hosted tests passed, including:

```text
test_sched_killall: PASS (66 checks)
ELF checks: PASS (AMD64, Multiboot2, entry, segments, no undefined symbols)
test_mkimage: PASS (562 checks, rules E1-E10 + CLI + output)
```

Command:

```text
make smoke
```

Observed result: exit `0`; the command rebuilt `build/0/jeios.iso`, QEMU
reached the existing `M4e-s1` marker, and ended with:

```text
JeiOS: initialization complete; halted
QEMU smoke: PASS
```

Command:

```text
git diff --check
```

Observed result: exit `0`.

## State and next bounded task

State: **implemented / tested / planned / not blocked**.

Implemented: Stage 1 D12 kill-all mechanism, validated report reader hook,
supervisor image placement predicate, hosted coverage and test wiring.

Tested: local hosted checks and NX QEMU smoke above.

Planned: Stage 2 supervisor image and services, monitor identity scan,
report-based clean-DONE distinction, kill-all/reap/baseline refusal path, and
TEST_MODE 12.

Not enforced yet: no supervisor image exists; no report is read at boot; no
slot-0 identity scan, kill-all/reap integration, baseline audit, refusal marker
or supervisor-loss QEMU scenario exists. No remote CI or human qualification
is claimed.
