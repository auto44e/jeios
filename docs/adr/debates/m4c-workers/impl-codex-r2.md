# M4 PR-C worker notes

Observation: 2026-10-31T11:12:30Z. Repository:
`wt-m4c-impl`; branch `wip/m4c-impl`; exact inspected base commit `dc5bf9f`
(`Merge wip/m4c-tests r1`). Changes are uncommitted. This is AI-assisted work;
no independent human review or release qualification is claimed.

State:

- Implemented: bounded M4c waits/fatal phase diagnostics, D3 grant rights
  `A=SEND|INSPECT` and `B=SEND|RECV|INSPECT`, behavioral A attenuation
  (`try_recv(A) -> H_DENIED`), complete fail-Nth baseline checks, published
  spawn rollback lifecycle completion, and exact M4c completion marker.
- Tested: the full local verification matrix below passed.
- Planned: lead integration and adversarial review.
- Blocked: no local blocker remains.

Files changed:

- `kernel/core/main.c`
- `kernel/core/sched.c`
- `kernel/core/spawn.c`
- `kernel/arch/x86_64/user_demo.S`
- `WORKER_NOTES.md`

Root cause and evidence:

1. The original silent hang was a release-signal bug: the parent stub waited
   on its own `0x40001b08 == 2`, while the kernel wrote `2` only to the child.
   The child progressed, but the parent remained in its hold.
2. A bounded diagnostic showed a progress/death observation race: progress `1`
   is written before the parent becomes DYING. The final fixture reads progress
   and death state in one IF=0 section and accepts progress `1` first.
3. A measured-spawn diagnostic showed the child record LIVE with one process
   handle reference immediately after spawn, then FREE after parent teardown.
   The parent handle-table teardown released that reference while the child
   was being reaped. The final fixture retains the measured record while
   checking LIVE/DEAD status.

Decisions:

1. RIGHTS requires INSPECT per ADR D3. The rejected SEND-only alternative
   contradicts that frozen security contract.
2. Published-thread rollback now completes LIVE -> DYING -> DEAD/FREE after
   dropping the scheduler thread and destroying the VM. Pre-publication
   failures retain unregister/destroy/abandon.
3. All M4c assembly waits are finite; kernel fixture waits have tick bounds
   and phase-bearing fatal messages. Diagnostic-only serial chatter was
   removed. `tools/smoke.py` was unchanged because its existing expected
   marker already checks the exact M4c marker.
4. The successful measured spawn is cleaned up under IF=0 after publication,
   using scheduler drop, VM destroy, and PROCESS lifecycle transitions. The
   initial spawned ring-3 child still executes startup-v1, RIGHTS,
   attenuation, send, and hold/release behavior; the measured path validates
   successful publication, status lifecycle, timing, and cleanup without a
   second scheduler/hlt race.

Commands and results:

- `make all` — PASS.
- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS: hosted tests, ELF checks,
  ABI isolation, and dev-injection isolation.
- `make smoke` — PASS: NX-present QEMU reached the exact M4c marker and
  `QEMU smoke: PASS`.
- `make integration` — PASS: fault modes 1--11 and normal mode completed.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent`
  — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent` — PASS.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present` — PASS via `make smoke`.
- `git diff --check` — PASS.

Not enforced or not claimed:

- The measured timing-only child is not dispatched; it is deterministically
  torn down after successful publication. Ring-3 startup/rights behavior is
  covered by the initial spawned child.
- No claim is made for all hardware, drivers, application ABIs, SMP, remote
  CI, or independent human review.

Next bounded task: lead integration and adversarial review. Pass criteria are
preservation of the exact M4c marker, all three x86 smoke profiles, hosted
checks, integration, rollback baselines, and documented rights/attenuation
assertions.
