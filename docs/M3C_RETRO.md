# M3c Retrospective & Qualification Evidence

## 1. Summary

M3c (bounded capability IPC, waits, and minimal two-process init) is complete and
merged; issue [#8](https://github.com/auto44e/jeios/issues/8) is closed. The
tested, merged baseline is `main` at `bac8c3b` (after the ADR 0000 retroactive
sweep, PR #50; the last M3c *code* landed with the step-7 bootstrap fixture,
PR #48).

The milestone delivered, over the frozen ADR 0009 contract (D1–D9, D-SEQ,
D-OWNER-1/2, amendments A1–A6 + A6-CLARIFY-1/2):

- **endpoints** as single pool records holding both ends with a **split refcount**
  (handle refs vs inflight refs), kernel-only creation, and genuine destruction +
  reclamation on process death;
- **non-blocking data IPC** (`try_send`/`try_recv`), copy-ordered and data-only
  (D6/D9);
- **capability transfer** — transactional, attenuated, destination-side mint with
  rollback, and the D9 cycle-reclamation invariant (cycles allowed, released on
  last-handle removal);
- **blocking receive** (`try_recv` with a timeout) with the single-terminal-CAS
  wait state machine (WOKEN / PEER_CLOSED / TIMED_OUT / CANCELLED), the constrained
  restartable-syscall park/resume, and one idempotent reservation-release owner;
- a **bootstrap fixture**: the kernel seeds an endpoint pair into two ring-3
  processes that run a real bounded protocol end-to-end.

## 2. What shipped, by issue/PR

M3c was decomposed into the frozen D-SEQ (ADR 0009) and shipped as owner-gated
stacked PRs, each built to CI-green and merged by the owner (never self-merged):

| D-SEQ step | PR(s) | What |
| --- | --- | --- |
| ADR 0009 (contract) | #40 | D1–D9, D-SEQ, D-OWNER-1/2 frozen up front |
| 2. lifecycle | #41 | `endpoint.c`/`endpoint.h`: pure/hostable channel, split refcount, retain-before-commit dup, endpoint rights in the ABI |
| 3. death release walk (owner-gated) | #42 | process-death release walk via a generic `je_vm` teardown hook in an IRQ critical section; typed close dispatch; ring-3 `endpoint_create` |
| 4. non-blocking data IPC | #43 | `try_send`/`try_recv`, Model A, D6 copy ordering, data-only (D9) |
| A5 amendment + ADR 0000 | #44 | step-5 contract + the decision-record policy |
| 5. capability transfer | #45 | transactional attenuated mint, dead-cap/cycle-reclaim, R8/R9 ABI |
| A6 + CLARIFY-1/2 + ADR 0000 part-4 | #46, (CLARIFY in #47) | step-6 wait contract + the attach-correspondence policy |
| 6. waits (owner-gated, `sched.c`) | #47 | `THREAD_BLOCKED_IPC`, the arbitrate CAS, the restartable-syscall park/resume, the release owner |
| 7. bootstrap fixture | #48 | two ring-3 processes over a seeded channel; discharges the step-5/6 ring-3 deferrals |
| retroactive decision-record sweep | #50 | ADR 0000 3-part back-fill for ADRs 0001–0008 |

(The MP0 mobile ADRs and MP1 ARM64 entry/console/vectors — PRs #49 et al. — ran
alongside on the second-architecture track; see the ARM64 ledger entries, not
part of #8.)

## 3. Qualification evidence (exact, on `bac8c3b`)

Honest status: this is **experimental single-CPU capability IPC in qualified
emulator profiles**, proven by test. It is **not** a hardened/release
qualification — that remains an independent human gate (ADR 0007/0008 discipline),
and all review in this milestone was **AI-assisted, not independent human
qualification**.

### 3.1 `make check` (hosted unit tests) — PASS
Endpoint suite (create, split-ref close/destruction, dup acquire-before-commit,
REFOVERFLOW, DENIED/WRONGTYPE, inflight keep-alive, pool-exhaustion + create-unwind,
release-walk, data IPC send/recv/FIFO/QUEUE_FULL/SHORT_BUFFER/PEER_CLOSED/rights/
zero-len, cap-transfer mint/dead-cap/cycle-reclaim, waiter register/arbitrate-CAS/
reserved-head/release/release-by-id/peer-close-EMPTY) + the syscall decode matrix
(incl. the R10-timeout cases) all pass under the hosted ASan/UBSan build.

### 3.2 `make integration` (QEMU fault modes) — PASS
No regression across the M3a/M3b fault-mode matrix.

### 3.3 `make smoke` — boot markers (verbatim, the M3c self-tests)
- `M3c-s3a` / `M3c-s3b` — kernel survivor-peer + ring-3 fault-death reclaim.
- `M3c-s4` — ring-3 try_send/try_recv loopback (direction, SHORT_BUFFER, copy-out
  fault leaves msg queued → delivered once, QUEUE_FULL, data-only reject,
  drain-then-PEER_CLOSED); channel reclaimed on exit.
- `M3c-s5` — cross-table capability transfer via the kernel forwarders.
- `M3c-s6` — blocking recv **TIMED_OUT end-to-end** through the real scheduler +
  ring 3 (thread parks; timer ISR stamps the terminal transition; continuation
  resumes with status 14; waiter released; channel reclaimed).
- `M3c-s7` — the **two-process bootstrap fixture**: B parks → A's cap-carrying
  send WOKE it via the continuation; B minted the transferred end; A closed c1
  BEFORE B used the mint (retained-ownership proof); 4-round ping-pong; DONE
  teardown; both exit 0; pool reclaimed to 0.

### 3.4 CI smoke profiles
3 NX profiles (`-nx absent`, `-nx absent --memory 16M`, `--memory 1536M
--expect-nx present`) + the ARM64 build all pass; remote CI (x86 bootstrap + ARM
compile) green on every merged PR. Honest scope: the ARM QEMU boot (vectors
self-test) is verified **locally**, not yet wired into remote CI.

## 4. What went well (retro)

- **Freezing the full D-SEQ contract up front (ADR 0009) paid off repeatedly.**
  Each step implemented against a frozen target; the amendments (A3/A5/A6) extended
  it with owner sign-off rather than drifting.
- **The owner-gate + stacked-PR rhythm held perfectly** — every PR built to
  CI-green, got an honest "PR ready" with verified evidence, and was merged by the
  owner. Never self-merged; never claimed remote-green from a local run.
- **The ordering choice (transfer before waits, waits before bootstrap) was the
  key enabler.** Blocking recv (step 6) is precisely what made the two-process
  ping-pong deterministic in step 7 — a parked receiver yields instead of
  spinning, dissolving the single-CPU cooperative-scheduler deadlock that had
  forced the step-5 and step-6 ring-3 deferrals. The deferrals were honest, and
  they were discharged for real once the enabler existed.
- **Adversarial peer review earned its keep on every hard change.** It caught 6
  contract-text defects in the A5 amendment pre-code, 10 in the A6 text, 8 real
  code defects in the step-6 diff (including an endpoint-wedging SHORT_BUFFER bug
  and a process-death-doesn't-wake-peer bug on paths the smoke test never reached),
  and in step 7 the strengthened assertions it demanded **caught a real exit-opcode
  bug** that had silently passed as green.
- **The decision-record policy (ADR 0000) + attached transcripts** turned the
  session's reasoning into durable, auditable record instead of ephemeral context.

## 5. What was hard / lessons

- **Single-CPU cooperative scheduling is a sharp constraint for multi-process
  proofs.** Two spin-based ring-3 stubs deadlock. The lesson — proven twice — is to
  make the receiver *block* (yield) rather than spin, and to drive handoffs with
  deliberate yields + reaper/timer stepping, never a race.
- **A passing end-to-end test can mask defects on the paths it doesn't take.** The
  step-6 TIMED_OUT smoke was green while SHORT_BUFFER-on-resume, peer-death-wake,
  and `drop_user` waiter-leak were all broken. Adversarial review of the *diff* (not
  just the test) found them. In step 7, strengthening the fixture's assertions
  (assert recv length, close results, clean-exit reason) converted a false green
  into a caught bug.
- **The restartable-syscall mechanism needed care to stay A6-5-compliant** — a
  plain restart re-resolves the handle (forbidden); the constrained form finishes
  from the owned reference. Choosing it over a heavier kernel-continuation kept the
  `sched.c` blast radius minimal, which is the whole point of the owner gate.
- **Honesty about proof scope is a recurring discipline, not a one-time note.** The
  ring-3 two-process deferrals, "AI-assisted not human-qualified," local-vs-remote
  CI, and "transcript not retained" in the sweep all required saying exactly what
  was and wasn't proven.

## 6. Honest limits carried forward

- Single CPU, `JE_VM_MAX_PROCESSES=2`, first-GiB allocator, no SMP/MMU-growth.
- `CANCELLED` is internal-only (no ABI trigger until a future signal mechanism).
- The restartable-syscall frame deviation (a returning handler must preserve
  x16-x18 on ARM; the x86 restart rests on immutable user text + one-thread-per-
  process — re-gate at mmap/threads).
- AI-assisted review only; independent human qualification for any
  hardened/release claim is still owed.

## 7. Conclusion

M3c closes the bounded-capability-IPC gate: two real processes can create, hand
off, block on, and tear down endpoints, with destruction and reclamation proven and
the authority model (typed rights, attenuation, owned references, generation
safety) carried intact from M3b. The next core gate beyond M3 and the ARM64 MP
track continue from the HANDOFF.
