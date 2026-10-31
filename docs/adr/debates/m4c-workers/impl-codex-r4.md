# M4 PR-C worker notes

Observation: 2026-10-31T12:24:10Z. Repository/worktree: `wt-m4c-impl`;
branch `wip/m4c-impl`; exact inspected base commit
`5e5f6fac97b2b9a37d7a3c04df832222a6dbdb5d`. Changes are uncommitted. This
is AI-assisted implementation evidence, not independent human review, remote
CI, hardened qualification, release qualification, or a merge claim.

State:

- Implemented: M4c QEMU fixture fixes for ADR 0010 D5/D6 review findings,
  plus a kernel-internal failpoint fired-counter accessor.
- Tested: final strict build, sanitizer-disabled hosted checks, normal smoke,
  integration, legacy-NX profiles, 16 MiB profile, and five consecutive
  NX-present 1536 MiB smoke runs.
- Planned: lead integration and adversarial review of this uncommitted diff.
- Blocked: no local blocker.

Files changed:

- `kernel/core/main.c`
- `kernel/arch/x86_64/user_demo.S`
- `kernel/core/spawn.c`
- `tools/smoke.py`
- `WORKER_NOTES.md`

Frozen headers under `include/jeios/` were not edited. No other worker files,
Git configuration, commit, push, or network action was changed.

Implementation:

1. The parent fixture now uses explicit phase/ack handshakes. Phase 6 parks
   before all ring-3 negative spawns; the kernel reads that phase, takes the
   complete baseline, and acknowledges under one IF=0 section. Phase 7 parks
   after those calls; the kernel compares the same baseline under IF=0 before
   acknowledging. Valid grants are restored before phase 9, the
   quota-invalid call. Phase 9/10 similarly brackets that call and arms
   journal step 1 before the ring-3 call. Phase 8 is a separate parent hold
   loop; the kernel waits for `phase == 8` and `hold == 1`, takes the fail-Nth
   baseline under IF=0, and keeps the saved IRQ state through the matrix and
   direct-spawn probes. This removes the previous progress/baseline TOCTOU.
2. Baselines include free pages, endpoint pool occupancy, PROCESS records,
   parent live handles, user threads, live VM slots, and both
   `handle_refs`/`inflight_refs` values for each grant endpoint. The endpoint
   fields are read only from the parent-owned endpoint handles while IF=0;
   those source handles keep the endpoint ends alive.
3. The parent and child assert startup-v1 zero-fill before using the startup
   values. They cover RAX, RBX, RBP, R10-R15; the parent additionally covers
   RCX, R8 and R9 for n=1, while the child checks R8/R9 for n=2. Existing
   version, count, nonzero-handle, order, and attenuation checks remain.
   Child acknowledgement is now only written after the child reports its
   first progress; the child's zeroing of the shared acknowledgement word was
   removed.
4. Every negative spawn checks both status and `RDX == 0`. The fixture covers
   bad image, missing TRANSFER, rights superset, count five, endpoint quota
   one, SELF as a wrong-type launcher, a closed DUP as STALE, and zero as
   BADINDEX. The valid grant descriptors are restored before the quota probe.
   A launcher without SPAWN is unreachable from ring 3: LAUNCHER is only
   kernel-seeded in this fixture, has no ring-3 creation/attenuation path, and
   is not DUP-able. Launcher DUP/RIGHTS denial and QUERY type 4 remain tested.
5. `je_spawn_failpoint_fired()` was added in `spawn.c`. Each valid arm resets
   the counter, a hit increments it, and an invalid/disarming arm clears both
   the hook and counter. The quota-invalid probe arms step 1 and requires the
   counter to remain zero, proving `n > quota` is rejected before the journal.
   Each fail-Nth case requires exactly one fired event. Step 3 with grant index
   2 (greater than n=2) and step 9 are armed before real spawns; both must
   succeed, proving unfired arms cannot poison a later call.
6. The returned PROCESS handle is checked from ring 0 for exactly
   `INSPECT | KILL`. Direct successful spawns are torn down while IF=0 with
   `drop_user -> vm_destroy -> on_dying(KILLED) -> on_reaped`, then a raw
   handle close is required to return OK and one PROCESS release removes its
   remaining record reference. The earlier retain-plus-double-release
   alternative was rejected because generic `je_handle_close` does not perform
   typed PROCESS release and would obscure the single-release proof.
7. `CLOCK_TICKS` still checks monotonicity and now has a ring-3 nonzero-argument
   call requiring `INVALID` and zero `RDX`.
8. The direct spawn diagnostic now brackets the call with LFENCE/RDTSC and
   reports `spawn cycles=`. It is explicitly a cycle diagnostic, not a timing
   bound and not a replacement for the PIT tick ABI. RDTSCP was tried and
   rejected after QEMU's `qemu64` profile raised #UD; RDTSC-only fencing is
   supported by the final profile.
9. The smoke harness marker was updated from `spawn ticks=` to the honest
   `spawn cycles=` marker and the new baseline/validation-order wording.

Verification commands and results:

- `git diff --check` — PASS.
- `make all` — PASS on the final worktree; strict C11 kernel build/link with
  `-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes`.
- `ASAN_OPTIONS=detect_leaks=0 make check` — PASS on the final worktree:
  hosted core, boot, allocator, VM, uframe, syscall, handle, endpoint,
  process, FDT, ELF, ABI-isolation, and dev-injection-isolation checks.
  Leak detection was disabled as requested and documented by the repository
  for this environment.
- `make smoke` — PASS on the final worktree. The marker was:
  `M4c-s1: spawn/LAUNCHER/startup-v1/CLOCK_TICKS OK; negative baselines OK;
  quota validation order OK; fail-Nth rollback OK (steps 1-5); spawn
  cycles=...`.
- `make integration` — PASS for fault modes 1 through 11 on the exact final
  worktree, including normal NX execution, fault-local D7, and
  exception-fixup recovery.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx
  --expect-nx absent` — PASS on the final rebuilt ISO.
- `python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M
  --expect-nx absent` — PASS on the final rebuilt ISO.
- `python3 tools/smoke.py build/0/jeios.iso --memory 1536M
  --expect-nx present` — PASS in the final five-run flake sweep; all five
  consecutive invocations passed.
- An earlier `make smoke` failed at the temporary unfired-spawn baseline
  diagnostic because the fixture retained the PROCESS record and then
  released only once. Removing that extra retain fixed the accounting.
- A subsequent smoke attempt raised kernel #UD at the temporary RDTSCP timing
  instruction. Replacing RDTSCP with fenced RDTSC fixed it; all later smoke
  and profile runs passed.

Not verified or not claimed:

- No remote CI, ARM execution, physical hardware, SMP, general application
  ABI, all-driver support, hardened profile, release qualification, or
  independent human review is claimed.
- The failpoint remains compiled into the normal kernel because the QEMU
  fixture is its evidence path. It is kernel-internal and not ABI-reachable.
- `docs/HANDOFF.md`, `IMPLEMENTATION.md`, and other documentation were not
  edited because this worker was explicitly restricted to the listed files
  plus `WORKER_NOTES.md`; the lead must carry integrated-head status into the
  handoff.

Next bounded task:

- Lead integrates this uncommitted diff and runs the full final suite at the
  integrated head.
- Pass criteria: frozen headers remain unchanged, the final adversarial review
  accepts the synchronization and negative-case proofs, all requested local
  checks remain green, and the lead records local versus remote evidence
  separately.
