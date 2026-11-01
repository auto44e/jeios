# PR-G FIX pass — worker notes

Observation time: `2026-11-01T08:04:50Z`. Repository: `jeios`; branch:
`wip/m4g-impl`; exact inspected commit:
`f704111f1ba17b6a0e75da19621993ab588b466b`. The changes are intentionally
uncommitted for lead integration.

State: **implemented / tested / planned / not blocked**. Remote CI and
independent human qualification were not observed. AI-assisted review is not
human qualification.

## Decisions and reasoning

- The broker remains ring-3 supervisor policy. No broker status or service
  identity was added to the kernel. The frozen `PROC_STATUS` ABI remains
  unchanged: successful status returns RDX = 0.
- `SERVICE_BIND` and `BROKER_GRANT` are now 12-byte records with a
  supervisor-minted transaction id. ACK detail must equal that transaction;
  stale or mismatched ACKs are consumed and discarded until the bounded
  deadline.
- Capability delivery remains service-first, client-second. Source handles close
  only after the corresponding receiver ACK. Failed delivery leaves a pending
  ledger entry when the service may already have minted its end, so both-dead
  collection can reclaim it safely.
- The ledger is keyed by the supervisor-owned client and service PROCESS handle
  values, service id, and service generation. It never uses a VM slot or a
  supervisor-reported identity. Collection requires exact DEAD observations for
  both processes; an uninspectable stale status handle clears the bounded entry
  so it cannot pin the table forever. A referenced PROCESS handle remains open
  until teardown or after its ledger entries are collected, preventing
  handle-value reuse while the key is live.
- Lifetime exhaustion and duplicate/PENDING checks happen before any service
  restart side effect. A generation-wrap restart is refused terminally.
- The kernel cap-delivery audit is recorded only after
  `je_endpoint_kern_recv_commit` succeeds. Failed later-cap mint or user-copy
  paths therefore cannot inflate the proof. Mode-13-only hooks are compiled
  out of normal IPC builds.
- Malformed or unexpected supervisor control messages are dropped or close the
  offending channel as appropriate; they do not call `failed()`. Control
  lengths and cap counts are exact per operation, and the client/supervisor
  waits use clock ticks with the client grant deadline at least twice the ACK
  deadline.
- Kernel corroboration requires exactly three client-side and three service-side
  brokered deliveries, the A/B/A endpoint sequence, live service state for the
  service-side proof, exact brokered rights, the observed third request with no
  third A delivery, and the supervisor endpoint-charge peak at or below
  `JE_SVC_EP_LIMIT`. It uses the kernel child ledger and endpoint inspection,
  never a supervisor-reported process identity.
- A and B report exercise completion over their control channels before the
  supervisor faults service generation 1. This makes the echo proof causal:
  the deliberate restart cannot interrupt B's first brokered round trip.
- Kernel corroboration records cap mints at the existing receive/mint boundary
  and snapshots client endpoint tables before reaping. Report fields and client
  claims are diagnostics/evidence only, not success authority.
- The fixed bounds remain explicit: `JE_BROKER_MAX=6`, endpoint pool capacity
  `24`, and supervisor endpoint limit `21`. The observed mode-13 peak has three
  broker channels plus three control channels and the private sleep pair
  (`7` supervisor-created channels), below both limits; teardown observes
  endpoint allocation and charge return to zero.
- M5 gap retained honestly: A's `EXHAUSTED` request occurs while a live
  generation-2 entry also exists. This proves the lifetime-check ordering but
  does not prove the duplicate-free cap in this scenario; a host unit test is
  still needed. No stronger claim is made.

## Verification record

Commands run locally (exact commands; results are recorded only when observed):

- `ASAN_OPTIONS=detect_leaks=0 make check TEST_MODE=13` — PASS.
- `ASAN_OPTIONS=detect_leaks=0 make check TEST_MODE=0` — PASS.
- `make smoke` — PASS. Mode 0 reached
  `M4f-s1`, zero-leak baseline, and `QEMU smoke: PASS`.
- `make integration` — PASS. Modes 1–12 retained their existing results;
  mode 13 reached `M4g-s1`,
  `M4g: client-reported EXHAUSTED; kernel delivery count A=2 corroborated`,
  and zero-leak baseline with no `M4g: supervisor lost` marker.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx
  present` — PASS.
- `make aarch64 && make smoke-aarch64 ARM_REQUIRE=1` — PASS. The existing
  ARM linker RWX warning remained; `QEMU aarch64 smoke: PASS`.
- `git diff --check` — PASS.

An earlier exploratory `make smoke TEST_MODE=13` returned the smoke wrapper's
default-mode marker mismatch because that target invokes `tools/smoke.py`
without a mode-13 expectation. It was not used as mode-13 evidence; the
mode-13 ISO was rebuilt and passed through `make integration` and the direct
mode-13 QEMU boot.

Remote CI and independent human qualification are not observed.

## Known dead ends

- Deferring all mode-13 monitor reaping stalled progress in QEMU. The final
  design keeps the shared reaper and captures client handle evidence before
  reaping; this preserves normal scheduler progress and still makes the
  proof kernel-owned.
- Triggering service death immediately after both grant ACKs allowed B's echo
  to race the deliberate fault. The explicit exercise-complete control record
  closes that race without weakening the bounded scenario.

## PR-G identity re-key fix — 2026-11-01

- Observation: `2026-11-01T10:33:58Z`; repository `jeios`; branch
  `wip/m4g-impl`; exact inspected commit
  `f704111f1ba17b6a0e75da19621993ab588b466b`. Changes remain uncommitted.
- Decision: revert the unapproved additive `PROC_STATUS` identity result and
  key the broker ledger on the stable PROCESS handles already owned by the
  supervisor. This preserves the frozen D4 ABI and avoids exporting record
  identity solely for this scenario.
- Ownership invariant: a PROCESS handle named by any live or PENDING ledger
  entry is not closed or reused while that entry exists. The restart path keeps
  the dead service PROCESS handle open across service-generation replacement;
  teardown closes the remaining supervisor-owned handles.
- Alternatives rejected: retaining `(index,generation)` would require an ABI
  extension; closing the old service handle at restart would permit handle-value
  reuse while ledger entries still reference it.
- Verification: `ASAN_OPTIONS=detect_leaks=0 make check` — PASS;
  `ASAN_OPTIONS=detect_leaks=0 make check TEST_MODE=13` — PASS, including
  `test_syscall` proc-status decode; `make smoke` — PASS; `make integration` —
  PASS for modes 1–13, including `M4g: client-reported EXHAUSTED; kernel
  delivery count A=2 corroborated`, `M4g-s1`, and zero-leak; the 16 MiB
  no-NX and 1536 MiB NX direct smoke profiles — PASS; `make aarch64 && make
  smoke-aarch64 ARM_REQUIRE=1` — PASS; `git diff --check` — PASS.

## Historical records preserved from the pre-PR-G file

# PR-F Stage 1 worker notes

## PR-F Stage 2 FIX worker notes — observation 2026-11-01T05:32:14+00:00

Repository `auto44e/jeios`; branch `wip/m4f-fix`; inspected commit
`6520f1515d06a83ba6fcf400e029a8a21e616b0a`; no commit created. The tree is
intentionally dirty for lead integration.

### State

State: **implemented / tested / planned / not blocked**. The success
attestation now uses kernel-observed child identity, death records, endpoint
shape, exact exit evidence, and live survivor/restart records; the report is
diagnostic only. The monitor reaps during the run, has a wrap-safe watchdog,
and performs a second supervisor identity scan before the reaper can erase the
supervisor's terminal record. Mode 12 remains a refusal path. Remote CI and
independent human qualification were not observed.

### Decisions and implementation record

- Added a bounded kernel-owned side ledger keyed by PROCESS identity. Spawn
  records the image id only after the transaction succeeds; terminal evidence
  is captured before monitor reaping, and ledger entries are cleared after
  reap while aggregate per-image death history remains available for
  corroboration. This avoids changing the frozen `je_proc_rec` layout.
- Reaped children in the monitor's interrupt-disabled scan section, before
  `sti; hlt`. This preserves the monitor as the sole reaper and leaves VM,
  PROCESS, and endpoint capacity available for the ARM rollback launch.
- Rewrote success corroboration to ignore report counters for the security
  decision. It requires the exact image population and per-image outcomes:
  one live restart-service, one live survivor with dispatches, one EXIT
  spoofer with the exact complement code and one SEND-only endpoint with zero
  charge, three image-6 FAULT terminals, the exact supervisor EXIT code, and
  the exact step-4 spawn failpoint observation.
- Kept the rollback launch after `je_proc_alloc`/`je_vm_build` and asserted
  VM headroom before arming. The supervisor performs exactly one ARM spawn and
  requires `H_NOSLOTS` with handle zero.
- Added wrap-safe monitor and ARM deadlines. A deadline is a liveness
  fail-stop; it is reset only by an observed phase or kernel-progress change.
- Hardened ring-3 behavior: survivor peer closure is fatal, the spoofer stale
  probe is exact and singular, the supervisor checks self handle
  `0x00010001`, and report publication is a complete odd/body/barrier/phase/
  barrier/even snapshot.
- Added `JE_SVC_DONE_EXIT` and require it in the supervisor death record.
  Mode 12 now yields once after publishing READY so the kernel must observe
  READY before the deliberate fault; the marker prints both last phase and
  reason.

Alternatives rejected: a `mark_dying` hook or DYING-only scan would violate
the frozen D12 identity-scan policy; report counters cannot serve as proof
because they are ring-3 writable diagnostics; changing `je_proc_rec` would
ripple into frozen process tests; leaving children unreaped would make the
rollback capacity check vacuous.

Negative coverage deliberately remains explicit: this pass does not add
separate forged-counter, torn-report, or impostor-child injection fixtures.
The implementation makes counters non-authoritative and performs seqlock and
image-ledger checks, but those adversarial injections are **not** claimed as
end-to-end tested.

### Exact verification observed

- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS. Hosted suites and
  validators completed, including `test_sched_killall: PASS (66 checks)`,
  `test_image: PASS (88 checks)`, and `test_mkimage: PASS (562 checks)`.
- `make smoke` — PASS. The rebuilt NX image printed
  `M4f-s1: supervisor scenario completed`,
  `M4f: zero-leak identity baseline verified`, and `QEMU smoke: PASS`.
- `make aarch64 && make smoke-aarch64 ARM_REQUIRE=1` — PASS.
  `QEMU aarch64 smoke: PASS`; the existing RWX linker warning remained.
- `make integration` — PASS. Modes 1–11 passed; mode 12 printed
  `M4f: supervisor phase=0x0000000000000001`, then
  `M4f: supervisor lost (last_phase=0x0000000000000001; reason=0x0000000000000000); no recovery path in this profile`,
  `M4f: zero-leak identity baseline verified`, and no scenario-completed
  marker.

### Next bounded task

Lead integration and review should inspect the complete dirty diff, rerun the
same local gates after integration, and obtain independent human review before
any hardened or release qualification claim. No PR, issue, or remote state was
changed by this worker.

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
