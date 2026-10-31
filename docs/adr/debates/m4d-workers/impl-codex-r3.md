# M4 PR-D worker notes

Observation time: 2026-10-31T15:26:47+00:00

Repository: `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4d-impl`

Branch: `wip/m4d-impl`

Inspected commit: `ce6bd8cbf723eaf851cde76b239fba3e3ccbacde`

## State

- Implemented: M4d synchronous PROC_KILL fixture coordination now uses a
  local non-reaping interrupt wait. The fixture retains the READY freeze and
  dispatch checks, supervisor guard, BLOCKED_IPC sentinel/waiter/ref checks
  before reap, WOKEN counter check, killer no-reschedule check, EXIT
  first-reason check, DYING re-kill check, and synchronous-return checks.
- Tested: the final ISO passed the complete requested local verification
  matrix listed below.
- Planned: lead integration and repository-level HANDOFF/status refresh.
- Blocked: no required local verification remains blocked. ARM execution,
  remote CI, and independent human review were not performed.

## Changed files and contracts

- `kernel/core/main.c`: added `m4d_wait_interrupt()`, which executes
  `irq_save(); sti; hlt` and does not reap. Reaping remains explicit through
  `je_kernel_idle_step()` only after the coordinator has completed the
  victim checks under IF=0. Replaced every M4d coordinator `pause` poll with
  this wait. Made the phase controls distinct so the killer cannot consume a
  coordinator-visible phase before its checks run.
- `kernel/arch/x86_64/user_demo.S`: aligned the killer's phase gates with the
  coordinator's distinct controls: phase 3 for the blocked-victim checkpoint,
  phase 4 for the parked WOKEN-victim checkpoint, and phase 5 for the EXIT
  setup checkpoint.
- `tools/smoke.py`: retained the exact expanded M4d success marker.
- `kernel/core/endpoint_kern.c` and `kernel/core/sched.c`: these contain the
  existing round-2 changes from the worktree. No round-3 changes were needed
  there after investigation; `mark_dying` was not modified.
- Frozen `include/jeios/*.h` headers were not edited.

## Investigation of the old `pause` stall

Conclusion: **not determined**.

The generated `build/0/kernel/core/main.o` was disassembled. Each old
coordinator poll called `vm_s3_scratch` (including its CR3 switch), returned
through the outer `je_arch_irq_restore`, and then executed `pause`.
`vm_s3_scratch` itself saves and restores flags. The source and generated loop
show a possible IF-preservation interaction, but do not prove that the helper
entered with IF=0 or that the `irq_save`/`irq_restore` pairing kept IF=0
across the poll.

Temporary diagnostics observed IF set, four M4d user threads READY, PIC mask
`0xfe`, and `je_sched_interrupt` preempting bootstrap slot 0 from RUNNING to
READY. Those facts establish that interrupts and scheduler preemption were
active, but they do not identify the exact cause of the old `pause` stall.
Therefore the root cause is recorded as not determined rather than attributed
to CR3 switching or interrupt masking without proof.

## Round-3 failures found and corrected

After replacing the polls with the requested wait, the first bounded QEMU
trace reported `M4d blocked kill timeout`. The killer had reused control 2
from the preceding READY phase, so it could advance through the blocked
checkpoint before the coordinator observed it. The blocked gate now uses
control 3.

The next bounded trace reported `JeiOS fatal: M4d WOKEN victim did not park`.
The killer had similarly reused control 3 for the WOKEN gate and could pass
through it before the coordinator observed it. The WOKEN parked gate now uses
control 4, and the later EXIT setup gate uses control 5.

These were fixture handshake races, not evidence of a scheduler or endpoint
kernel defect. No additional endpoint or scheduler fix was made in round 3.

## Decisions

1. Use `irq_save(); sti; hlt` for coordinator progress waits because the
   request requires one interrupt without reaping. `je_kernel_idle_step()` is
   reserved for the explicit post-check reap points.
2. Keep the phase controls distinct instead of adding more polling or moving
   the checks. This preserves the intended ordering and makes each
   coordinator observation stable before the killer advances.
3. Record the old-poll root cause as not determined. The disassembly and
   diagnostics constrain the possibilities but do not establish a single
   causal instruction sequence.
4. Do not alter `mark_dying` or frozen headers. The observed failures were
   reproducible in fixture control flow and were fixed at the permitted
   coordinator/assembly boundary.

Rejected alternatives:

- Keeping `pause` and adding a larger tick bound was rejected because it
  would not address the observed lack of progress and would weaken the
  bounded fixture evidence.
- Calling `je_kernel_idle_step()` from the new wait was rejected because it
  would reap while the killer was mid-phase, violating the M4d check order.
- Changing scheduler or endpoint code was rejected because the final traces
  identified transient fixture gates, and no kernel defect was demonstrated.

## Verification

All commands below were run locally. No network, remote CI, ARM execution, or
independent human review was performed.

- `git diff --check` — PASS.
- `make -B all` — PASS with the repository's strict C11/freestanding build.
- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS; hosted core/boot/alloc/vm/
  uframe/syscall/handle/endpoint/proc/proc_retire/fdt tests, ELF checks, ABI
  isolation, and dev-inject isolation passed.
- `make smoke` — PASS; rebuilt `build/0/jeios.iso`, emitted the exact M4d
  success marker, reached `JeiOS: initialization complete; halted`, and
  reported `QEMU smoke: PASS`.
- `make integration` — PASS; modes 1 through 11 passed.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present` — PASS.
- Five consecutive
  `python3 tools/smoke.py build/0/jeios.iso --expect-nx present` runs after
  the final `make smoke` — PASS, all five emitted the M4d marker and
  `QEMU smoke: PASS`.

Diagnostic commands/results retained for reasoning:

- A bounded 8-second QEMU run with `-d int` after the first wait replacement
  exited by timeout without an M4d marker; the serial trace showed the
  blocked phase timeout.
- A bounded 24-second QEMU run with `-d int` after the first phase fix exited
  by timeout with `M4d WOKEN victim did not park`; user RIP mapped to the
  killer's WOKEN gate. These traces led to the distinct controls 3, 4, and 5.

## Handoff

The permitted implementation files are ready for lead integration. The lead
should preserve the exact phase ordering and rerun the stated commands after
integration. Repository-level `docs/HANDOFF.md` was not edited because the
worker file scope was restricted to the listed implementation files plus this
uncommitted `WORKER_NOTES.md`.
