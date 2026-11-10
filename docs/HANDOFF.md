# Agent handoff: MP3c ARM capability IPC

## Current status — MP3c G1/G2/G3 tightening fix — observed 2026-11-10T12:58:14Z

Repository `/tmp/wt-mp3c`, branch `wip/mp3c`, exact inspected base commit
`a5b4914a2fa3fba17de26f4f4623d0f792cf6283` (the requested fix-pass changes
were uncommitted at this observation). State:
**implemented / tested locally / planned (owner review, remote CI, physical ARM
and independent human qualification) / not blocked**. No push, PR publication,
reviewer assignment or release was performed. The requested
`docs/adr/debates/0017-mp3c-arm-ipc/06-claude-confirm-review.md` is absent from
this worktree; the available ADR records and the task-provided findings were
used. Demonstrated support tier remains QEMU 8.2.2 `virt-8.2`, `cortex-a72`,
one CPU, 256 MiB. This is AI-assisted local evidence, not independent human or
release qualification.

### Implemented

- G1: the F1-relaxed death/reuse oracle now accepts a still-allocated endpoint
  record only when the non-victim end is `PEER_CLOSED`, the generation is
  unchanged, the victim has no waiter in any state, and both ends have zero
  in-flight references. The existing release-counter, no-stale-id, and
  no-pending-continuation guards remain. The kernel D9 release ordering was
  not changed.
- G1 injection: `ARM-MP3C-WOKEN-WAITER-LEAK` skips the real endpoint
  in-flight-reference decrement while clearing the WOKEN waiter slot. The
  oracle rejects that record at `death-release`; the mutant is enabled and
  killed through the real ARM path.
- F1 race tolerance remains: the forced CPU-spin race still accepts the benign
  `rec_freed=0` outcome. The new timer rearm is retained because removing it
  prevents the TIMER_PREEMPT proof from firing.
- F2 WAITERS_BUSY: the canonical baseline marker is
  `FATAL sched IPC WAITERS_BUSY guard`; removing the endpoint guard reaches the
  distinct `FATAL sched IPC WAITERS_BUSY guard missing` outcome.
- G2: hosted endpoint case 30 asserts that a send cannot overwrite a
  `TIMED_OUT` waiter; `EP-TIMED-OUT-SEND-TERMINAL` pins that reverse direction
  and is killed by the hosted test.
- F3/F4: the flaky or vacuous standalone timed-out-late-send,
  woken-then-deadline, and attenuation-bypass stages remain removed. Their
  remaining evidence is hosted endpoint coverage plus the load-bearing ARM
  mutants; no standalone stage claim remains.
- F5: the strict MP3a-2 TIMER_PREEMPT predicate was restored: the switch must
  be from the first worker to the peer and the saved ELR must lie inside the
  two-instruction CPU-only spin. The EL0 fixture rearms the timer after setup
  so the first measured IRQ cannot preempt outside that spin.
- G3: the honest resumed-receive limits list was restored in this handoff and
  the implementation/ADR ledgers. It does not claim the mint rollback,
  PEER_CLOSED release, short-buffer release, poisoned continuation,
  reap/sweep-order, or two caps-bearing copy-fault cleanup branches.
- NITs: the dead `JE_G_FAULT_WAITERS_BUSY` hook and unused
  `EL0_IPC_LATE_SEND` constant were removed. The attenuation killer now names
  `register-round-trip`, and the PEER_CLOSED-wake killer names
  `reap-mismatch`. The shared body still contains `JE_TEST_MODE==13/15`
  hooks and per-architecture handle macros, so this is partial convergence,
  not full source purity.

### Evidence observed

```text
make check                                                        PASS
make smoke                                                        PASS
make -B aarch64 ARM_EL0_TEST=1                                  PASS
make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1 \
  ARM_SMOKE_TIMEOUT=70                                            PASS (6/6)
make smoke-aarch64 ARM_EL0_TEST=1 ARM_EL0_DEATH_RACE=1 \
  ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=70                              PASS (6/6)
python3 tools/mutants.py --id ARM-MP3C-WOKEN-WAITER-LEAK \
  --timeout 300                                                    KILL (3/3)
python3 tools/mutants.py --id ARM-MP3C-WAITER-RELEASE-DEATH \
  --timeout 300                                                    KILL
python3 tools/mutants.py --id EP-TIMED-OUT-SEND-TERMINAL \
  --timeout 300                                                    KILL
```

The forced F1 race repeatedly emitted
`DEATH_RELEASED_WAITER rec_freed=0 no_stale_id=1` and passed. Reverting the
waiter-release gate was killed by the same D9 proof; the WOKEN leak was also
killed in 3/3 repeated runs and emitted the expected `death-release` fatal.
The retained ARM IPC matrix (`drop-the-rewind`, `wrong-resume-assertion`,
`re-resolve-on-resume`, `commit-mismatch`, `copy-fault-resume`,
`short-buffer-resume`, `waiters-busy`, `idle-no-panic`) booted 8/8 to its exact
named fatal detector. These expected-fatal smoke commands return make exit 2
because the harness reports a caught injected failure as
`FATAL as expected`; that is a passing negative result. The WAITERS_BUSY
guard-removal injection reached the distinct `guard missing` fatal.

`ARM_TIMER_TEST=1` and `ARM_SCHED_TEST=1` both passed their non-EL0 smoke
profiles. All 13 non-EL0 scheduler-mutation profiles reached their expected
named detectors. The x86 comparison built current and archived-base
`build/<mode>/kernel/core/syscall.o` for TEST_MODE 0..15, extracted `.text`
with `objcopy`, and `cmp` found all 16 byte-identical to base `52adf4f`.
The full hosted check also included the reverse TIMED_OUT late-send assertion.

The required solo `make mutants` result was:
**77 killed, 1 equivalent, 0 SURVIVED, 0 BLOCKED, 2 skipped**.

### Honest limits

- The standalone F3/F4 stages are intentionally absent; they are not claimed
  as deterministic proofs. The hosted terminal-transition test and the
  attenuation/deadline mutants remain the available coverage.
- The resumed positive fixture does not individually exercise these
  resumed-receive branches: mint rollback after a later cap failure,
  PEER_CLOSED release, short-buffer release after a nonfatal return, poisoned
  continuation/identity rejection, and reap/sweep ordering beyond the
  exercised waiter/reuse path.
- It also does not individually exercise the two caps-bearing cleanup branches
  in `kernel/core/syscall_glue.inc`: byte-copy fault cleanup at lines 79-83
  and handle-array copy-fault cleanup at lines 90-94. Those branches remain
  honest limits; the current copy-fault mutants and hosted coverage do not
  prove them.
- One-thread-per-process remains a fixture/model boundary and G6 remains
  mutation-only; this pass does not claim a user-created multi-thread race or
  a separate kernel poison profile.
- The shared syscall glue is converged but not pure: its test-mode 13/15 hooks
  and per-architecture handle accessors remain.
- All results are local host/QEMU evidence. Remote CI, physical ARM, SMP,
  arbitrary hardware support, and independent human qualification remain
  unverified. No broad hardware or application-ABI claim is made.

### Next bounded task and pass criteria

Commit this reviewable fix pass to `wip/mp3c`, then perform owner review. Pass
requires the evidence above to remain reproducible from the committed tree,
with `git diff --check` clean and no regression of B1/B2/B3/E1. Remote CI,
physical ARM, SMP, and independent human qualification remain separate gates.

## Current status — MP3c B1/B2/B3 and vacuity fix pass — observed 2026-11-10T04:35:56Z

Repository `/tmp/wt-mp3c`, branch `wip/mp3c`, exact inspected parent commit
`fa7d1d63c6db97b1c72561d09ee425c6bad19636`, implementation commit
`589447da065a0c135ef94b474fc0eaaf6ee162b0`; this entry is the required
post-commit documentation refresh. State: **implemented / tested locally /
planned (owner review, remote CI, physical ARM and independent human
qualification) / not blocked**. No push, PR publication, reviewer assignment
or release was performed. The named adversarial review
`docs/adr/debates/0017-mp3c-arm-ipc/03-claude-impl-review.md` is absent from
this worktree; the requested findings were applied from the task specification
and the available ADR review records. Demonstrated platform remains QEMU 8.2.2
`virt-8.2`, `cortex-a72`, one CPU, 256 MiB.

### Implemented

- B1: EL0 slot 0 now uses a separately allocated, mapped, aligned kernel stack
  below an unmapped guard; the idle frame points into it. A5 canaries surround
  the EL0 scheduler state and occupy the low idle-stack zone. The late
  `IDLE_STACK_GUARD bss_overwrites=0 stack_guard=1 resumed=2` assertion checks
  the real resumed identity, frame generation, pending continuation state and
  canaries. Allocation failure frees the run before the fatal map error.
- B2: the EL0-only idle-frame install and EL0 idle bookkeeping are gated out of
  non-EL0 builds. The dead non-EL0 idle-frame object was removed so `-Werror`
  preserves the baseline profiles.
- B3: B is bound before the live-root-core fixture reads its slot; the
  unmutated profile now reaches `FATAL sched copy live root`. The lone-survivor
  witness uses a bounded, load-bearing survivor spin that is redirected only
  after the negative stages; the READY-before-pick mutant is enabled again.
- MP3a-2 gates are strict: exactly one real `TIMER_PREEMPT from=1 to=2`,
  exactly one independent `TIMER_WAKE`, and exactly one real
  `LONE_IRQ_RESELECT`; no timer/SVC substitutes are accepted.
- Vacuity fixes: all-blocked idle is emitted only after both real user slots
  were BLOCKED, slot 0 ran, and a user wake resumed; WOKEN_RESTART requires
  the parked/woken/restarted state; TIMER_WAKE is emitted by the deadline
  wake path independently of TIMED_OUT. The no-kill EL0 profile proves the
  resumed context and idle guards without claiming the default profile’s
  teardown/reuse marker.
- Negative guards: drop-rewind, commit-mismatch, resumed copy-fault and
  waiter-busy now observe the actual violated predicate; short-buffer is
  targeted at a resumed receive and accepts only a trusted short-buffer result
  with a real wake/restart witness. The shared glue case shape was not
  refactored; only these test-gated guard conditions were tightened.
- The dead-cap mutant now targets the real endpoint descriptor-liveness
  predicate rather than a redundant glue branch. The positive caps-bearing
  receive therefore proves the endpoint predicate by observing a live sibling
  and a NULL dead-cap result.
- Re-enabled mutants: `ARM-USER-LONE-SURVIVOR-READY-BEFORE-PICK`,
  `ARM-MP3B-LIVE-ROOT-CORE`, and `ARM-MP3C-TWO-PHASE-COMMIT`; stale waiter
  release killer text was corrected.

### Evidence observed so far

```text
make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=70       PASS
make smoke-aarch64 ARM_EL0_TEST=1 ARM_EL0_NO_WAKE_KILL=1 \
  ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=35                                       PASS
make smoke-aarch64 ARM_TIMER_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60     PASS
make smoke-aarch64 ARM_SCHED_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60     PASS
make aarch64 ARM_EL0_TEST=1                                           PASS
make check                                                            PASS
```

The no-kill EL0 run also passed and emitted the genuine
`TIMER_PREEMPT`, `WOKEN_RESTART`, `DEADCAP_NULL_MARKER`, independent
`TIMER_WAKE`, late `ALL_BLOCKED_IDLE`, zero-overwrite
`IDLE_STACK_GUARD`, `TIMED_OUT`, `PEER_CLOSED`, and `LONE_IRQ_RESELECT`
markers; it did not emit `sched frame invalid`. All 13 scheduler mutation
profiles reached their named detectors. The IPC negative matrix reached its
named detector for all 11 profiles, including drop-the-rewind,
commit-mismatch, copy-fault-resume, short-buffer-resume, woken-then-deadline,
timed-out-late-send, waiter-busy and idle-no-panic.
`ARM_COPY_FAULT=live-root-core` reached `FATAL sched copy live root`.
The final solo mutation sweep completed with **75 killed, 1 equivalent,
0 survived, 0 blocked, 2 skipped**.

### Unresolved / honesty boundaries

- The no-kill profile intentionally does not claim `DEATH_RELEASED_WAITER`;
  the default kill-and-slot-reuse profile owns that lifecycle proof.
- The resumed fixture covers the caps-bearing receive, the endpoint
  dead-cap-liveness predicate, and the resumed copy-fault release path. The
  following resumed branches remain individually uncovered: mint rollback
  after a later cap failure, short-buffer release after a non-fatal return,
  PEER_CLOSED release, one-thread-per-process enforcement, continuation
  identity under a poisoned proc id, and reap/sweep ordering beyond the
  exercised waiter/reuse checks.
- `can_wake` considers pending IPC deadlines and does not inspect `SLEEPING`;
  ARM EL0 has no sleep syscall, so this is latent coverage rather than a
  claimed sleep guarantee.
- The `post_svc_elr >= ARENA_BASE + 4` check is defensive and does not prevent
  a valid block-SVC at `ARENA_BASE` from rewinding to `ARENA_BASE`.
- x86 whole-object evidence must record that `syscall.o` is unchanged while
  `endpoint_kern.o` is +32 bytes from the waiter-release counter; the linked
  x86 text therefore differs by 32 bytes. No x86 identity claim is made here.
- All evidence is local QEMU/host evidence. Remote CI, physical ARM, SMP,
  arbitrary hardware support and independent human qualification remain
  unverified. AI-assisted review is not human qualification.

### Next bounded task and pass criteria

Commit this reviewable fix pass to `wip/mp3c`, then have the lead re-run the
local gates and perform owner review. Pass requires the recorded zero
SURVIVED/zero BLOCKED mutation result, clean formatting, all named negative
detectors, the two non-EL0 profiles and the genuine EL0 proof. Remote CI,
physical ARM, SMP, and independent human qualification remain separate gates.

## Current status — MP3c completion pass — observed 2026-11-10T00:23:07Z

Repository `/tmp/wt-mp3c`, branch `wip/mp3c`, exact inspected parent commit
`a8f95e99d5f17446b587ad36c6bae6e1b9221f48`; completion-pass changes were
uncommitted at this observation. State: **G1-G4 implemented and locally tested;
G5 implemented except the timed-out-late-send proof remains a dropped claim; G6
mutation-only; G7 implemented with IRQ-only reaping / no SVC fallback; final
mutation result pending at this observation; owner review, remote CI, physical
ARM and independent human qualification planned**. No push, PR publication,
reviewer assignment or release was performed. Demonstrated platform remains
QEMU 8.2.2 `virt-8.2`, `cortex-a72`, one CPU, 256 MiB.

### Implemented and trusted markers

- G1 PEER_CLOSED: the real blocked receiver is woken by endpoint peer-close
  arbitration only after the endpoint is empty. Marker:
  `EL0 IPC PEER_CLOSED trusted-cause blocked-before-ready=1`; the callback
  checks endpoint state and pre-wake BLOCKED state.
- G2 dead-cap NULL: the real message carries two cap descriptors, the source
  handle for one descriptor is closed after enqueue, and the receiver observes
  a live sibling plus handle `0` for the dead descriptor. The live sibling is
  used for a real reply before close. Marker:
  `EL0 IPC DEADCAP_NULL_MARKER slot=<n> live_sibling=ok`.
- G3 death release / slot reuse: the fixture parks a real receiver, creates a
  WOKEN reservation, kills it through the real lifecycle path (there is no ARM
  PROC_KILL), reaps on the IRQ path, and creates a replacement in the same VM
  index. Marker:
  `EL0 IPC DEATH_RELEASED_WAITER rec_freed=1 no_stale_id=1`; it checks record
  FREE, generation change, no stale waiter id, waiter-release progress, and no
  pending continuation before reuse.
- G4 all-blocked idle: both user slots enter real timed receives, slot 0 is
  reached and WFI executes after ERET, then the timer wakes a user. Marker:
  `EL0 IPC ALL_BLOCKED_IDLE woke=1`. Fault 11 removes every deadline in the
  genuine all-blocked/no-wake state and reaches deterministic
  `FATAL sched nothing can wake`.
- G5 short-buffer has the trusted marker
  `EL0 IPC SHORT_BUFFER trusted-cause required=1 caps=0` and ends at
  `FATAL el0-test short-buffer resume`. Drop-rewind, wrong-resume-assertion,
  attenuation-bypass, commit-mismatch, resumed-copy-fault,
  woken-then-deadline, and WAITERS_BUSY retain their named fatal checks.
  The timed-out-late-send profile currently detects a second waiter still
  present and ends at `FATAL el0-test late-send ordering`; it does not prove
  a user send after a fully released waiter, so that stronger claim is dropped.
- G6 remains mutation-only. One thread per process prevents the requested user
  race. Fault 4 injects the re-resolve mutation and reaches
  `FATAL sched IPC re-resolve`, but no kernel-side poisoned-handle /
  different-stale-answer profile was completed. This is not claimed as an
  A6-5 proof.
- G7 is resolved in favor of the ADR: peer-death reaping is IRQ-path
  authoritative and the bounded SVC fallback was removed. The positive G1-G4
  fixture and existing EL0 proof pass with IRQ-only reaping.

### Evidence and exact commands

Observed in this completion pass:

```text
make aarch64 ARM_EL0_TEST=1                         PASS
make smoke-aarch64 ARM_EL0_TEST=1 \
  ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60                 PASS
```

The positive serial evidence includes exactly one trusted marker for each of
`WOKEN_RESTART`, `TIMED_OUT`, `TIMER_WAKE`, `PEER_CLOSED`,
`DEADCAP_NULL_MARKER`, `ALL_BLOCKED_IDLE`, and
`DEATH_RELEASED_WAITER`, followed by
`EL0 proof PASS reviewed-cache-tlb=not-observed`.

The named negative matrix was built and booted through
`make aarch64 ARM_IPC_FAULT=<name>` plus
`make smoke-aarch64 ARM_IPC_FAULT=<name> ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60`.
All profiles except the deliberately dropped late-send claim reached their
named detector; the late-send ordering control reached its explicit
`FATAL el0-test late-send ordering`.

The x86 preservation check rebuilt `kernel/core/syscall.o` for TEST_MODE 0
through 15 and compared raw `.text` bytes against the exact parent commit
`a8f95e9`; all 16 comparisons passed. Direct hashes between modes 11, 13 and
15 differ for their pre-existing test-only code, so that is not the identity
claim being made.

### Changed invariants / unresolved hazards

The ARM fixture now uses two RX and two RW pages, and the ARM usercopy fixup
range covers that complete fixture. ARM lifecycle owns waiter release;
architecture code clears the continuation before RR exit; the IRQ reaper
performs record teardown and generation advancement before slot reuse. Remaining
hazards are the dropped late-send ordering proof, mutation-only G6 re-resolve
poison coverage, QEMU's lack of stale-cache/TLB modeling, physical ARM/SMP
absence, remote CI absence, and the required independent human qualification.
No claim is made for arbitrary hardware or ABIs.

### Next bounded task and pass criteria

Review the final mutation result and this handoff on `wip/mp3c`; if the owner
requires exact G5 late-send or G6 A6-5 completion, add a separate bounded,
owner-gated change. Pass for this completion handoff is: zero SURVIVED and
zero BLOCKED mutants, `make check`, the exact positive ARM smoke, the named
negative matrix, x86 parent-identity, `git diff --check`, and a local commit.
Remote CI, physical ARM, and independent human review remain unverified.

## Current status — MP3c implementation pass — observed 2026-11-09T23:19:19Z

Repository `/tmp/wt-mp3c`, branch `wip/mp3c`, exact inspected commit
`b1845a517b51dfc8947fc52d7ece585938129d02` (committed locally; working tree
clean at observation). State: **implemented in part / tested locally in part / planned
(remaining MP3c proof stages, owner review, remote CI, physical ARM and
independent human qualification) / blocked for the requested completion gate**.
No push, PR publication, reviewer assignment or release was performed.
Demonstrated platform remains QEMU 8.2.2 `virt-8.2`, `cortex-a72`, one CPU,
256 MiB.

### Implemented

- Added the frame-neutral syscall fragment `kernel/core/syscall_glue.inc` and
  the x86-preserving include `kernel/core/syscall_glue_x86.inc`. ARM uses the
  same endpoint/handle/send/receive case bodies with argument/status/result,
  return, and park accessors; ARM PROCESS/DEVICE authority remains
  `UNSUPPORTED`. The ARM dispatcher now decodes once and has a distinct
  `JE_SYSCALL_BLOCK = 0x100` action.
- Appended `JE_RR_BLOCKED` and added `je_rr_block()` /
  `je_rr_wake()`; hosted tests cover exclusion, rejected switch, idempotent
  wake, blocked death, and rejection of wake on SLEEPING.
- Added the ARM continuation record, ELR rewind before parking, saved owned
  endpoint record/side and arguments, process identity check, post-SVC ELR
  assertion, shared resumed receive finish, and immediate return after
  restart.
- Added a real slot-0 idle frame below the live boot stack, WFI only after
  ERET, ARM IRQ deadline sweep, endpoint bridge installation, ARM DAIF and
  scheduler shims, and fail-closed ARM device hooks.
- Split lifecycle waiter release from device hooks so blocked endpoint waiters
  are released on the normal death path.
- Extended the EL0 fixture with seeded bootstrap-channel routing, cap transfer,
  destination-side cap use, WOKEN restart, post-commit EMPTY, and timer
  TIMED_OUT checks. The current proof is trusted-kernel-marker based, not a
  user-reported success bit.

### Tested evidence

Observed locally at this worktree state:

```text
make check                                      PASS
make aarch64 ARM_EL0_TEST=1                    PASS
make smoke-aarch64 ARM_EL0_TEST=1 \
  ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60            PASS
make mutants                                   PASS
python3 -m json.tool tools/mutants.json         PASS
python3 -m py_compile tools/smoke_arm.py \
  tools/run_arm_ipc_fault.py                    PASS
git diff --check                                PASS
```

The successful ARM serial tail includes:

```text
JeiOS/aarch64: EL0 IPC WOKEN_RESTART trusted-cause
JeiOS/aarch64: EL0 IPC TIMED_OUT trusted-cause
JeiOS/aarch64: EL0 IPC TIMER_WAKE trusted-cause
JeiOS/aarch64: EL0 LONE_IRQ_RESELECT slot=1
JeiOS/aarch64: EL0 proof PASS reviewed-cache-tlb=not-observed
QEMU aarch64 smoke: PASS
```

The current mutation result is `69 killed, 1 equivalent, 0 SURVIVED,
0 BLOCKED, 4 skipped`; the skipped entries are the pre-existing endpoint
inflight gap plus the three explicitly disabled MP3c coverage gaps. The
earlier MP3b mutation result must not be treated as current MP3c evidence.

### Not yet verified / blockers

- The requested x86 `objdump` byte-identity matrix is verified for TEST_MODE
  0 through 15 at this source state: normalized disassembly and `.text` bytes
  of `syscall.o` are identical. A separately linked whole-ELF comparison was
  not performed.
- `drop-the-rewind`, `wrong-resume-assertion`, `attenuation-bypass`,
  `commit-mismatch`, resumed-copy-fault, woken-then-deadline, and WAITERS_BUSY
  profiles have reached FATAL behavior in local runs. The short-buffer profile
  currently reaches the generic EL0 proof failure rather than a dedicated
  trusted SHORT_BUFFER marker.
- `re-resolve-on-resume` is mutation-only: one thread per process prevents the
  user race, and the current profile reaches proof completion rather than a
  real failing mutation. It is not claimed as proven.
- `timed-out-late-send` currently reaches the generic proof failure rather
  than the intended endpoint late-send FATAL; its ordering is not deterministic
  after the timed-out waiter is fully released.
- `idle-no-panic` currently reaches proof completion and times out rather than
  the intended no-wake FATAL control. The positive fixture does not yet force
  both user slots into BLOCKED simultaneously.
- The positive fixture does not yet provide the complete requested PEER_CLOSED,
  dead-cap NULL-marker, death-releases-waiter/slot-reuse, and all-blocked idle
  stages. These are implementation/proof gaps, not passing tests.
- The IRQ path is authoritative for peer-death reaping, but a bounded SVC
  fallback remains in `je_arm_el0_svc` to handle the synchronous death handoff
  observed in the existing EL0 proof. This is a documented deviation from the
  strict IRQ-only wording and must be resolved or owner-approved before exact
  ADR 0017 completion.

### Next bounded task and pass criteria

Finish the missing ARM fixture stages and deterministic fault profiles, remove
or explicitly approve the SVC reaping fallback, rerun x86 TEST_MODE 0..15
byte identity, then run:

```text
make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60
make smoke-aarch64 ARM_IPC_FAULT=<each profile>
make mutants
```

Pass requires every requested positive stage to complete from trusted kernel
state, every enabled negative to produce its named FATAL/behavior, zero
SURVIVED/BLOCKED mutants, current x86 byte identity, `make check`, and
`git diff --check`. Remote CI, physical ARM, and independent human review
remain unverified.

## Current status — MP3b fix pass — observed 2026-11-09T08:04:29Z

Repository `/tmp/wt-mp3b`, branch `wip/mp3b`, exact inspected commit
`5b338b7b89f787365ddff37f1969ae2d8416925d` (the exact inspected pre-fix-pass
base; this working tree contains the fix pass). State: **implemented / tested
locally / planned (owner review, remote CI, physical ARM and independent human
qualification) / not blocked**.
No PR, push, reviewer assignment or release was performed. Demonstrated support tier:
QEMU 8.2.2 `virt-8.2`, `cortex-a72`, one CPU, 256 MiB.

### Implemented

- Added `kernel/arch/aarch64/usercopy_arm.S` with separate `LDTRB/STRB`
  from-user and `LDRB/STTRB` to-user leaves. The leaves return 0/1, use only
  x0-x3 and x30, have no stack frame and no x16-x18 use, and expose the exact
  labels `je_copy_user_load_site`, `je_copy_user_store_site`, and
  `je_copy_user_fixup`.
- Extended the portable usercopy scope/prevalidation path for ARM, including
  the live TTBR0 root equality check and the exact ARM predicate:
  vector 4, EL1h, EC 0x25, IL 1, ISS 0x007/0x047 only, exact ELR site,
  half-open FAR span, active current owner, and live-root equality.
- Added the tri-valued vector guard and dedicated 304-byte copy restore tail.
  The copy path requires guard state 2, rewrites ELR to the fixup, restores
  guard state 1, and never calls `je_arm_restore_selected`.
- Added the ARM current-user shim and separate ARM-local syscall glue for
  `COPY_PROBE` plus SELF-only handle mint/close/query/rights/dup operations.
  Wrong object types fail closed; x86 `syscall.c` and x86 usercopy behavior
  remain outside the ARM path.
- Extended the EL0 proof with exact byte round-trips for lengths 0, 256 and
  255 (including page-crossing spans), prevalidation-versus-fixup counting,
  SELF attenuation/query/rights/close/stale-close, and guard-state checking.
  Added fourteen isolated fatal-negative copy-fault profiles, including
  save-path, live-root-core, load-permission, guard-2, far-lower, and
  prefilter-stray proofs, plus ARM MP3b mutant anchors for the new load-bearing
  guards.

Changed files:

`docs/HANDOFF.md`; `docs/IMPLEMENTATION.md`;
`docs/adr/0016-mp3b-aarch64-usercopy-and-handles.md`;
`include/jeios/{syscall.h,usercopy.h}`; `kernel/core/usercopy.c`;
`kernel/arch/aarch64/{el0_test.c,usercopy_arm.S,vectors.S}`;
`mk/aarch64.mk`; `tools/{check_arm_usercopy_boundary.sh,mutants.json,run_arm_copy_fault.py,smoke_arm.py}`.

New proof markers and labels:

```text
je_copy_user_load_site
je_copy_user_store_site
je_copy_user_fixup
JeiOS/aarch64: EL0 COPY_PROBE_PASS slot=<N> lengths=0,256,255
JeiOS/aarch64: EL0 HANDLE_ROUND_TRIP_PASS slot=<N> type=SELF attenuated=0 stale=3
JeiOS/aarch64: EL0 COPY_FIXUP_RECOVERED slot=<N> count=2 guard=1
JeiOS/aarch64: FATAL copy fault fixup rejected
JeiOS/aarch64: FATAL el0-test copy-guard-cleared
```

### Tested

Observed locally:

```text
make aarch64 ARM_EL0_TEST=1                                      PASS
make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60 PASS
make smoke-aarch64 ARM_COPY_FAULT=<each of 14 profiles>          PASS
make check                                                        PASS
make mutants                                                       PASS (65 killed, 1 equivalent, 0 survived, 0 blocked, 1 skipped)
sh tools/check_arm_usercopy_boundary.sh                           PASS
python3 -m json.tool tools/mutants.json                           PASS
python3 -m py_compile tools/smoke_arm.py tools/run_arm_copy_fault.py PASS
git diff --check                                                  PASS
```

The positive boot tail included:

```text
JeiOS/aarch64: EL0 COPY_PROBE_PASS slot=1 lengths=0,256,255
JeiOS/aarch64: EL0 COPY_FIXUP_RECOVERED slot=1 count=2 guard=1
JeiOS/aarch64: EL0 HANDLE_ROUND_TRIP_PASS slot=1 type=SELF attenuated=0 stale=3
JeiOS/aarch64: EL0 proof PASS reviewed-cache-tlb=not-observed
QEMU aarch64 smoke: PASS
```

The final negative injection results were:

```text
exact-elr / far-bound / far-lower / owner / direction: C-path FATAL
permission: ISS=0x04f FATAL; load-permission: ISS=0x00f FATAL
l0: lower-level translation ISS FATAL; live-root: predicate FATAL
live-root-core: FATAL sched copy live root
save-path / guard-2 / prefilter-stray: bounded FATAL double fault
restore-selected: FATAL el0-test copy-guard-cleared
```

The positive `COPY_FIXUP_RECOVERED ... guard=1` assertion is the load-bearing
proof that the real restore-selected tail restores, rather than clears, the
guard. The `restore-selected` negative profile is retained as a redundant
tripwire for the explicitly mutated tail.

The exact killer texts are recorded in `tools/mutants.json` for:
`ARM-MP3B-TRANSLATION-ONLY`, `ARM-MP3B-TRANSLATION-ONLY-LOAD`,
`ARM-MP3B-EXACT-ELR`, `ARM-MP3B-OWNER`, `ARM-MP3B-DIRECTION`,
`ARM-MP3B-LIVE-ROOT`, `ARM-MP3B-LIVE-ROOT-CORE`,
`ARM-MP3B-STACK-FREE-PREFILTER`, `ARM-MP3B-GUARD-3-STATE`, and
`ARM-MP3B-GUARD-2-STATE`. The final mutant run killed all 10 ARM MP3b
mutants; the one equivalent result is the pre-existing
`IMAGE-SPAN-GUARD`, and the one skipped result is the pre-existing
`GAP-EP-WAIT-INFLIGHT`.

### Planned / blocked / not enforced

Next bounded task: owner review of this MP3b change and its ADR 0016 evidence,
then remote CI/physical-ARM reproduction. Nothing is currently blocked.
Not enforced or observed here: the hosted ARM predicate matrix mirroring
`tests/test_usercopy.c` with one ARM fault-predicate field flipped per case,
physical ARM execution, SMP, cache/TLB freshness beyond QEMU TCG, arbitrary
boards, remote CI, dynamic loading, general application ABIs, and independent
human qualification. Copy-fault smoke runs still use the full timeout rather
than an early FATAL stop. The QEMU support tier must not be generalized to all
ARM hardware.

## Current status — MP3a-2 FINAL FIX PASS — observed 2026-11-09T03:38:01+00:00

Repository `/tmp/wt-mp3a2`, branch `wip/mp3a2`, exact inspected base commit
`59e19046b515dac52f439478c2ae1c4e1df10782` (pre-change inspected commit;
the final fix pass is the current local change recorded by this handoff). State:
**implemented / tested locally / planned / not blocked**. Support tier
demonstrated: QEMU 8.2.2 `virt-8.2`, `cortex-a72`, one CPU, 256 MiB. PR/issue
state: no PR published, no push, no reviewer assignment; owner review, remote
CI, physical ARM, and independent human qualification are still planned.
The review transcripts are committed under
`docs/adr/debates/0015-mp3a-el0-svc/`; the stray root-level
`MP3A2-BRIEF.md` and `MP3A2-REVIEW.md` are removed by this fix pass.

### Implemented

MP3a-2 adds actual AArch64 EL0 execution on top of MP3a-1: slots 8/9 lower-EL
savers, frozen-frame EL0 validation, first ERET, per-process TTBR0/ASID and
kernel-stack selection, `SP_EL0`/TPIDR restore, x8 SVC decode/dispatch,
fault-death, lifecycle reap-with-survivor, MMU-on user-frame/table teardown,
EL0 sysreg hardening, and the two-process real-code proof. Non-EL0 profiles
retain their fatal lower-EL stubs and existing EL1h behavior.

Files changed or added:

- `kernel/arch/aarch64/{main.c,paging.c,paging.h,sched_arm.c,trapframe.h,vectors.S,vm_backend.c,el0_test.c}`
- `kernel/core/lifecycle.c`
- `mk/aarch64.mk`
- `tools/{check_arm_trapframe_boundary.sh,smoke_arm.py,run_arm_el0_test.py,mutants.json}`
- `docs/{IMPLEMENTATION.md,HANDOFF.md}`
- Removed the stray root-level `MP3A2-BRIEF.md` and `MP3A2-REVIEW.md`;
  retained the committed review transcripts under
  `docs/adr/debates/0015-mp3a-el0-svc/`.

The proof is kernel/trusted-state driven. It does not accept a user-reported
success flag: the smoke oracle checks non-zero SVC status/result and x16-x18
canaries, distinct user-SP round-trips, a poisoned-TPIDRRO scrub round-trip,
exact syndrome fields, same-VA different contents, timer-live preemption,
three isolated mode discriminators plus one composite AArch32 case,
DCC/CNTKCTL negative exercises, structural-vs-user-death classification,
ordered death/reap, and continued peer execution. The MMU-on barrier/TLBI/cache
sequences are documented and reviewed; QEMU TCG does not model stale TLB or
cache behavior. The EL0 run keeps the timer live. Its PASS gate is evidence of
at least one completed lone self-yield reselect plus at least one lone
self-targeted IRQ reselect; the PASS marker is emitted before the second lone
self-yield dispatch completes.
The `(!vector_el1 && !vector_el0)` range clause has no independent negative
probe yet and is not independently enforced by this proof.

### Tested

Exact commands and observed results:

```text
make aarch64 ARM_EL0_TEST=1                       PASS
make aarch64 ARM_USER_TEST=1                      PASS
make smoke-aarch64 ARM_USER_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60
                                                     PASS
make smoke-aarch64 ARM_EL0_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60
                                                     PASS
make smoke-aarch64 ARM_REQUIRE=1                  PASS
make smoke-aarch64 ARM_SCHED_TEST=1 ARM_REQUIRE=1 PASS
make smoke-aarch64 ARM_TIMER_TEST=1 ARM_REQUIRE=1 PASS
make smoke-aarch64 ARM_MMU_FAULT=text ARM_REQUIRE=1 PASS
timeout 60 qemu-system-aarch64 -M virt-8.2,gic-version=2,virtualization=off,secure=off,dtb-randomness=off -cpu cortex-a72 -smp 1 -m 256M -display none -serial file:/tmp/fx2.log -no-reboot -kernel build/aarch64-el0test/Image
                                                     serial PASS; timeout rc 124 after halt
make mutants                                       PASS (55 killed, 1 equivalent, 0 survived, 0 blocked, 1 skipped)
make check                                         PASS
sh tools/check_arm_trapframe_boundary.sh           PASS (self-test catches all 3 raw-SP forms)
git diff --check                                   PASS
python3 -m json.tool tools/mutants.json            PASS
python3 -m py_compile tools/smoke_arm.py tools/run_arm_el0_test.py
                                                     PASS
```

The final `make mutants` result was `55 killed, 1 equivalent, 0 SURVIVED,
0 BLOCKED, 1 skipped`; there were no survivors or blocked mutants. The
ARM-USER mutants include the TPIDRRO scrub, vector/M discriminator, and
CNTKCTL/DCC syndrome guards; each is required to be killed. The scheduler
mutant is load-bearing: its mutated raw boot FATALs
`sched no live EL0 successor`. The remaining unobserved items are hardware
cache/TLB effects and physical-ARM execution.

### Fix-pass mutation evidence

Each listed mutation was injected into a disposable build and booted far
enough to verify a kernel `FATAL`; no result below relies only on a printed
success marker:

- Deleting the EL0t `TPIDRRO_EL0` scrub caused
  `FATAL el0-test svc-proof-result`.
- Deleting the `vector_el0 && mode != EL0t` discriminator caused
  `FATAL el0-test mode-negative`.
- Replacing CNTKCTL zero-hardening with the old EL0PTEN-enabled mask caused
  `FATAL el0-test cntkctl-trap-syndrome`.
- Clearing TDCC in a disposable DCC mutation caused
  `FATAL el0-test dcc-trap-syndrome`.
- The MODE-CONFUSION mutation caused
  `FATAL sched kernel-sp mode` inside the synthetic probe.
- The boundary self-test catches `(*tf).sp`, `tf[0].sp`, and `tf-> sp`;
  the real source scan passes.

### EL0 proof serial evidence

```text
EL0 MODE_CHECK EL1h-frame-rejected-as-EL0
EL0 MODE_CHECK vector9-EL1h-rejected
EL0 MODE_CHECK vector5-EL0t-rejected
EL0 MODE_CHECK aarch32-M4-composite-rejected
EL0 setup two-processes real-code timer-live
EL0 TIMER_PREEMPT from=1 to=2 switches=1
EL0 SVC register proof slot=1 status=1 result=0x...5aa5 x16=...1616 x17=...1717 x18=...1818 tpidrro=0x...0000
EL0 TPIDRRO_ROUND_TRIP slot=1 value=0x...0000
EL0 SVC register proof slot=2 status=1 result=0x...5aa5 x16=...1616 x17=...1717 x18=...1818 tpidrro=0x...0000
EL0 TPIDRRO_ROUND_TRIP slot=2 value=0x...0000
EL0 RUN slot=1 data=0x...1111 arena=0x0000100000002000
EL0 RUN slot=2 data=0x...2222 arena=0x0000100000002000
EL0 EXPECTED_DEATH stage=0 ec=0x...0024 FAR=0x0000000040000000
EL0 EXPECTED_DEATH stage=1 ec=0x...0024 FAR=0x0000000040000000
EL0 EXPECTED_DEATH stage=2 ec=0x...0020 FAR=0x0000000040000000
EL0 EXPECTED_DEATH stage=3 reason=2 (bad SP)
EL0 EXPECTED_DEATH stage=4 reason=2 (bad ELR)
EL0 EXPECTED_DEATH stage=5 ec=0x...0007 (FP)
EL0 EXPECTED_DEATH stage=6 ec=0x...0018 (CNTVCT)
EL0 EXPECTED_DEATH stage=7 ec=0x...0015 (SVC #1)
EL0 CNTKCTL_PREEMPT_WRITE_TRAP stage=8
EL0 DCC_CHANNEL_TRAP stage=9
EL0 REAP_WITH_SURVIVOR stage=0..9
EL0 REAP_WITH_SURVIVOR lone slot=2
EL0 LONE_SURVIVOR_YIELD slot=1 count=1
EL0 LONE_IRQ_RESELECT slot=1
EL0 proof PASS reviewed-cache-tlb=not-observed
```

### Planned

MP3b and MP3c are deferred to their bounded ADR/roadmap gates. Remaining
qualification work is owner review, remote CI reproduction, physical ARM
execution, independent human review, and any future runtime IRQ/EL0 profile
needed to promote beyond the demonstrated QEMU tier.

### Blocked / not enforced

Nothing is currently blocked. Not enforced or observed here: cache/TLB
staleness on hardware, physical ARM, SMP, arbitrary board support, dynamic
loading, broad application ABIs, remote CI, and independent human
qualification. Security statements above map to actual EL0 frame validation,
TTBR0/ASID installation, explicit ASID-1 per-page TLBI, sysreg readback,
exception syndrome checks, mode-checked SP accessors, and lifecycle state
transitions; they are not blanket claims about all hardware. The 64-byte
cache-line stride, identity-VA I-cache invalidation, and VALE1IS-to-VAE1IS
maintenance edit remain target-profile/flagged items.

## Current status — MP3a-1 adversarial fix pass — observed 2026-11-08T23:29:54Z

Repository `/tmp/wt-mp3a1`, branch `wip/mp3a1`, exact inspected base commit
`8cb1f3d1d0fd9ede667db1014cdd86729a7ae510` (the implementation commit; this status refresh is a
documentation-only descendant). State: **implemented / tested locally /
planned (owner review,
remote CI, physical ARM and independent human qualification) / not blocked**.
This is AI-assisted evidence, not human or release qualification. Support tier:
**QEMU 8.2.2 only**. No push, PR publication, reviewer assignment or release
was performed. `MP3A1-BRIEF.md` is pre-existing, untracked, and untouched.

### Implemented

Files changed: `kernel/core/vm.c`,
`kernel/arch/aarch64/{main.c,paging.c,paging.h,sched_arm.c,vm_backend.c}`,
`tools/{smoke_arm.py,mutants.json,run_arm_user_test.py}`, this handoff, and
`docs/IMPLEMENTATION.md`. Existing review briefs and debate attachments were
preserved and not added.

The ARM backend now builds per-process TTBR0 roots that copy the decorated
kernel L0 entries and add a private arena branch at VA
`0x0000100000000000`. The three EL0 leaf classes are exact and audited:
code `0x0020000000000fc3` (EL0 RX), rodata `0x0060000000000fc3` (EL0 R), and
data `0x0060000000000f43` (EL0 RW). Kernel leaves remain AP[1]==0. The
separate per-process audit additionally rejects valid arena L1/L2 upper entries,
a mapped guard, shifted stack leaves, and foreign/out-of-window process-table
pointers before dereference. The pre-enable kernel audit and kernel map were
not changed semantically.

The EL1 proof uses `AT S1E0R/W`; it does not execute at EL0 and does not add
SVC or a user trapframe. TTBR0 install is wired for user ASID 1 and kernel
ASID 0 with `ASIDE1IS; DSB ISH; ISB`, operand recording, TTBR0 readback, and
an ELF instruction check. The dead `je_arm_sched_bind_user_root` seam was
removed; MP3a-2 must add root ownership and unbind on slot free/reuse before
dispatch can install process roots. SP_EL1 installation is deferred to MP3a-2
because changing it in this C seam would invalidate the active EL1 stack
context.

USER_CODE and USER_RODATA publication now enters the paging.c W^X ledger and
clear-PTE removes the ledger entry; the in-proof RW alias returns
`JE_ERR_DENIED`. USER_DATA remains RW and UXN without a W^X entry. The user
oracle prints and ELF-checks probe VAs, requires level-3 permission faults
`0x...081f` for kernel/device/write-denial probes, and checks TTBR0 root+ASID.
The table cap is 256 and the user profile prebuilds L3 paths for the 256 MiB
managed RAM window. Its pre-enable audit evidence was
`cycles=0x000000000051df1f reads=0x0000000001081800`. The cycle value is a
QEMU CNTVCT execution-counter sample, not a wall-clock qualification; reads
are the stable descriptor-input count.

### Verification

Exact commands and observed results:

```text
make aarch64                                  PASS
make aarch64 ARM_USER_TEST=1                  PASS
make smoke-aarch64 ARM_USER_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60
                                               PASS
make smoke-aarch64 ARM_REQUIRE=1              PASS
make smoke-aarch64 ARM_SCHED_TEST=1 ARM_REQUIRE=1
                                               PASS
make smoke-aarch64 ARM_MMU_FAULT=text ARM_REQUIRE=1
                                               PASS
make check                                    PASS
make mutants                                  PASS
git diff --check                              PASS
python3 -m py_compile tools/smoke_arm.py tools/run_arm_user_test.py
                                               PASS
```

The user-test oracle independently checked the following observed values:

```text
P1 root 0x0000000040105000, P2 root 0x0000000040115000
TTBR0: 0x0001000040105000 / 0x0001000040115000 (ASID 1)
kernel AT S1E0R/W: PAR 0x000000000000081f / 0x000000000000081f (fault)
UART/GICD AT S1E0R: 0x000000000000081f / 0x000000000000081f (fault)
P1 code R/W:       PAR 0xff00000040109b80 / 0x000000000000081f
P1 data W:         PAR 0xff0000004010bb80
P1 rodata R/W:     PAR 0xff0000004010ab80 / 0x000000000000081f
P2 data R:         PAR 0xff0000004011bb80
W^X alias:          code-pa=0x0000000040109000 status=DENIED
same-VA data PA:   0x4010b000 != 0x4011b000
ASIDE1IS operand:  0x0001000000000000 for both installs
audit marker:      user-test audit PASS
terminal:          user-test PASS reviewed-tlb=not-observed
```

All requested ARM profiles and x86 `make check` passed locally. The full
hosted check passed the unchanged paging-bit suite and all existing kernel
tests. The current user Image SHA-256 is
`3494a76dcdd3fb79b850964976dcc537025605224e3264dbd7224e7a2c74b0e5`.

B1 scratch re-injections were negative as required: stray arena L1[1] and L2[1]
each built with `build_rc=0` and stopped with `smoke_rc=2`,
`FATAL user-test audit-p1`; the shifted-stack/guard injection did the same.

`make mutants` reported `41 killed, 1 equivalent, 0 SURVIVED, 0 BLOCKED,
1 skipped`. The eight new ARM mutants were all killed:
skip-decorate-kernel-L0, wrong-root-for-p2, stray-arena-L1-entry,
guard-page-shift, unmapped-kernel-probe, no-ASIDE1IS-TLBI,
no-ASID-in-TTBR0, and map-user-code-frame-RW. The pre-existing enabled set
remained `33 killed, 1 equivalent, 0 SURVIVED, 0 BLOCKED` before these
additions.

### Planned / blocked / not enforced

Planned: owner review, remote CI, physical ARM reproduction, and independent
human qualification. Not blocked. MP3a-2 is the next bounded task: add
MMU-on mappings/cache maintenance for process tables and user frames, clear-PTE
and TLBI lifecycle, root ownership/unbind on slot reuse, then install the
process kstack in the safe EL0-return seam and add the user trapframe/entry.
SVC remains outside this slice. EL0 execution, SVC, SP_EL1 handoff,
MMU-on user-table teardown, cache/TLB staleness, SMP, dynamic loading,
arbitrary hardware, remote CI, and independent human qualification are not
claimed here.

The TLBI/barrier order and cache maintenance are reviewed, not observed as
hardware cache/TLB behavior under QEMU TCG. The backend intentionally fails
closed rather than silently performing its MMU-off identity-pointer lifecycle
after enable. The observed facts are the AT S1E0 fault/success results, exact
PAR physical addresses, root/ASID readback, W^X refusal, root separation, and
audit markers.

## ADR 0014 lifecycle extraction — observed 2026-11-08T20:00:36Z

Repository `/tmp/wt-adr14-extract`, branch `wip/adr0014-extract`, exact
implementation baseline inspected at
`9c356e80e2be1503f7c1a0bcaadf9752c4cb8ad2` with the extraction worktree
changes. State: **implemented / tested locally / planned (owner review, remote
CI and independent human qualification) / not blocked**. This is AI-assisted
evidence, not human or release qualification. No push, PR publication,
reviewer assignment or release was performed. The pre-existing untracked
`EXTRACT-BRIEF.md` was not modified.

### Implemented

- Added `kernel/core/lifecycle.c` and `include/jeios/lifecycle.h` as the
  neutral link-time port. The module owns `process_pool`, `registered_vm[]`,
  and `registered_proc[]`, their four existing `je_sched_*` registry APIs,
  read accessors, waiter-release wrapper, ordered dying prefix, process-record
  kill decision, and destroy/`on_reaped` pair.
- Added `lifecycle.c` to the x86 kernel `SOURCES` and to both affected hosted
  target prerequisites. `test_sched_killall` includes it exactly once under
  its existing `#define static`; the unchanged lifecycle-order test uses the
  test-only scheduler include binding so it does not link a second copy.
- Kept `threads[]`, `current`, `ready`, the complete reaper loop, all six
  reaper/F08 mutant anchors (REAPER-SIGN, REAPER-IF-CHECK, REAPER-CR3-GUARD,
  REAPER-RSP0-GUARD, REAPER-VM-INDEX, F08-ONE-THREAD — verified present exactly
  once and still KILLED), kill and kill-all run-state/frame gates, death
  record writes, initial frame synthesis, and every architecture-specific
  scheduler operation in `sched.c`.
- Added the ADR 0014 D3 neutral conformance suite `tests/test_proc_lifecycle.c`
  (links `lifecycle.c` as its own TU + real `proc.c`, no `JE_SCHED_HOSTED_TEST`),
  pinning the dying ORDER, the record-LIVE-at-device-hooks ordering, failure
  propagation, the legacy (gen==0) release-only path, the fault/non-fault split,
  the registry writers, `je_lc_kill_check`, and `je_lc_reaped`. Added two
  neutral-module mutants (LC-ONDYING-FAIL-PROPAGATE, LC-ONDYING-ORDER-DEVHOOK),
  each killed by that suite. Added `tools/check_lifecycle_boundary.sh`, run by
  `make check`: it strips comments then rejects any x86/run-queue token in
  `lifecycle.[ch]` code, and compiles `lifecycle.c` with the aarch64 freestanding
  flags (`-Werror -Wconversion -Wshadow`) to prove arch-neutrality.

### Decision and logic

The approved link-time-port boundary was used instead of a runtime per-slot
operations table. The white-box test directly accesses the registry storage,
and the six enabled `sched.c` mutants require exact text in the reaper/F08
regions. Moving those regions or mirroring them behind a new authority would
either block the mutants or make the tests observe a non-authoritative copy.
The registry is therefore single-owned by `lifecycle.c`; the
`threads[].{vm,vm_index,proc_id}` values remain the scheduler's synchronized
slot cache required by the frozen white-box and run-queue paths.

`je_lc_on_dying` preserves the observable sequence by performing waiter release
first, then the fault-only/device death hooks under the original
`JE_SCHED_HOSTED_TEST` conditional, then `je_proc_on_dying`. It returns the
process status; `mark_dying` retains the existing fail-stop policy and only
then publishes wait/run/death fields. `je_lc_reaped` preserves
destroy-before-`je_proc_on_reaped` and returns either failure so the unchanged
reaper calls `abort_scheduler`. `je_lc_kill_check` contains only the D8
PROCESS-record decisions; the caller's IF check, slot/kind/state/frame gate,
frame snapshot, and `mark_dying` remain in `sched.c`.

The neutral header does not include `sched.h` and the neutral source has no
architecture frame, register, interrupt, halt, or run-queue references. The
ARM user-stack ownership fix and reaper slot-cache cross-check are not
implemented here; they remain the ADR 0014 MP3 entry criteria because the ARM
user path does not yet exist and changing its tested scheduler path would
expand this surgical port.

### Verification actually run

```text
make build/0/test_sched_killall build/0/test_lifecycle_order build/0/kernel/core/lifecycle.o
./build/0/test_sched_killall
test_sched_killall: PASS (259 checks)
./build/0/test_lifecycle_order
test_lifecycle_order: PASS (50 checks)
make check
make mutants
git diff -U0 tests/test_sched_killall.c
nm -u build/0/kernel/core/lifecycle.o
grep -n -E 'je_trap_frame|cr3|tss|rsp0|rflags|int[[:space:]]+0x80|int[[:space:]]+0x81|hlt|<<[[:space:]]*9' kernel/core/lifecycle.c include/jeios/lifecycle.h
git diff --check
```

Observed results (lead-run on the integrated head, gcc 13.3, QEMU 8.2.2):

- `make check` passed, including the full hosted suite (test_sched_killall 259,
  test_lifecycle_order 50, test_proc_lifecycle 59), kernel ELF/image checks, and
  the new lifecycle boundary check.
- `make mutants` reported `33 killed, 1 equivalent, 0 SURVIVED, 0 BLOCKED,
  1 skipped`; all six scheduler anchors killed, plus the two new neutral-module
  mutants.
- `make smoke` (default) PASS; `make integration` (modes 1-15) PASS with serial
  markers identical to the pre-extraction baseline after masking volatile
  numbers (allocator free-page counts and cycle counters shift only because
  `lifecycle.c` is a new linked object; no marker kind or PASS/OK text changed);
  `make check TEST_MODE=1` and `TEST_MODE=2` PASS.
- The `test_sched_killall.c` diff contains exactly one added include line.
- The lifecycle boundary check passed (no x86 tokens in code; `lifecycle.c`
  compiles clean under the aarch64 freestanding flags).

`make smoke`/`make integration`/`JE_TEST_MODE` WERE rerun (ADR 0014 part 3
requires it, and `lifecycle.c` is a new kernel source in `SOURCES`, so the boot
image does change in size/layout). An earlier draft of this handoff claimed they
were skipped for "no boot image change"; that was incorrect and is corrected here.

### Planned / blocked / not enforced

Planned: owner/peer review, remote CI reproduction, and the later ARM
neutral/conformance binding. Not blocked for this x86 link-time port. Not
enforced: ARM user lifecycle/reaping, VM-owned ARM stack non-freeing,
slot-cache cross-check during ARM reaping, SMP, dynamic loading, arbitrary
hardware support, remote CI and independent human qualification.

Next bounded task: review this committed extraction against ADR 0014, then
begin MP3 only after the ARM stack-ownership and reaper-cache criteria are
implemented and independently tested.

## Current status — 2026-11-08T13:56:53+00:00

Repository `/tmp/wt-mp2-cfix`, branch `wip/mp2-cfix`. The implementation and
verification were inspected at exact commit
`531f70ee872d6fb9fc61028711b35bb98d8a89cc`; this final documentation-only
amend is its descendant. State:
**implemented / tested locally / planned (owner review, remote CI, physical
ARM and independent human qualification) / not blocked**. This is
AI-assisted evidence, not human or release qualification. The demonstrated
support tier is QEMU-only. No push, PR publication, reviewer assignment,
release, or remote-CI result was created or observed. The user-provided
`RE-REVIEW-FINDINGS.md` and `REVIEW-FINDINGS.md` remain untracked and were
not modified.

This refresh supersedes the 13:01 status below and records the second-pass
re-review fixes:

- The test-only phase-1 ELR rule now admits the canary/NZCV assembly, worker
  entry/trampoline, idle preparation, and exit regions. The idle and exit
  assembly call sites mask I before their C calls, and `je_arm_sched_exited`
  is published only after `je_rr_exit`; this closes the three injected
  unmasked-window false-FATAL cases.
- Phase-B canary addressing uses already-verified low registers for its
  stack/table indexes and bases. The high-restore mutant remains test-only:
  it omits the pair restore and poisons x26 from restored x0, making the
  named `canary reg=26` result deterministic. The production `vectors.o`
  is byte-identical to the 8009df0 pre-fix checkpoint.
- `drop-consume-on-resume` is documented and detected at the actual
  cookie-zeroed-by-IRQ-saver check. A new `replay-stale-frame` mutant saves
  an old nonzero frame copy and replays it at the same address; its named
  detector is the resume-path frame-generation comparison.
- `monitor_wait` now reads the readiness deadline before idle is requested
  and emits `FATAL sched proof readiness deadline` if readiness never
  completes. The no-tick path also runs sleeping-state validation.
- Phase-A and phase-B labels end at their final comparisons; the static
  canary-phase array is marked `used`; redundant nested test guards were
  removed; test-only console diagnostics are explicitly separated from the
  production IRQ-switch contract.

Observed commands and results:

```text
make aarch64                                      PASS; Image 36873 bytes
make aarch64 ARM_SCHED_TEST=1                    PASS; Image 53264 bytes
make smoke-aarch64 ARM_SCHED_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60
                                                   PASS; normal proof/guard halt
make check                                        PASS
git diff --check                                  PASS
python3 -m py_compile tools/smoke_arm.py         PASS
```

The final normal scheduler smoke used QEMU 8.2.2 (`virt-8.2`, `cortex-a72`,
one CPU, 256 MiB), GCC 13.3.0, and image SHA-256
`375d1a00170f24f91c356b9fb076ea0dafb52d82a4de9fe90fc359b0a3e297f`.
It observed idle interval `16->24` with four slot-0 ticks and worker 1
resuming; slots 1 and 2 each had starts/resumes/phase-A/phase-B/canary/
NZCV/verified-resume evidence; the oracle observed
`dispatched=32 = preemptions 12 + nonpreempt_switch_ins 20`; and the guard
translation fault ended in the expected terminal halt.

The required 13-mutant loop was run with
`ARM_SMOKE_TIMEOUT=25`. Every run emitted `MUTATION FIRED`, ended at its
named detector, emitted no final guard PASS, and intentionally returned
make rc 2:

| mutation | observed detector |
| --- | --- |
| `drop-mov-sp` | `FATAL sched observed_running` |
| `pick-skips-b` | `FATAL sched proof ready-bound slot=2` |
| `rebuild-fake-frame` | `FATAL sched proof frame validation slot=1` |
| `unconditional-cur-ready` | `FATAL sched proof dispatches_after_exit slot=1` |
| `wfi-i-masked` | `FATAL sched proof exit DAIF.I slot=1` |
| `corrupt-spsr-nzcv` | `FATAL sched canary reg=32` |
| `spsr-i` | `FATAL sched proof frame validation slot=1` |
| `early-fake-swap` | `FATAL sched proof frame validation slot=0` |
| `live-pointer-swap` | `FATAL sched proof frame validation slot=4` |
| `live-content-swap` | `FATAL sched canary reg=26` |
| `drop-high-reg-restore` | `FATAL sched canary reg=26` |
| `drop-consume-on-resume` | `FATAL sched frame cookie zeroed by IRQ saver` |
| `replay-stale-frame` | `FATAL sched proof frame generation slot=1` |

The deterministic high-register check was repeated ten times:
all ten runs returned rc 2 and all ten ended at `FATAL sched canary reg=26`;
none hit the former data-abort/guard-mismatch outcome.

Required empirical re-confirmations were run in disposable scratch copies:

- Trampoline, idle-call, and exit-call timing delays each completed the
  normal QEMU proof without a false FATAL.
- Forcing `all_proof_ready()` to remain false produced
  `FATAL sched proof readiness deadline slot=4`, with smoke rc 2, rather than
  an external timeout.
- Removing only the resume-path generation comparison from the replay
  scratch image let `replay-stale-frame` emit its FIRED marker and reach a
  normal scheduler/guard PASS, proving that comparison is load-bearing.

The production seam check compared `build/aarch64/vectors.o` with a fresh
build of checkpoint `8009df0c36aabee92ed223d26c8429671cc4ecf5`:
both SHA-256 values were
`3e9ce000c688cf72ebf55c95e097d477ecdc7ff0a2a2015e5f313894a27c1b7d`.
The only source delta in `vectors.S` is under the test-mutant conditional.

Known limitations intentionally left open:

- C: `wfi-i-masked` deliberately clears I inside the test-only C mutant; it
  does not model dropping the later assembly `msr daifclr` before WFI, which
  would hang without a named detector.
- F: the pre-existing ready-bound oracle can become unsound if a sentinel
  runs for more than one tick because its limit is stale.
- SF4: killed and surviving mutants both exit 1 through `make` rc 2; there is
  no hosted `check_sched_test` test yet.

Not enforced: SMP safety, arbitrary ARM hardware, driver/application ABI
coverage, userspace context switching, dynamic loading or fault isolation,
remote CI, physical ARM, and independent human qualification. The existing
linker RWX LOAD warning remains and is not a security qualification.

Next bounded task: owner/peer review of this committed second-pass diff.
Pass criteria are the exact command results above, preservation of the
production vectors object and frozen scheduler contracts, and explicit
classification of the C/F/SF4 limitations. No ARM hardware or general
kernel-readiness claim follows from this QEMU-only evidence.

## Current status — 2026-11-08T13:01:48+00:00

Repository `/tmp/wt-mp2-cfix`, branch `wip/mp2-cfix`, exact commit
`8009df0c36aabee92ed223d26c8429671cc4ecf5`. State:
**implemented / tested locally / planned (owner review, remote CI, physical
ARM and independent human qualification) / not blocked**. AI-assisted only;
QEMU-only support tier. No PR, push, release, reviewer assignment, or remote
CI result was created/observed. User-provided `REVIEW-FINDINGS.md` remains
untracked and was not modified.

Implemented files:

- `kernel/arch/aarch64/sched_arm.c`: no-op ARM idle hook with slot-0 fallback,
  frame-generation comparison and consume-on-resume proof, strict phase/ELR
  checks, D3/D7 fail-closed checks, phase-A/phase-B live-register canary,
  all-sleep idle proof, terminal wait placement, and real mutation detectors.
- `kernel/arch/aarch64/vectors.S`: test-only `drop-high-reg-restore` guard
  around the existing high-register restore.
- `mk/aarch64.mk`, `tools/smoke_arm.py`: twelve mutation profiles, counters
  for both live phases, idle marker validation, and named detector checking.
- `docs/IMPLEMENTATION.md`, this file, and
  `docs/audit/evidence/2026-11-mp2-sched.md`: current status/evidence refresh.

Observed:

- `make aarch64`: PASS.
- `make aarch64 ARM_SCHED_TEST=1`: PASS.
- `make smoke-aarch64 ARM_SCHED_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60`:
  PASS; idle `all_sleep=1`, interval `16->24`, four slot-0 ticks, worker 1
  resumed; oracle `dispatched=32 = preemptions 12 + nonpreempt 20`; guard
  fault PASS and terminal halt.
- Exact twelve-mutant loop: all twelve emitted FIRED, ended at their named
  detector, emitted no final guard PASS, and intentionally returned make rc 2.
- `make check`: PASS.
- `git diff --check` and `python3 -m py_compile tools/smoke_arm.py`: PASS.

The four detector-deletion experiments were run only in disposable scratch
copies: wfi real-check deleted -> normal PASS; unconditional exited[] check
deleted -> timeout/no named detector; corrupt NZCV branches deleted -> normal
PASS; READY-bound fatal deleted -> normal PASS. This is evidence that those
four checks are load-bearing. No independent human qualification is claimed.

Next bounded task: owner/peer review of commit `8009df0`, then reproduce the
same commands in remote CI if requested. Pass criteria are preservation of
the frozen vector/ADR 0013 contracts, the normal idle/counter/guard evidence,
and all twelve named mutation results. Do not advance an ARM hardware or
userspace readiness claim from this QEMU-only result.

## Historical 2026-11-08T12:19:50Z — MP2 ADR 0013 ARM switch seam, superseded

Repository `/tmp/wt-mp2-switch`, branch `wip/mp2-switch`, exact inspected
commit `6dd9e59` plus the uncommitted worktree diff. State:
**implemented / tested locally / planned (owner review, remote CI, physical
ARM and independent human qualification) / not blocked**. This is
AI-assisted evidence, not a hardened/release qualification. No commit or push
was made.

Files changed: `kernel/arch/aarch64/vectors.S`, `main.c`, `trapframe.h`,
new `sched_arm.c`, `mk/aarch64.mk`, `tools/smoke_arm.py`, and this
documentation. `sched_rr.c`, ARM paging sources, and x86 sources were not
touched. The exact asm edit is in `vectors.S`: after `bl je_arm_irq`, `tst
x0,#0xf; b.ne je_arm_double_fault; mov sp,x0`, then the existing guard-clear,
selected-frame ELR/SPSR/GPR restore, `add sp,#JE_TF_SIZE`, and `eret`; no
`daifclr` is executed before `eret`.

The C seam records the current IRQ frame, writes a slot/generation cookie,
ticks/picks/switches through `je_rr_switch`, runs the same frame validator as
the early hook as the final gate, consumes `next->frame`, and returns only
the exact validated local pointer. Validation covers non-NULL/alignment,
overflow-safe owned-stack bounds and mapped state, exact recorded pointer,
consume-on-resume state, `sp`, EL1h policy SPSR with NZCV masked, and ELR
inside linked text or the trampoline. Stack allocation maps only the top four
pages of each five-page run and leaves the lowest guard page unmapped.

Observed proof counters from
`make smoke-aarch64 ARM_SCHED_TEST=1 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=60`:

```text
slot=1 starts=1 dispatched_to=4 resumed=3 observed_running=3
       preempted_in_canary=3 verified_resumes=3 interleave=4
       nzcv_window_preempts=2 dispatches_after_exit=0
slot=2 starts=1 dispatched_to=4 resumed=3 observed_running=3
       preempted_in_canary=3 verified_resumes=3 interleave=4
       nzcv_window_preempts=2 dispatches_after_exit=0
slot=3 starts=1 dispatched_to=8 resumed=7
slot=4 starts=1 dispatched_to=8 resumed=7
oracle: dispatched=24 preemptions=8 nonpreempt_switch_ins=16 ready_bound=1
guard: target=0x40007000, expected translation fault, FAR==guard,
       monitor-masked I preserved in guard SPSR, PASS
```

Mutation battery, each run emitted `MUTATION FIRED`, the named FATAL, no
PASS, and intentionally returned a nonzero smoke status:

```text
drop-mov-sp          -> FATAL sched observed_running
pick-skips-b         -> FATAL sched proof preempted_in_canary slot=2
rebuild-fake-frame   -> FATAL sched starts==1
unconditional-cur-ready -> FATAL sched proof dispatches_after_exit slot=1
wfi-i-masked         -> FATAL sched proof exit DAIF.I slot=1
corrupt-spsr-nzcv    -> FATAL sched proof nzcv-window slot=1
spsr-i                -> FATAL sched proof frame validation slot=1
early-fake-swap      -> FATAL sched trampoline arg!=current
live-pointer-swap    -> FATAL sched proof frame validation slot=4
live-content-swap    -> FATAL sched canary reg=32
```

Verification actually run:

- `make aarch64` and `make aarch64 ARM_SCHED_TEST=1`: PASS.
- `make check`: PASS, including hosted scheduler, allocator, MMU/GIC and
  image/ELF checks.
- `python3 -m py_compile tools/smoke_arm.py` and
  `python3 tools/test_smoke_arm_timer.py`: PASS.
- Existing ARM smoke matrix: default, `ARM_TIMER_TEST=1`,
  `ARM_VECTOR_TEST=1`, `ARM_DTB_BAD=1`, `ARM_BSS_CHECK=1`, and
  `ARM_MMU_FAULT=text|data|rodata|guard`: all PASS with `ARM_REQUIRE=1`.

Not enforced yet: SMP safety, arbitrary hardware/driver support, userspace
context switching, dynamic loading/fault isolation, remote CI, physical ARM,
and independent human qualification. The linker still reports its existing
RWX LOAD warning. The scheduler proof is a QEMU support-tier result, not a
claim for all ARM hardware.

Next bounded task: owner/peer review of this isolated diff and a reviewable
commit/PR. Pass criteria: preserve the frozen vector/ADR 0013 contracts,
reproduce the normal counter table and all smoke profiles, and retain one
named detector/no-PASS result for every mutation. Do not advance the next
ARM userspace/lifecycle gate from this QEMU-only evidence.

## JTerminal future roadmap — 2026-11-06T10:31:01-05:00

Inspected core main `a82352f405b8e4c4cb9336fdf847514ea9435734` (PR #94 merged), in isolated
`docs/native-terminal-roadmap` worktree. Implemented documentation only:
[JTerminal](JTERMINAL.md) with JT0–JT4, plus README/agent/roadmap/shell/cluster
links. Runtime remains planned; current stabilization priority is unchanged.
Name and scope follow owner direction: native app, persistent attached/detached
sessions, open compatibility, ecosystem connections and enterprise distributed
management. No source imports, frozen ABI changes or mobile pin changes.

Verification: `make check` exit 2, LeakSanitizer /proc inspection failure.
`ASAN_OPTIONS=detect_leaks=0 make check` exit 2: generated build/0/test_vm lacked
execute permission (mode 0644); earlier tests ran but suite completion is blocked.
Leak detection and full hosted PASS are not claimed. No boot changes, so no smoke
rerun; no local native/container/hardware qualification. `git diff --check` and
new-document relative links passed. No independent peer/human review claimed.

Next: finish existing repair gate; then review JT0 contracts alongside SH2/M6.
JT1 needs qualified PTYs/process control; UI waits for M7; remote/enterprise
features wait for identity, networking and clustering qualification. Implementers
must supply per-gate negative tests, exact-revision evidence and status updates.


## Current-code fix gate — 2026-11-06 UTC

Reviewed core main `e4bd9c24d69de4fc6798d733b1f2e6398522748f`, merged PRs
#89–#92, raw peer audits, and mobile main
`9221701ab2d5333978a294f54574ee384f4216e3`. Implemented documentation only:
[repair-first response](audit/2026-11-fix-gate.md), F01–F14 acceptance packages,
current entrypoint/status routing, and [reproduction evidence](audit/evidence/2026-11-fix-gate.md).
No repairs or new functionality are claimed complete. Original dirty worktrees
were preserved. This single-agent review is not independent human qualification.

Verification: `make check` exit 2 (LeakSanitizer environment limitation);
`ASAN_OPTIONS=detect_leaks=0 make check` exit 0. `make mutants` exit 0, reported
six killed/four skipped, subject to the independently reproduced false-green
runner defect. ARM self-cycle rejected; foreign sibling pointer faults during
alias scan. Main CI 42177874117 passed at the baseline, not this response commit.
No local Docker/QEMU/hardware run. Final source diff is documentation only.

Next: working agents close F01–F14 with exact-revision before/after evidence and
reviewed dispositions before new implementations. Begin with runner/build
trustworthiness, then reaper sign and unsafe ARM audit dereferences. ADR 0013 is
accepted; scheduler implementation remains gated. Update #18/#19/#87 with repair
PR evidence. Do not advance the mobile core pin without separate qualification.


## Audit refresh — 2026-11-06 UTC

Implemented: canonical status routing and dated-history labels; core adds audit response and corrected security introduction. Tested: `make check` stopped in LeakSanitizer: unable to read /proc task data under this execution environment. `ASAN_OPTIONS=detect_leaks=0 make check` passed; ASan/UBSan remained enabled, leak detection was not validated. No smoke/integration/emulator rerun for this docs-only change. `git diff --check` passed.

Reviewed audit PR #86 against fetched core main `fc8b8af931ee3fc79a82dcecc23356ba964c662b` and mobile main `d9a5146b1de5b870f495ccf5300ee7b679c3507e`. Both baseline main CI runs passed; this is not new-PR CI evidence. No runtime invariants changed.

Next: core #87 tracks remaining MP2 scheduling, lifecycle design review before MP3, and ARM service parity; #18/#19 track named critical-mutant CI. Require explicit pass/fail runtime evidence for each implementation gate. Lifecycle extraction is a proposal requiring owner review, not authorization to change tested paths. Mobile stays unqualified. Original dirty checkouts were preserved using separate worktrees.

Current checkpoint and work order: [STATUS.md](STATUS.md). Dated status and next-task statements below are historical when superseded by that checkpoint.

## 2026-11-05T01:27:58Z — MP2 PR-2b `--timer-test` smoke oracle + canary review (test author)

Observed repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios`,
branch `feat/mp2-aarch64-irq-return`, exact commit
`bf4725ba20876bfb1471f37464acc3727c3caa94` plus the uncommitted PR-2b
worktree. Author: Claude (test author; a different author from the IRQ asm/C).
State: **oracle implemented / tested locally (QEMU 8.2.2 TCG) / two canary
coverage gaps open (flagged to lead, asm NOT edited) / remote CI not observed**.
AI-assisted work; not independent human qualification.

Files (test author):

- `tools/smoke_arm.py`: `--timer-test` (mutually exclusive with every other
  mode). Independently re-judges the block that must directly follow the timer
  line: exactly 4 `irq tick` lines n=1..4; per tick vector==5, intid==0x1e,
  isr==0x80 (only ISR_EL1.I), SPSR M==EL1h, SPSR I=0 with D=A=F=1, no
  IL/SS/PAN/BTYPE/high bits; ELR inside the canary loop's IRQ-unmasked window
  `[msr daifclr #2 + 4, first msr daifset #2]` derived from the Image bytes at
  `je_arm_canary_loop` (ELF symbol has no `.size`; first word must be
  `sub sp,sp,#128`); sp identical across ticks, 16-aligned, inside
  `[__stack_bottom, __stack_top)`; cntpct strictly increasing, delta >=
  625000 (TCG lower bound only); heartbeat strictly increasing (eret-resume
  proof); `irq timer PASS ticks=4`; then exactly the 5-line BRK control
  (trigger, EXCEPTION vector 4 EC 0x3c ISS 0xb2b2, esr 0xf200b2b2 with
  ELR==`je_arm_irq_brk_site` whose Image word is BRK #0xb2b2 and SPSR EL1h
  DAIF=0xf, far=n/a with vbar==installed vbar, terminal). Any `double fault`
  text is a FAIL; irq lines outside `--timer-test` are a FAIL.
- `kernel/arch/aarch64/main.c`: print-only change — the tick line now also
  prints the already-recorded `elr=` field (needed for the ELR-window check).
- `tools/test_smoke_arm_timer.py` (new): replays the captured good serial log
  through unmodified `smoke_arm.py` via a fake-QEMU `--qemu` shim; baseline
  must PASS, 27 doctored logs must each FAIL with the expected reason.

Commands and observed results (local, QEMU 8.2.2, aarch64-linux-gnu-gcc 13.3.0):

- `make aarch64 ARM_TIMER_TEST=1 && make smoke-aarch64 ARM_TIMER_TEST=1
  ARM_REQUIRE=1` — PASS (ELR window `[0x40084b40, 0x40084dfc]`, sp
  `0x400cde40` stable).
- Same pair for default, `ARM_TIMER_POLL=1`, `ARM_MMU_FAULT=text|data|rodata|guard`,
  `ARM_VECTOR_TEST=1`, `ARM_DTB_BAD=1`, `ARM_BSS_CHECK=1`, `ARM_PHYSALLOC=1` —
  all PASS.
- `python3 tools/test_smoke_arm_timer.py` — PASS (baseline + 27 mutations
  rejected: frozen/decreasing heartbeat, vector=4, varying sp, sp off-stack,
  double-fault with/without FATAL, ELR out of window/misaligned, isr 0/0xc0,
  intid 31, SPSR EL1t / I set / A clear, cntpct non-monotonic / short delta,
  wrong n, 3 ticks, PASS ticks=5, BRK vector 5 / wrong ELR / I unmasked, stray
  irq line, line after terminal, log without flag, flag conflict).
- `make check` (includes ASAN/UBSAN hosted tests) — PASS. `git diff --check` — PASS.
- Throwaway asm mutants in a scratch copy (`/tmp/mp2b/mut`, not committed):
  `mov x16|x17|x18, #0` before `eret` -> `FATAL irq canary mismatch reg=16|17|18`
  -> smoke FAIL (the frozen-MP1 hazard IS caught). `mov x30, #0` before `eret`
  -> smoke **PASS** (gap 1). `x0` cleared only on the 4th tick's return ->
  smoke **PASS** (gap 2).

Open findings (for the lead; asm is owner-frozen, not edited by the test author):

1. x30 restore is effectively unverified: x30 is the CANARY_CHECK temporary,
   so a corrupted x30 is overwritten by the next `adrp` and finally reloaded
   from `[sp,#80]`; it is live as a canary for only ~3 of ~180 loop
   instructions. x29/x30 corruption is likewise masked inside the heartbeat
   and tick-count `stp/ldp [sp,#104]` windows. Proposed fix: temp-free compares
   (`cmp xN, #imm12, lsl #12` with distinct non-zero patterns such as
   `(0x800+N)<<12`), leaving only the heartbeat window masked and documented.
2. The 4th tick's restore is only partially verified: the loop exits on
   `ticks>=4` without a full post-tick check pass. Proposed fix: on seeing
   ticks>=4, `msr daifset,#2`, reload x29/x30 and run one complete x0..x30
   check before `.Lcanary_restore`.

Not covered / residual: x16-x18 are genuinely owned (never written in the
loop) and caught for every tick that is followed by a full check pass. No
hosted test of `je_arm_irq`: it lives in `main.c` with inline asm and console
dependencies and has no fake `je_gic_handle_one` seam; the JE_OK/BUSY/spurious
branching is proven only on QEMU (BUSY/spurious not exercised at all in the
live run). Moving `je_arm_irq` to `irq.c` would make a hosted fake possible.
The `ticks==4` pin can false-FAIL if a 5th tick lands in the 3-instruction
window between the count check and `daifset` (fail-closed, not observed).
Next bounded task: lead fixes gaps 1-2 (owner-approved asm), then rerun the
mutants above expecting `reg=30` and tick-4 FAILs; then remote CI.

## 2026-11-05T01:14:25Z — MP2 PR-2b returning IRQ implementation (single-agent worktree)

Observed repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios`,
branch `feat/mp2-aarch64-irq-return`, exact inspected commit
`bf4725ba20876bfb1471f37464acc3727c3caa94` plus uncommitted changes. State:
**implemented / tested locally / planned / not blocked**. The timestamp is
recorded at day precision because this entry was authored from the current
session; the exact runtime commands and values below are the evidence. Remote
CI and independent human qualification were not observed.

Verification refresh observed 2026-11-05T01:18:05Z at the same repository,
branch, and exact commit: **implemented / tested locally / planned / not
blocked**. The relocated IRQ common block was rebuilt and boot-tested; the
linked frozen common-plus-double-fault prefix compared byte-identically against
the committed vector object linked with the current non-vector objects.

Files changed:

- `kernel/arch/aarch64/vectors.S`: dedicated slot-5 veneer, returning saver,
  transient guard, frozen-frame restore, `eret`, and the test-only register
  canary loop. Slots 0-4 and 6-15 retain the original stub source; the
  existing fatal common path and stack-free double-fault body were not edited.
- `kernel/arch/aarch64/main.c`: returning handler, bounded tick evidence,
  entry-time ISR_EL1 sample, ARM_TIMER_TEST proof/markers, and BRK `0xB2B2`
  negative control.
- `kernel/arch/aarch64/gic.h`, `kernel/arch/aarch64/gic.c`: last-IAR getter
  populated from the one existing GICC_IAR read.
- `kernel/arch/aarch64/trapframe.h`: documents the distinction between the
  fatal MP1 scratch frame and complete PR-2b IRQ frames.
- `mk/aarch64.mk`: ARM_TIMER_TEST toggle/build directory/smoke pass-through.
- `docs/IMPLEMENTATION.md`: current status, evidence, workflow, and gaps.

Decision and invariant record: the dedicated veneer is required because the
frozen `mov w16,#idx` veneer would destroy interrupted x16. The new common path
saves all other GPRs before scratch use, checks EL1h from SPSR, writes vector 5
from an immediate, and sets the guard only after frame construction so a fault
inside `je_arm_irq` takes the existing double-fault path. On successful return
it clears the guard, restores ELR/SPSR and every GPR, uses `add sp,#304`
instead of loading `tf->sp`, and executes `eret`; no SPSel write is used.
The C handler has no allocation, blocking, console I/O, or unmask operation.
It treats `JE_ERR_BUSY` as an acknowledged non-tick wakeup and never re-reads
GICC_IAR. The canary loop preserves the caller's callee-saved registers,
deliberately keeps x16-x18/x30 live across an I-only unmask, increments a
heartbeat, checks all x0-x30, and exits after four successful IRQ ticks.

Commands and observed results:

- `make aarch64` — PASS; default image linked with DAIF.I still masked.
- `make aarch64 ARM_TIMER_TEST=1` — PASS; timer-test image linked under
  `-mstrict-align -mgeneral-regs-only -Werror -Wconversion`.
- `make smoke-aarch64 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=30` — PASS on QEMU
  8.2.2 for the default image; the existing MMU/GIC/timer markers and terminal
  marker were observed.
- Pinned direct timer-test boot:
  `timeout 8 qemu-system-aarch64 -M
  virt-8.2,gic-version=2,virtualization=off,secure=off,dtb-randomness=off
  -cpu cortex-a72 -smp 1 -m 256M -display none -serial
  file:build/aarch64-timertest/direct-serial.log -no-reboot -kernel
  build/aarch64-timertest/Image` — reached four vector-5 records, PASS,
  and the vector-4 BRK halt. The timeout exit `124` is expected because the
  kernel intentionally halts after the negative control; it is not treated as
  a smoke pass.
- `git diff --check` — PASS.

Observed timer-test records:

```text
1: vector=5 intid=0x1e isr=0x80 sp=0x400cde40 cntpct=0x1c5679 hb=0x11ca8
2: vector=5 intid=0x1e isr=0x80 sp=0x400cde40 cntpct=0x25fbc2 hb=0x2054d
3: vector=5 intid=0x1e isr=0x80 sp=0x400cde40 cntpct=0x2f91b2 hb=0x30856
4: vector=5 intid=0x1e isr=0x80 sp=0x400cde40 cntpct=0x3922e1 hb=0x43183
```

CNTPCT deltas were `632137`, `628208`, and `626991`; heartbeat deltas were
`59557`, `66313`, and `76077`. The BRK control emitted vector 4, EC `0x3c`, ESR `0xf200b2b2`, ELR
`0x40080fe0`, and `exception handled; halted`; no double-fault marker was
observed. The first run sampled ISR_EL1 after ACK/EOI and showed zero; it was
rejected and corrected before recording the evidence above.

Planned handoff: the different test author must add and run the
independent `--timer-test` smoke oracle (including ELF canary-loop range,
heartbeat monotonicity, interval lower bound, exact marker order, and absence
of double-fault), then rerun remote CI. `make check` and all prior local ARM
modes now pass at this worktree; remote CI remains unobserved. Next bounded
task: independent smoke/oracle integration and full regression verification.
Pass criteria: timer-test smoke accepts exactly four vector-5/IAR-30/ISR-I
records and the vector-4 BRK control, all prior modes remain green, and no new
fatal marker appears. The current smoke invocation fails at argument parsing
because `tools/smoke_arm.py` has not yet added `--timer-test`; that is an
inter-agent handoff, not a timer-path runtime failure.

## 2026-11-05T01:03:50Z — MP2 PR-2a test/oracle hardening after cross review (test author)

Observed repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios`,
branch `feat/mp2-aarch64-gic-timer`, exact inspected commit
`f8d7de6ab691acb1502908a61d61ec3623efa5fc` plus the uncommitted PR-2a
worktree (lead's gic.c contract changes + this patch). Author: Claude (test
author, a different agent from the gic.c implementor). State: **implemented /
tested locally / remote CI not observed / not blocked**. Polled proof only,
DAIF.I masked; this is AI-assisted testing, not independent human
qualification. Supersedes the "stale oracle" status of the 00:33:15Z entry
below (kept as history).

Lead contract changes now asserted by the hosted test:
`je_gic_handle_one` ISTATUS==0 returns `JE_ERR_BUSY` (EOIRs the exact IAR, no
tick, no re-arm); `interval > INT32_MAX` (and 0) refused with
`JE_ERR_INVALID` before any access; `je_gic_init` reads TYPER/IIDR/GICC_IIDR
before the first MMIO write (first GIC access must be a TYPER/IIDR READ;
identification refusals perform zero MMIO writes). The pre-hardening test
FAILED against the new gic.c (it expected firmware GICD_CTLR cleared on a
SecurityExtn refusal), so the update was required.

Changed files:

- `tests/test_gic_bits.c` (1503 checks, ASan/UBSan): fake now MODELS CNTP
  (TVAL write -> CVAL = CNTPCT + SignExtend(TVAL); ISTATUS = ENABLE && CNTPCT
  >= CVAL; PPI level = ISTATUS && !IMASK, reflected in ISPENDR0 bit 30), advances
  CNTPCT on each TVAL write, makes GICC_IAR a one-shot ack (later reads 1023),
  splits GICD_CTLR disable vs post-enable faults, checks EOIR at any width,
  ends EVERY case with `violations == 0`, and self-tests the violation
  detector and models. 300 GHz CNTFRQ asserts the deterministic masked
  outcome (freq 3647256576, interval 36472565). IAR read exactly once on
  timer/spurious/unexpected/busy paths.
- `kernel/arch/aarch64/gic.h` / `gic.c` (small, coordinated): `je_gic_evidence`
  gains `igroupr0` and `ipriority30`, filled from readbacks gic.c already
  performed. No behaviour change.
- `kernel/arch/aarch64/main.c`: three AT S1E1R probe lines after `mmu attr
  uart` and an extended config marker; the in-kernel probe check now also
  fails closed on GICD/GICC not identity Device or GICC_DIR translating.
- `tools/smoke_arm.py`: MMU block 7 -> 10 lines; GICD/GICC PAR F=0, PA==VA,
  ATTR=0x00, SH=0b10; GICC_DIR (GICC+0x1000) PAR F=1, FST=0x07 (L3
  translation); config `igroupr0` bit 30 == 0, `prio30 == 0x80 < PMR`; DTB
  timer cells validated in full (`<1 13|14|11|10 0x104>`, cpumask 0x01).

New/changed marker literals:

```text
JeiOS/aarch64: mmu xlate gicd va=<hex> par=<hex>
JeiOS/aarch64: mmu xlate gicc va=<hex> par=<hex>
JeiOS/aarch64: mmu xlate gicc_dir va=<hex> par=<hex>
JeiOS/aarch64: gic config gicc_ctlr=<hex> gicc_pmr=<hex> gicd_ctlr=<hex> isenabler0=<hex> igroupr0=<hex> prio30=<hex>
```

Observed (QEMU 8.2.2, GCC 13.3.0, pinned virt-8.2/cortex-a72/1 CPU/256M):
gicd par `0x8000b00`, gicc par `0x8010b00`, gicc_dir par `0x80f`,
igroupr0 `0x0`, prio30 `0x80`; poll cntpct `0x198891, 0x2326fe, 0x2cc2b4,
0x365d63` (deltas 630381/629686/629423 >= 625000; TCG lower bound only).

Commands run locally (all PASS): `make check`;
`ASAN_OPTIONS=detect_leaks=0 make check`; `make smoke` (x86);
`git diff --check`; `make aarch64 [t] && make smoke-aarch64 [t] ARM_REQUIRE=1
ARM_SMOKE_TIMEOUT=60` for default, `ARM_BSS_CHECK=1`, `ARM_VECTOR_TEST=1`,
`ARM_DTB_BAD=1`, `ARM_PHYSALLOC=1`, `ARM_TIMER_POLL=1`,
`ARM_MMU_FAULT=text|data|rodata|guard`. Remote CI not observed.

Mutation battery (local, scripted copies of gic.c; not committed): 22 of 23
killed, each by its targeted assertion — sample-after-rearm, TVAL=0 /
negative / deleted / CTL-toggle instead of TVAL (killed by the modelled
ISTATUS check), deleted post-enable and disable GICD_CTLR readbacks, double
IAR read (timer use-first/use-second, unexpected, busy), spurious EOIR 8- and
32-bit, BUSY->IO, busy bumps ticks, busy skips EOIR, missing INT32_MAX arg
check, write-before-identify, 300 GHz refuse, FIQEn, igroupr0 evidence from
constant. Survivor: `ipriority30` evidence taken from the constant — an
EQUIVALENT mutant (init refuses any readback != 0x80, so a successful init
always reports 0x80); the oracle's prio30 check therefore guards marker
emission only. Oracle mutants on the captured log/fixture (12/12 killed):
GICD ATTR Normal, GICC wrong PA / faulting, GICC_DIR mapped / permission
fault / wrong VA, deleted probe line, prio30 0x40, igroupr0 bit 30, DTB flags
0x304 / 0x004 / 0x10104.

Not enforced / open: the in-kernel GIC probe fail-closed branch was not
exercised by a mutant boot (only the oracle side was mutated); the fake models
CNTP/GIC behaviour from the spec and is not proof QEMU or hardware matches;
no live spurious injection; no IRQ vector path or unmask (PR-2b).

Next bounded task: lead review of this patch, then PR-2a commit/PR; PR-2b
(returning IRQ vector, first DAIF.I unmask) reuses `je_gic_handle_one` and must
treat `JE_ERR_BUSY` as handled-not-fatal.

## 2026-11-05T00:33:15Z — MP2 PR-2a GICv2 + CNTP polled proof

Observed repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios`, branch
`feat/mp2-aarch64-gic-timer`, exact inspected commit
`f8d7de6ab691acb1502908a61d61ec3623efa5fc` plus the uncommitted worktree.
State: **implemented / tested locally / planned / not blocked**. PR-2a remains
strictly polled with DAIF.I masked; vectors, exception assembly and IRQ
unmasking are untouched and remain PR-2b.

Changed files:

- `kernel/arch/aarch64/gic.c` implements the frozen `gic.h` contract: GICD/GICC
  Device MMIO, IIDR/SecurityExtn/group/priority/enable/PMR/CTLR refusals and
  readbacks, CNTP arm, exact-IAR ACK/EOI handling, bounded pending polling and
  counters/readback helpers.
- `kernel/arch/aarch64/main.c` maps GICD/GICC as two 4 KiB Device leaves,
  appends `gicd=<hex> gicc=<hex>` to the MMU map marker, performs bring-up after
  MMU in every build, and provides the `JE_ARM_TIMER_POLL` four-cycle proof
  body. It deliberately does not inject a live spurious INTID; the hosted fake
  test is the intended proof for no-EOIR spurious handling.
- `mk/aarch64.mk` adds `gic.o` and its compile rule. The independent test author
  still needs to wire the poll build selector and update `tools/smoke_arm.py`.
- `docs/IMPLEMENTATION.md` records the current evidence and limits.

Exact new marker literals, in order:

```text
JeiOS/aarch64: gic init gicd_typer=<hex> gicd_iidr=<hex> gicc_iidr=<hex>
JeiOS/aarch64: gic config gicc_ctlr=<hex> gicc_pmr=<hex> gicd_ctlr=<hex> isenabler0=<hex>
JeiOS/aarch64: timer cntfrq=<hex> interval=<hex> intid=0x000000000000001e
JeiOS/aarch64: timer poll n=<hex> intid=0x000000000000001e cntpct=<hex> ticks=<hex>
JeiOS/aarch64: timer poll PASS cycles=0x0000000000000004
JeiOS/aarch64: initialization complete; halted
```

Observed pinned QEMU evidence (QEMU 8.2.2, GCC 13.3.0,
`virt-8.2,gic-version=2,virtualization=off,secure=off`, `cortex-a72`, one
CPU, `-m 256M`):

```text
gicd_typer=0x0000000000000008 gicd_iidr=0x000000000000043b gicc_iidr=0x000000000002043b
gicc_ctlr=0x0000000000000001 gicc_pmr=0x00000000000000ff gicd_ctlr=0x0000000000000001 isenabler0=0x000000004000ffff
cntfrq=0x0000000003b9aca0 interval=0x0000000000098968 intid=0x000000000000001e
```

The poll-toggle QEMU serial log reached all four proof lines and the terminal:
`cntpct=0x24777d, 0x2e0cf5, 0x37adb5, 0x413fdd`, `ticks=1,2,3,4`; deltas
`628088, 630976, 627240` were all at least `625000`. This is a polled
acknowledge/handle/re-arm/EOI proof, not evidence of vector delivery or
precision interrupt latency.

Commands/evidence:

- `make aarch64` — PASS.
- Separate poll-toggle strict build — PASS.
- `make smoke-aarch64 ARM_REQUIRE=1 ARM_SMOKE_TIMEOUT=30` — QEMU booted
  through the terminal, but the command correctly exited FAIL because the
  current smoke oracle still expects the old MMU map marker. The poll-toggle
  run likewise reached the complete terminal and then failed only on that
  stale oracle expectation.
- `make check` — PASS (hosted/unit suite, ELF checks and image checks).
- `make smoke` — PASS (existing x86 QEMU regression).
- `git diff --check` — PASS at this observation.
- Remote CI is not observed for this worktree. The independent hosted GIC tests
  and updated ARM oracle remain planned work owned by the other author.

Open hazards/not enforced: no live spurious injection in QEMU; no IRQ vector
path or DAIF unmask; no SMP/per-CPU GIC support; no physical ARM hardware;
QEMU TCG does not model real interrupt latency; AI-assisted review is not
independent human qualification.

Next bounded task: the different test author adds the fake-MMIO GIC tests,
updates the smoke map/leaf oracle for two Device leaves, and wires the
`JE_ARM_TIMER_POLL` build. Pass criteria are exact complete-line marker order,
independent refusal/readback/ACK-EOI coverage including IAR==1023 with no EOIR,
preservation of MP1/PR-1a markers, and a green pinned-QEMU smoke result. PR-2b
then owns the dedicated returning IRQ vector and the first IRQ unmask.

## 2026-11-04T19:10:58-05:00 — dynamic clustering roadmap

Inspected main `b1ebbdab6d89dc581e4cd96e317460ab597e3dfb` (merged MP2 PR #81);
branch `docs/dynamic-clustering`. Implemented: planning documentation only.
Added CLUSTERING.md and links in README, ROADMAP, AGENT_START and ACCELERATORS.
Planned: personal device pools through enterprise distributed compute with
explicit opt-in, programmable policy, scoped authority and staged failure tests.
Core #82, mobile #14 and roadmap #9 track it. No new dependencies, runtime
clustering, protocol freeze or changes to tested kernel/ADR contracts.
Next bounded cluster task: CL0 threat model and versioned job/membership/authority
contracts, then CL1 deterministic fault harness. Existing execution milestones
remain authoritative; this plan does not claim MP2 completion.

Validation: `make check` exited 2 at environment LeakSanitizer inspection
(ptrace restriction); `ASAN_OPTIONS=detect_leaks=0 make check` passed, exit 0,
leak checking disabled. Local Markdown links and `git diff --check` passed.
No boot changes; smoke not rerun. No remote CI for this change observed.
Single-agent planning contribution, not independent human qualification.

## 2026-11-03T10:52:23+00:00 — MP2 PR-1b W^X/guard fault proofs + hardened MMU oracle (test author)

Observed repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios`,
branch `feat/mp2-aarch64-mmu-enable`, inspected commit
`b58c84951df6e239ff40c55c57e8ab6034c898f7` plus the uncommitted PR-1b worktree
(implementor's patch + this test patch). Author: Claude (test author, a
DIFFERENT agent from the MMU implementor, per the AGENTS.md split). State:
**implemented / tested locally / remote CI not observed / not blocked**.
Supersedes the "Next bounded task" of the 10:37:28 entry below (that entry is
kept as history).

Added / changed:

- `mk/aarch64.mk`: `ARM_MMU_FAULT=text|data|rodata|guard` toggle; own build dir
  `build/aarch64-mmufault-<name>`; joins the mutually-exclusive group (any
  non-empty value counts as set; an unknown name is a make error). The smoke
  target passes `--mmu-fault <write-to-text|execute-from-data|execute-from-rodata|stack-guard>`.
- `kernel/arch/aarch64/main.c` (`#ifdef JE_ARM_MMU_FAULT` only): after the MMU
  block, prints `mmu fault test <name> target=.. par_r=.. par_w=..` (AT S1E1R/W
  of the target), performs ONE forbidden access (labelled `str xzr` site or
  `blr` to a `ret`), and on no fault prints FATAL. `je_arm_exception` asserts
  vector 4, EC, IL=1, FSC=ISS[5:0], WnR, S1PTW/CM/EA/FnV=0, FAR, ELR and SPSR
  EL1h+DAIF, then prints a distinctive PASS line. The execute-from-data case
  first proves the buffer RW by write/read-back.
- `tools/smoke_arm.py`: the implementor's MMU check was regex-only (any hex
  accepted). Replaced with an independent oracle in EVERY good-boot mode:
  map line == ELF symbols; guard == `__data_end` == `__stack_bottom`-4 KiB;
  root a window page outside the image; audit leaf count recomputed (region
  pages + table frames, fixed-point over L0..L2 prefixes assuming first-fit
  contiguous table frames; a non-contiguous allocator FAILs, never false
  passes); SCTLR == 0x30D00808|M|C|I|WXN exactly, EE=0; mapped PAR F=0,
  PA==VA frame, ATTR=0xff, SH=0b11; guard PAR F=1, FST=0x07; UART PAR F=0,
  PA==VA, ATTR=0x00, SH=0b10. `--mmu-fault` re-decodes the raw ESR fields,
  FAR/ELR/VBAR against ELF symbols, binds ELF to Image (RET at targets,
  `STR XZR,[Xn]` at the store site), and checks the AT probes.
- `kernel/arch/aarch64/paging.c`: test-only seam under
  `JE_ARM_PAGING_HOSTED_TEST` (exports `je_arm_leaf_desc_hosted`, compiles out
  the AT asm). No change to the ARM build; frozen `paging.h` untouched.
- `tests/test_paging_bits.c` + `Makefile test:`: hosted ASan/UBSan test. Maps
  the ARM window at 0x40000000 (MAP_FIXED_NOREPLACE; failure is a FAIL) so the
  real builder/audit run. Checks MAIR/TCR/SCTLR field by field; every class's
  leaf against an independent DDI0487 spec encoder; bad builds incl. a text
  region over allocator frames and NOMEM with full rollback; 23 audit tamper
  rejections each followed by restore + audit OK; enable gating and exact
  enable-leaf arguments.
- `.github/workflows/ci.yml`: one step running the four fault modes.

Observed values (QEMU 8.2.2, GCC 13.3.0, pinned virt-8.2/cortex-a72/256M):

```text
write-to-text       ESR=0x9600004f FAR=0x40080f90 ELR=0x40080ff0 (store site) par_r=0xff00000040080b80 par_w=0x081f (F=1 FST=perm L3)
execute-from-data   ESR=0x8600000f FAR=ELR=0x40087000              par_r=par_w=0xff00000040087b80 (RW)
execute-from-rodata ESR=0x8600000f FAR=ELR=0x40086b60              par_r=0xff00000040086b80 par_w=0x081f
stack-guard         ESR=0x96000047 FAR=0x400c8000 ELR=0x40080ff0  par_r=par_w=0x080f (F=1 FST=xlate L3)
```

Commands run locally (all PASS): `make check`; `git diff --check`;
`make aarch64 [toggle] && make smoke-aarch64 [toggle] ARM_REQUIRE=1` for
default, `ARM_PHYSALLOC=1`, `ARM_MMU_FAULT=text|data|rodata|guard`,
`ARM_VECTOR_TEST=1`, `ARM_DTB_BAD=1`, `ARM_BSS_CHECK=1`. `make smoke` (x86)
was NOT rerun by this test patch (no x86 file changed). Remote CI: not observed.

Mutation evidence (scratch copy, not committed): rodata PXN=0 -> default smoke,
audit and data test all still PASS; only `ARM_MMU_FAULT=rodata` FAILs ("did
not fault") and `test_paging_bits` FAILs. Guard mapped -> default smoke FAILs
on the leaf count, guard mode "did not fault". Guard mapped RO with the kernel
comparator also mutated to expect a permission fault -> kernel printed PASS,
harness FAILed on the raw ESR. SCTLR without WXN -> kernel self-check passes
(same constant), harness FAILs. UART AttrIdx 0 -> harness FAILs on PAR.ATTR,
hosted test FAILs on the encoder. AF/nG/device-SH encoder mutants and removing
the audit's W+X+alias checks FAIL the hosted test.

Findings from cross-lineage review — RESOLVED by the lead (transcripts +
resolution: docs/adr/debates/0011-mp2-pr1b-review/):

1. FIXED. `je_arm_mmu_audit` originally compared each leaf against the SAME
   `leaf_descriptor()` the builder used, so an encoder bug passed the audit
   (observed: rodata PXN=0, UART AttrIdx 0). It now compares the WHOLE
   non-address part of each leaf against a per-class LITERAL
   (TEXT/RODATA/RW/DEVICE) and requires OA < 2^44 — so any stray bit
   (Contiguous/DBM/NS/software) is rejected too, not just the named fields. The
   Contiguous-bit W^X hole and the rodata-PXN=0 bug now FATAL-halt at the boot
   audit (validated by mutation).
2. FIXED. The audit now requires every table descriptor's output frame to be one
   of the builder's own `table_pa` frames, closing the "table pointer redirected
   into a .data frame passes, then post-enable writes rewrite live translations"
   hole. (The install-time `record_frame_flags` ledger remains defence-in-depth;
   identity-uniqueness + this table-membership check are the load-bearing
   enforcement for the identity-only MP2 map.)
3. FIXED. The audit now requires VA==PA for region leaves too (not only table
   frames) — the identity contract (ADR 0011 D2).
4. FIXED (related). A PARange below the 44-bit profile now FATAL-halts in
   `mmu_bringup` (ADR D3/D7, AGENTS rule 5); previously IPS was hardcoded 44 with
   no refusal. No effect on cortex-a72 (reports 44).

Lower / recorded (not code-changed): no write-to-.rodata deliberate-fault mode
(covered by ARM_MMU_FAULT=rodata + the hosted test, not default boot); the DC
IVAC range is [_text,_end) vs the ADR's [__data_start,_end)+DTB (wider is safe);
the smoke leaf-count oracle pins the count + first-fit layout, not full table
placement; the enable assembly is argument-tested only (cache/barrier
maintenance is review-only under TCG); SMP will need the TLBI …IS broadcast
variants (single-core NSH is correct now).

NOT enforced / honesty: QEMU TCG has no caches, so none of this is evidence for
`DC IVAC`/barriers/TLB/I-cache maintenance. The expected ESR pins ISV=0, which is
QEMU 8.2 behaviour for EL1-taken aborts; hardware may set ISV (kernel-side check
deliberately ignores ISS[24:11]; the harness pins it for the QEMU profile). The
leaf-count oracle assumes first-fit contiguous table frames. AI-assisted test
authoring/review is not independent human qualification.

Next bounded task: PR-1b is complete; later MP2 steps are the GIC + architected
timer + scheduling, then MP3 (the per-process je_vm_ops backend + the high-half
TTBR1 kernel VA split).
and observe remote CI for the new `ARM64 MMU W^X and stack-guard faults` step.

## 2026-11-03T10:37:28+00:00 — MP2 PR-1b aarch64 MMU enable

Observed repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios`,
branch `feat/mp2-aarch64-mmu-enable`, exact inspected commit
`b58c84951df6e239ff40c55c57e8ab6034c898f7` plus an uncommitted worktree patch.
State: **implemented / tested / planned / not blocked**. This is PR-1b stacked
on PR-1a; ADR 0011 D2/D3/D4/D6/D7 are pinned and were not relitigated.

Implemented files and contracts:

- `kernel/arch/aarch64/paging.c`: symbolic `_Static_assert`-checked
  `MAIR_EL1=0x00000000000000ff`, `TCR_EL1=0x0000000480903510` and
  `SCTLR` enable bits `0x0000000000081005`; zeroed L0-L3 tables from
  `je_pages`; 4 KiB identity leaves; table-frame RW mappings; TEXT/RODATA/RW/
  DEVICE descriptor classes; install-time W^X physical-frame tracking;
  descriptor walk audit; `AT S1E1R/W` + `PAR_EL1`; root accessor.
- `kernel/arch/aarch64/paging_enable.S`: one stack-free leaf. It invalidates
  the table range and image superset range with `DC IVAC` using
  `CTR_EL0.DminLine`, then performs the pinned DSB/ISB, MAIR/TCR/TTBR,
  `TLBI VMALLE1`, one `SCTLR_EL1` OR/write for M|C|I|WXN, and `IC IALLU`
  sequence. The image range includes `.data`, `.bss`, the DTB copy and stack.
- `kernel/arch/aarch64/main.c`: builds text/rodata/data/stack/PL011 regions,
  audits/enables in every ARM build, emits the exact default marker block,
  and checks mapped identity translation, guard translation fault and UART
  Device attribute.
- `kernel/arch/aarch64/linker.ld`: adds page-aligned `__data_start`.
- `mk/aarch64.mk`: links and compiles `paging.o` and `paging_enable.o`.
- `tools/smoke_arm.py`: requires the MMU marker block and accepts it between
  the PR-1a physmem block and the terminal/fault proof.
- `docs/IMPLEMENTATION.md`: current implementation/evidence status.

Exact default marker lines observed in order:

```text
JeiOS/aarch64: mmu build root=0x0000000040000000
JeiOS/aarch64: mmu map text=[0x0000000040080000,0x0000000040086000) rodata=[0x0000000040086000,0x0000000040087000) data=[0x0000000040087000,0x00000000400c8000) device=0x0000000009000000
JeiOS/aarch64: mmu audit ok leaves=0x0000000000000053
JeiOS/aarch64: mmu enabled sctlr=0x0000000030d8180d
JeiOS/aarch64: mmu xlate mapped va=0x0000000040080000 par=0xff00000040080b80
JeiOS/aarch64: mmu xlate guard va=0x00000000400c8000 par=0x000000000000080f
JeiOS/aarch64: mmu attr uart va=0x0000000009000000 par=0x0000000009000b00
```

Verification actually run locally:

```text
make check                                      PASS
ASAN_OPTIONS=detect_leaks=0 make check          PASS
make smoke                                      PASS
make aarch64                                    PASS
make smoke-aarch64 ARM_REQUIRE=1               PASS
make smoke-aarch64 ARM_BSS_CHECK=1 ARM_REQUIRE=1 PASS
make smoke-aarch64 ARM_VECTOR_TEST=1 ARM_REQUIRE=1 PASS
make smoke-aarch64 ARM_DTB_BAD=1 ARM_REQUIRE=1 PASS
make smoke-aarch64 ARM_PHYSALLOC=1 ARM_REQUIRE=1 PASS (oracle 0xfeb3 pages)
git diff --check                                PASS
```

The ARM build used GCC 13.3.0 and QEMU 8.2.2 with the pinned
`virt-8.2,gic-version=2,virtualization=off,secure=off,dtb-randomness=off`,
`cortex-a72`, one CPU and 256 MiB RAM command generated by
`tools/smoke_arm.py`. The linker still reports the known single-ELF RWX
segment warning; descriptor-level W^X is audited and observed, while ELF
program-header separation remains future work.

Honesty and open issues: QEMU TCG has no cache model, so the `DC IVAC`,
barrier, TLBI-freshness and instruction-cache claims are reviewed, not
observed by smoke. QEMU did observe the identity translation, guard
translation fault and Device UART attribute. The separate W^X audit/fault-mode
author still needs to add deliberate write-to-text, execute-from-data,
execute-from-rodata and exact ESR/FAR proofs. No remote CI result, physical ARM
execution, SMP, userspace, dynamic loading, arbitrary-driver support or
independent human qualification is claimed. PR/issue publication state was not
changed.

Next bounded task: add the independent W^X/fault-mode tests against this frozen
builder and marker contract. Pass criteria are exact ordered complete-line
markers, permission-vs-translation ESR/ISS/FAR evidence, preservation of the
default/MP1/PR-1a markers, and no marker after an expected halted fault.

## 2026-11-03T04:24:29+00:00 — MP2 PR-1a aarch64 physical allocator bring-up

## 2026-11-03 — MP2 PR-1a aarch64 physical allocator bring-up (MMU still OFF)

Observed repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios`,
branch `feat/mp2-aarch64-mmu`, built on commit
`f09fd928495ab6b511f4fa58bf4cd751024a7b33` (ADR 0011 accepted). State:
**implemented / tested locally / not blocked**. First of the stacked MP2 PR-1
pair from ADR 0011 (D-SPLIT); PR-1b (MMU enable) is the follow-up and is NOT in
this change. No page tables, no TCR/TTBR/SCTLR/MAIR, no W^X/linker symbols.

Implemented:
- `include/jeios/boot.h` + `include/jeios/alloc.h`: per-arch compile-time
  physical window `JE_PHYSICAL_BASE`/`JE_PHYSICAL_LIMIT`/`JE_PHYSICAL_SPAN`
  (`JE_ARCH_AARCH64` → 0x40000000/0x50000000; else 0/0x40000000, byte-identical
  to the pre-ADR-0011 x86 behavior). `JE_PAGE_COUNT` now sizes to the window
  SPAN, not the absolute limit. Added `je_page_free_pred` + `je_pages_init_pred`.
  (ADR 0011 D5 — owner-approved tested-path change.)
- `kernel/core/alloc.c`: default-deny `je_pages_init_pred` over a free-page
  predicate; `je_pages_init` now wraps it with the x86 `je_boot_context`
  predicate. alloc/free are base-relative (`JE_PHYSICAL_BASE + i*PAGE_SIZE`);
  `je_pages_free` rejects out-of-window addresses via the single unsigned test
  `(a-BASE) >= SPAN` BEFORE deriving an index (prevents an OOB `state[]` write).
  Compile-time `_Static_assert`s on the window shape.
- `kernel/arch/aarch64/physmem.{h,c}`: the frozen adapter contract + its
  implementation. `je_arm_page_free` is the default-deny predicate: a page is
  free iff its whole [pa,pa+4096) lies in exactly one FDT RAM range AND
  intersects none of the kernel image, DTB source, initrd (if present), or any
  kept reservation (incl. no-map). Half-open intersection, 64-bit, with a
  top-of-address-space overflow guard so the predicate is total. `rsv_dynamic`
  has no address → nothing to subtract (obligation recorded for a later PR).
- `kernel/arch/aarch64/main.c`: file-scope `je_arm_facts` (stored by
  `dtb_intake()` after validation) and `je_arm_pages` (128 KiB `.bss`, inside
  `[_text,_end)` so the kernel-image reservation covers its own state).
  `physmem_bringup()` runs in EVERY build after `dtb_intake()`, emits ordered
  markers, and FATAL-halts on any failure.
- `mk/aarch64.mk`: physmem.o + the arch-neutral alloc/bootctx/memory objects
  added to the ARM link; build-neutral `ARM_PHYSALLOC` smoke selector.
- `tests/test_physmem.c` (hosted, ASAN+UBSAN) + `tools/smoke_arm.py`
  `--phys-alloc` oracle, written by a different author than the kernel code;
  plus `tests/test_physmem_arm.c` (hosted at the aarch64 nonzero window,
  `-DJE_ARCH_AARCH64`) added after cross-review to exercise the base-relative
  alloc/free arithmetic and out-of-window rejection under ASan — the one gap both
  code reviews independently flagged (base-0-only tests left it uncovered).

Cross-lineage review fixes (transcripts: docs/adr/debates/0011-mp2-pr1a-review/):
the nonzero-base hosted test above; a `range_intersects_page` overflow branch so
the predicate fails SAFE on a wrapped reserved range (header promises totality);
a `#error` guard so an aarch64 TU without JE_ARCH_AARCH64 cannot silently pick
the x86 window; a `physmem unmanaged ram bytes=<hex>` degraded-profile marker
(AGENTS rule 5) that the oracle recomputes independently (verified 0x10000000 at
-m 512M); a CI `ARM_PHYSALLOC=1` step; and the "byte-identical" x86 claim
corrected to "behavior-identical, alloc/free/heap code identical at -O2".

Marker honesty note (important): an earlier draft emitted a `physmem rollback ok`
marker by requesting `free_count+1` pages — but on the 65k-page window that
exceeds the `JE_ALLOC_MAX_PAGES=256` API cap, so it returned `JE_ERR_INVALID`
without ever running the allocator scan: a VACUOUS proof. It was replaced with
two honest markers: `physmem allocmax pa=<hex>` (a real 256-page contiguous run
IS allocated via the scan, then freed, free_count restored) and
`physmem overcap rejected pages=<hex>` (an over-cap request is rejected as
INVALID with state unchanged — labeled a BOUNDS check, not a rollback claim).
The real "failed multi-page alloc frees nothing" rollback property is proven in
`tests/test_physmem.c` on a small fragmented window (free_count>=n, no run of n;
NOMEM leaves `state[]` byte-identical) — the only place it can be set up cheaply.

Fixture limitation (recorded honestly): at `-m 256M` the single FDT RAM range
covers the entire allocator window, so a predicate bug that DROPS the
RAM-membership check is invisible to the ARM smoke oracle (the computed free
count is unchanged). Only the hosted test catches it (it builds RAM ranges with
gaps). The cross-lineage check confirmed the oracle's value: a predicate that
ignores the DTB reservation passes default smoke but FAILS `--phys-alloc`
(0xffc6 vs 0xfec6).

Tested locally at this observation (QEMU 8.2.2; local only, NOT CI):

```text
ASAN_OPTIONS=detect_leaks=0 make check          PASS (incl. test_physmem)
make aarch64                                    PASS
make aarch64 ARM_BSS_CHECK=1 / VECTOR_TEST=1 / DTB_BAD=1   all PASS
make smoke-aarch64 ARM_REQUIRE=1                PASS
make smoke-aarch64 ARM_PHYSALLOC=1 ARM_REQUIRE=1  PASS (oracle 0xfec6 == kernel)
git diff --check                                PASS
```

The ARM link still emits the pre-existing `LOAD segment with RWX permissions`
warning; W^X is PR-1b. Value-carrying markers observed at the pinned fixture:
window [0x40000000,0x50000000); kernel reserve base 0x40080000; dtb reserve base
0x48000000 size 0x100000; free pages 0xfec6 (65222 = 65536 − 58 kernel − 256
dtb); alloc1 pa 0x40000000; allocmax pa 0x400ba000; overcap rejected 0xfec6.

AI-assisted multi-agent work (Codex implemented the kernel path, Claude wrote
the tests + oracle, cross-lineage reviewed); this is NOT the independent human
qualification the critical profile requires, and no remote CI is observed yet.

Next bounded task: PR-1b (MMU enable) per ADR 0011 D2/D4/D6/D7 — identity map in
TTBR0, W^X per-section, guard page, the corrected invalidate-only + TLBI/IC
enable leaf, and the AT/PAR + fault-mode proofs. Pass criteria: non-identity
translation proven via AT/PAR; write-to-code / execute-from-data /
execute-from-rodata / stack-guard faults with exact ESR/FAR; the pre-enable W^X
audit; MP1 + PR-1a markers still pass.

## 2026-11-02T01:09:05-05:00 — native privilege tools, vi and developer environment

Inspected core main `d76a201510bb79a579719ffb8ad43eed9d0d02ba` (PR #77 merged);
branch `docs/native-security-tools`. Implemented: documentation only in
NATIVE_PRODUCTS, README, ROADMAP, AGENT_START, SHELLS, SECURITY_KEYS and HANDOFF.
Planned: stable-source doas enabled by default after qualification; optional sudo
disabled without authority; nvi2 as planned vi implementation; Binutils/NASM,
build tools and compiler/runtime dependencies. Added shared hardening and
feature-preservation gates, critical elevation tests and explicit ISA/dialect
coverage. Tracker #75 updated. No source imports, ABI/security invariant changes
or native execution claims. Next: #75 bounded source/provenance/dependency
inventory and feature/test matrices; existing MP2/M5a/shell priorities stand.

Validation on this documentation branch: `make check` exited 2 at LeakSanitizer
environment inspection (ptrace restriction). `ASAN_OPTIONS=detect_leaks=0 make
check` passed, exit 0, leak detection disabled. Local Markdown link checks and
`git diff --check` passed. No boot changes; smoke not rerun. Remote CI for this
change not yet observed. Single-agent work; no independent human qualification.

## 2026-11-01T22:22:44-05:00 — coreutils preference clarification

Inspected main `da4179d437670f2ac015fa2cef57b07bcb55536b` after PR #76 merged.
Planned policy: stable GNU C coreutils is the preferred baseline and default
provider. Review uutils release changes only for genuine improvements supported
by comparative evidence; selectively adapt those changes in C. No parallel Rust
provider or full uutils C rewrite is planned. Updated NATIVE_PRODUCTS and portfolio
links in README, ROADMAP, SHELLS, SECURITY_KEYS and AGENT_START; tracker #75 updated.
No runtime behavior or security invariants changed. Next: #75 source/dependency
inventory with exact pins and per-change acceptance evidence.

Validation: `make check` exited 2 because LeakSanitizer cannot inspect this
environment; `ASAN_OPTIONS=detect_leaks=0 make check` passed (exit 0; leak checking
disabled). `git diff --check` passed. No boot change; smoke not rerun. Native
product imports and execution remain planned, not qualified.

Owner clarification: existing C++ is allowed in included upstream products;
retain native compatibility/integration patches. Core C11 and coreutils C-only
remain distinct requirements. Tracker: #75.
Validation: `make check` blocked by environment LeakSanitizer inspection;
`ASAN_OPTIONS=detect_leaks=0 make check` passed (exit 0, leaks disabled).
Changed Markdown links and `git diff --check` passed. Docs-only; no emulator
rerun, source import or native product support claim. Single-agent contribution.

## Native product portfolio — observed 2026-11-01T22:08:31-05:00

Inspected core main `2c5bdb01c3726dbe551492f6d2ecd0e30b66308e`; shell PR #74 merged. Branch docs/native-products. Added NATIVE_PRODUCTS.md and roadmap/shell/FIDO entry links. Implemented: policy docs only. Planned: included native product portfolio; LibreSSL default with explicit OpenSSL consumers; C-only coreutils adaptation tracking GNU and uutils independently. No dependency import/runtime/ABI change. Other products require language inventories; a blanket compiler rewrite is not assumed. Next: reviewed dependency/provenance/qualification matrix. MP2 remains next execution gate. PowerShell deferred. Exact local validation recorded below before publication; no native package or new remote CI result claimed.

Refresh validation: Core `make check` hit environment LeakSanitizer inspection failure; `ASAN_OPTIONS=detect_leaks=0 make check` passed. Leak detection disabled. Local Markdown links and `git diff --check` passed. No emulator rerun for this docs-only change; cited remote CI is for inspected main, not the new documentation head. Board summary/history refreshed; shell issues #71–#73 and mobile #12 added Todo; M4 #53 Done. Single-agent docs contribution, no independent human qualification. Branch `docs/native-shell-refresh`; no ABI, runtime, dependency import or mobile pin change.

## Historical ecosystem status — observed 2026-11-01T21:30:30-05:00

Core main `0f702036c52d40d03bb4c8919d0dcae53ba9f2b1`: AMD64 M3 and scoped M4 complete; PR #70
merged. ARM64 MP1 complete (#52); MP2 then MP3 remain. Main CI passed:
https://github.com/auto44e/jeios/actions/runs/41376309893 . M4 qualifies bounded
supervisor/broker/COM2 UART scenarios, not arbitrary hardware, DMA or a release.
Mobile main `f1d9b0d87746e9dc353a2f48e7b9d27e7fe86d07` has passing planning CI:
https://github.com/auto44e/jeios-mobile/actions/runs/39243515951 . Its core pin
remains older M1/M2; upstream progress does not qualify that integration.
No open PRs were observed before this refresh. M4 #53 is now closed as completed;
MP2 (mobile #2) is next execution work; M5a #54 remains planned.

Shell roadmap: SH1 recovery #71, SH2 original native sh #72, included native
Bash/Zsh and custom-shell registration #73; mobile parity #12. PowerShell deferred.
All shell implementation remains planned. Storage, networking, desktop, POSIX
completion, LDAP, encryption, fleet/ISO 27001 features, RISC-V, accelerators and
Android execution remain later gates. Standards adoption is not certification.
This current status supersedes older next-task/status wording below; timestamped
records remain evidence at their original revisions, not new test runs.


## M4 PR-I (#53) — brokered UART service + D9 timeout/detach/revoke/UNSUPPORTED — observed 2026-11-02T01:12:33Z

Branch `feat/m4i-uart`, based on main `af17af5` (#69 merged; main CI green);
integrated head `81e31c7`. Opened as the PR-I pull request — the FINAL M4 PR. Built
as impl → continuation (fixed a phase-RUNNING stall) → code review → evidence fix,
each cross-reviewed; the lead mutant-verified the proofs.

State: **implemented / tested / planned / not blocked**.

- **Implemented:** supervisor-owned healthy-incarnation PING/PONG before HANG
  and after every restart; kernel HANG acknowledgement before the timeout PING;
  non-closing HANG behavior with bounded timeout/kill; full before/after
  hardened-refusal snapshots including COM2 bitmap and IRQ3 mask; identity-based
  per-client cap attribution; exact exit-code checks; generation-validated
  SERVICE_BIND; revoke-send ordering; separate admin-revoked-owner death
  accounting; cleanup of endpoint/device/cap handles; actual partial-write
  lengths; and the gen-3 `I3` recovered-TX proof.
- **Tested:** `ASAN_OPTIONS=detect_leaks=0 make check`, `make smoke`,
  `make integration`, explicit NX-present and NX-absent smoke profiles,
  `make aarch64 && make smoke-aarch64 ARM_REQUIRE=1`, and `git diff --check`
  all passed. Final mode 15 reached all kernel corroboration markers and
  QEMU's independent COM2 file contained `b'I1I2I3'`; `D!` and `R!` were
  absent. Required four A/B mutants all failed their corresponding proof and
  were restored; exact outputs are in `WORKER_NOTES.md`.
- **Planned:** **M4 is complete** (PR-A..PR-I + the frame-zero security fix all
  merged or open). Next: **MP2** (mobile milestone #2). **Blocked:** no local
  implementation blocker; remote CI and the independent human gate (required for
  any hardened/release qualification claim) are unobserved and not claimed.

**Review record:** `docs/adr/debates/m4i-reviews.md` (design + code review; the
evidence-gap fixes), worker notes in `docs/adr/debates/m4i-workers.md`.

Changed invariants: mode 15 cannot complete until each healthy incarnation
answers its generation-tagged PING, HANG has been kernel-applied and timed out
under the bounded scenario budget, the normal client has exactly three
identity-attributed brokered caps, the hardened client has zero, both client
exit codes are exact, the admin-revoked owner fault is consumed against the
matching identity, and final QEMU COM2 evidence is `I1I2I3` with `D!`/`R!`
absent. The post-teardown final device check separately expects the final live
owner death revoke, so the pre-teardown corroboration count is exact one and
the teardown count is exact two.

Unresolved hazards: the hardened refusal is an explicit legacy-PIO
`UNSUPPORTED` profile, not DMA containment; support is limited to the tested
QEMU COM2 fixture; IRQ3 and TSS authority remain architecture-specific; no
remote CI or human qualification was observed; and no claim is made for SMP,
arbitrary hardware, dynamic loading, all drivers, or a general application ABI.

Exact commands/evidence:

```text
ASAN_OPTIONS=detect_leaks=0 make check — PASS
make smoke — PASS
make integration — PASS; mode 15 COM2=b'I1I2I3'
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent — PASS
python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx present — PASS
make aarch64 && make smoke-aarch64 ARM_REQUIRE=1 — PASS
git diff --check — PASS
```

Next bounded task: lead integration and owner/human review. Pass criteria are
preservation of the mode-15 kernel-state and independent COM2 evidence above;
do not claim hardened/release qualification until the human gate occurs.

## M4 PR-I — D9 brokered UART service / final M4 gate — observed 2026-11-02T00:07:42+00:00

Repository `/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/wt-m4i-impl`;
branch `wip/m4i-impl`; exact inspected commit
`3b447d0aa7c194e55fa2beca1bdad41b64fd2f60`. Changes remain uncommitted for
lead integration. This is local evidence only; remote CI and independent human
qualification are not observed. AI-assisted review is not human qualification.

State: **implemented / tested / planned / not blocked**.

- **Implemented:** mode-15 completion checklist and bounded supervisor deadline;
  the client grant wait now covers the supervisor's bounded kill/restart/attach
  sequence; the HANG incarnation yields and closes broker channels so
  `PEER_CLOSED` is observable without monopolizing the single CPU; hardened
  `DEV_ASSIGN` is refused with `UNSUPPORTED` and the legacy-PIO profile; and
  the UART scenario reaches DONE after timeout, same-live detach/reassign, and
  admin-revoke/fault restart.
- **Kernel/independent hardware evidence:** final mode 15 accepted the timeout
  PING/PONG deadline, detach, vector-13 revoke restart, hardened refusal with
  zero brokered caps, final revoked DEVICE state (`bitmap=0xff`, IRQ3 masked),
  and zero-leak identity baseline. QEMU's independent COM2 file contained
  exactly `b'I1I2'`; `D!` and `R!` were absent.
- **Root cause of the stall:** the client issued its reconnect request while
  the supervisor was still spending the 600-tick PING deadline plus
  kill/restart/DEV_ASSIGN/ATTACH work. The client used the same 600-tick grant
  wait, exited before the restart grant arrived, and the supervisor later saw
  `PEER_CLOSED` while sending it. The HANG path also needed to yield and close
  its broker channels so the client could observe the expected peer death.
- **Planned:** lead integration, cross-review, and the independent human gate
  required for any hardened/release qualification claim. No broader hardware,
  DMA, SMP, arbitrary-driver, or application ABI support is claimed.
- **Blocked:** no local implementation blocker.

Changed invariants: mode 15 exits only after all five checklist facts are set
(`timeout_done`, `detach_done`, `revoke_done`, `hardened_done`,
`client_i2_done`), both clients are closed, service generation is 3, and the
service is live. A missing fact fails with `JE_SVC_FAIL_UART_DEADLINE` rather
than relying on the kernel monitor deadline. The QEMU COM2 backend, not the
driver's self-report, is authoritative for `I1`/`I2` and forbidden post-
detach/revoke writes.

Exact local verification:

```text
ASAN_OPTIONS=detect_leaks=0 make check — PASS
make smoke — PASS (mode 0 unchanged)
make integration — PASS (modes 1–15; final mode 15 COM2=b'I1I2')
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent — PASS
python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx present — PASS
make aarch64 && make smoke-aarch64 ARM_REQUIRE=1 — PASS
git diff --check — PASS
```

The ARM build retained the existing linker RWX warning. Temporary A/B controls
were restored: removing the hardened refusal produced fail code `0x7` and no
`M4i-s1`; omitting `DEV_REVOKE` produced fail code `0x7` and QEMU COM2
`b'I1I2R!'`, proving the final absence check is meaningful.

Next bounded task: lead review of this uncommitted PR-I diff. Pass criteria are
preservation of the above mode-15 kernel/COM2 evidence and completion of the
owner's human qualification gate; no claim is made until that gate occurs.

## M4 PR-H (#53) — D9 device / TSS I/O bitmap / kernel-owned IRQ3 — observed 2026-11-01T21:18:49Z

Branch `feat/m4h-device`, based on main `b82d426` (#68 merged; main CI green);
integrated head `18349b8`. Opened as the PR-H pull request. Built as impl → security
code review (mutant-verified blockers) → fix, each cross-reviewed. The most
privilege-sensitive M4 PR: the first ring-3 hardware port I/O + real IRQ.

State: **implemented / tested / planned / not blocked**.

- **Implemented:** kernel-owned exact FAULT/EXIT/#GP mode-14 attestation;
  observed revoke/self-close denial probes; same-binding two-delivery IRQ3
  evidence; exact binding-generation and IRQ3-mask checks; teardown-before-walk
  centralized revoke with full inactive-state validation; corrected IRQ7
  real/spurious handling; genuine `0x2FF`→`0x300` crossing; and hosted D9
  lifecycle/IRQ7 coverage (`test_device`, 76 checks).
- **Tested:** `ASAN_OPTIONS=detect_leaks=0 make check`,
  `ASAN_OPTIONS=detect_leaks=0 make check TEST_MODE=14`, `make smoke`, the
  explicit NX/no-NX/16 MiB/1536 MiB profiles, `make smoke-aarch64`, and
  `make integration` passed. Final mode 14 printed
  `IRQ3 deliveries=0x...0f; max-binding=0x...2; <=1/tick`, TSS U=0,
  ring-3 #GP isolation, D9 completion, and zero-leak markers. Required
  bitmap-deny and allowed-port A/B mutants both failed mode-14 completion and
  were restored.
- **Planned:** PR-I (UART driver demo / end-to-end D9 console), then MP2.
  IRQ7 remains host-tested but not QEMU-injected (recorded gap).
- **Blocked:** no local implementation blocker; remote CI and independent human
  qualification are unobserved and not claimed.

**Review record:** `docs/adr/debates/m4h-reviews.md` (security design + code review;
the mutant-verified blockers), worker notes in `docs/adr/debates/m4h-workers.md`.

Changed invariants: mode 14 requires 13 vector-13 FAULT deaths and one clean
EXIT for the 14 driver incarnations; binding generation is exactly 28; at
least one IRQ3 delivery and at least two deliveries in one binding are
observed with no duplicate tick; revoke/death/teardown leaves the device
inactive, ownerless, pending-free, re-arm-free, bitmap-denied and IRQ3 masked.
The hosted test does not substitute for QEMU ring-3 #GP evidence or real PIC
IRQ7 injection. No DMA, SMP, arbitrary hardware, remote CI, or hardened/release
qualification is claimed.

## Historical pre-fix PR-H record

## M4 PR-H — D9 COM2 DEVICE / TSS I/O bitmap / IRQ3 — observed 2026-11-01T20:24:32Z

Repository `jeios`; branch `wip/m4h-impl`; exact inspected commit
`b4e51542d1795e84057b114df5890046a13668c0`; implementation remains uncommitted
as requested.

State: **implemented / tested / planned / not blocked**.

- **Implemented:** packed 201-byte TSS wrapper with a 96-byte deny-by-default
  I/O bitmap plus terminator and GDT limit 200; per-dispatch COM2 byte
  authority; kernel-only PIC IRQ3/IRQ7 helpers; singleton kernel-seeded DEVICE
  record; ASSIGN/REVOKE/IRQ_ACK ABI operations; delayed, generation-tagged IRQ3
  re-arm; exact-slot revoke/close/death/teardown integration; owner-aware VM
  teardown identity threading; supervisor startup n=2 with DEVICE admin; the
  ring-3 UART image; and TEST_MODE 14.
- **Observed D9 evidence:** QEMU mode 14 with a real second 16550 at COM2
  reached `M4h: IRQ3 deliveries=0x...2; <=1/tick`, `M4h: TSS page U=0 in
  user CR3`, `M4h: ring-3 #GP isolation, revoke, and deny bitmap
  corroborated`, `M4h-s1: D9 device scenario completed`, and the zero-leak
  identity baseline. The scenario exercised COM2 ports 0x2F8–0x2FF,
  forbidden-port/width/crossing probes, self-close, admin revoke, driver death,
  stale authority, delayed ACK re-arm, and scheduler progress with a masked
  IRQ3.
- **Tested locally:** `ASAN_OPTIONS=detect_leaks=0 make check`,
  `ASAN_OPTIONS=detect_leaks=0 make check TEST_MODE=14`, `make smoke`,
  `make integration`, the default NX-present, legacy no-NX, 16 MiB no-NX,
  and 1536 MiB NX-present smoke profiles, `make smoke-aarch64`, and
  `git diff --check` all passed. ARM smoke reported the existing linker
  RWX warning and otherwise passed its DTB/CRC/EL1 markers.
- **Planned:** independent real/spurious IRQ7 runtime injection, additional
  hosted DEVICE-core tests, and owner review/PR qualification. PR-H does not
  complete all M4 hardware/device gates; PR-I/MP2 remain future work.
- **Blocked:** no local implementation blocker. Remote CI and independent
  human qualification are unobserved and are not claimed.

**Changed invariants and unresolved hazards:** only the COM2 bitmap byte may
  become zero, only for a live exact driver handle during user dispatch; the
  kernel console COM1 and all other tested ports remain denied. IRQ3 is masked
  on delivery and can be unmasked only by the next-tick generation-checked
  re-arm path. Revoke clears authority before closing the copied exact slot.
  Process death revokes synchronously before reap; teardown treats an active
  recorded DEVICE slot as fatal and drains all other DEVICE references.
  IRQ7 real/spurious dispatch code exists, but neither case was generated by
  this QEMU run. No DMA, SMP, arbitrary hardware, dynamic loading, remote CI,
  or release/hardened qualification is claimed.

## PR-H verification record — exact commands and results

At the observation above, these commands returned exit 0:

```text
ASAN_OPTIONS=detect_leaks=0 make check
ASAN_OPTIONS=detect_leaks=0 make check TEST_MODE=14
make smoke
make integration
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --expect-nx absent
python3 tools/smoke.py build/0/jeios.iso --cpu qemu64,-nx --memory 16M --expect-nx absent
python3 tools/smoke.py build/0/jeios.iso --memory 1536M --expect-nx present
make smoke-aarch64
git diff --check
```

The mode-14 integration output included:

```text
M4h: IRQ3 deliveries=0x0000000000000002; <=1/tick
M4h: TSS page U=0 in user CR3
M4h: ring-3 #GP isolation, revoke, and deny bitmap corroborated
M4h-s1: D9 device scenario completed
M4h: zero-leak identity baseline verified
```

The next bounded task is owner review of this privilege-sensitive first
ring-3 direct-port-I/O change, with pass criteria of preserving the observed
denial evidence and either adding an IRQ7 injection fixture or recording that
coverage as a separate accepted gate.

## M4 PR-G (#53) — D11 broker / TEST_MODE 13 — observed 2026-11-01T10:43:43Z

Branch `feat/m4g-broker`, based on main `ade5ddc` (#67 merged; main CI green);
integrated head `51b252b`. Opened as the PR-G pull request. Built as impl → code
review (blockers found) → fix → owner option-2 identity re-key, each cross-reviewed.

State: **implemented / tested / planned / not blocked**.

- **Implemented:** bounded ring-3 supervisor policy over existing M3c IPC,
  exact 12-byte transaction-bound `SERVICE_BIND`/`BROKER_GRANT` records,
  two-phase service-first/client-second delivery, generation-wrap refusal,
  supervisor-PROCESS-handle-keyed ledger, both-DEAD collection, PENDING
  retention after uncertain ACK delivery, strict control-message validation,
  and TEST_MODE 13.
- **Kernel evidence:** cap-delivery audit is recorded only after receive commit,
  resolved from kernel PROCESS state rather than VM slot. Mode 13
  corroborates exactly three client-side and three service-side deliveries,
  A/B/A ordering, supervisor creator, exact `SEND|RECV` rights, live service
  state at the service-side delivery, the observed third A request with no
  third delivery, and supervisor endpoint-charge peak at or below 21. The
  report remains diagnostics only.
- **Teardown:** shared kill-all/double-reap remains the only teardown path; mode
  13 additionally requires endpoint allocation 0, endpoint quiescence, and
  process-pool use count 0 before the baseline marker.
- **Tested locally:** `ASAN_OPTIONS=detect_leaks=0 make check`,
  `ASAN_OPTIONS=detect_leaks=0 make check TEST_MODE=13`, `make smoke`,
  `make integration`, the 16 MiB/no-NX and 1536 MiB/NX smoke profiles,
  `make aarch64 && make smoke-aarch64 ARM_REQUIRE=1`, and `git diff --check`
  all passed. Integration mode 13 reached
  `M4g: client-reported EXHAUSTED; kernel delivery count A=2 corroborated`,
  `M4g-s1`, and the zero-leak baseline; `M4g: supervisor lost` was absent.
- **Planned:** PR-H (TSS I/O bitmap / DEVICE / IRQ3), PR-I (UART driver demo),
  then MP2. **Blocked:** no implementation blocker; remote CI and independent human
  qualification are unobserved.

**Review record:** `docs/adr/debates/m4g-reviews.md` (design + code review; the code
review's blockers and the owner's option-2 identity decision), worker notes in
`docs/adr/debates/m4g-workers.md`. **Known coverage gap (honest):** A's EXHAUSTED
lands where a live duplicate also exists, so mode 13 proves the cap check-order but
not a duplicate-free lifetime cap; a host unit test would close it.

**Changed invariants and unresolved hazards:** brokered endpoints are never
forwardable; supervisor source handles close only after receiver ACKs; malformed
control traffic cannot fail the supervisor scenario; and uncertain service
delivery remains pending until both keyed PROCESS handles report DEAD or an
uninspectable handle is explicitly cleared. A PROCESS handle referenced by a
ledger entry is never closed or reused while that entry exists; the old service
handle is retained across restart for this reason. The fixed broker table and
endpoint bounds are exercised (`JE_BROKER_MAX=6`, endpoint pool 24,
supervisor ep-limit 21).
M5 remains intentionally limited: A's `EXHAUSTED` request occurs while a live
generation-2 entry exists, so this scenario proves check ordering but not the
duplicate-free cap; a host unit test is the next bounded task. No production
service registry, dynamic loading, ring-0 fault isolation, hardware-wide
support, remote CI, or independent human qualification is claimed.

## M4 PR-F (#53) — the supervisor + D12 supervisor-loss — observed 2026-11-01T05:41:51Z

Branch `feat/m4f-supervisor`, based on main `1343225` (#66 merged; main CI green);
integrated head `1008d40`. Opened as the PR-F pull request.

- **Implemented:** ADR 0010 D7/D11/D12. A ring-3 supervisor (image 2) launches and
  monitors four child services (restart-svc 3, survivor 4, spoofer 5, crash-loop
  6); a kernel slot-0 monitor watches the supervisor by identity and, on loss or
  clean completion, runs the shared kill-all / reap / identity-baseline / halt. See
  IMPLEMENTATION "M4 PR-F". Built across three worker passes (Stage 1 kernel
  mechanism, Stage 2 supervisor+monitor, Stage 2 fix) with cross-lineage review at
  each; the frozen-path frame-zero security bug found mid-build landed separately
  as #66 and PR-F is rebased on it.
- **Tested (local):** `make check` (incl. test_sched_killall 66, vm stale-upper
  rejection, test_image 88, test_mkimage 562), `make smoke` (default boot runs the
  success scenario to `M4f-s1` + zero-leak identity baseline), `make integration`
  (12 modes incl. mode 12 = D12 supervisor-loss refusal), NX ×4, ARM64 base — all
  PASS. Two design reviews and three code reviews (both lineages); the Stage-2
  review found and fixed real blockers (success attestation that trusted report
  counters; a vacuous rollback test; an unbounded monitor). See
  `docs/adr/debates/m4f-reviews.md`.
- **Key invariants:** D12 loss is detected by identity (`je_proc_status != LIVE`),
  never a `mark_dying` hook; the monitor is the sole reaper after seeding and never
  calls `idle_step`; success is decided ENTIRELY from kernel state (exact per-image
  population from a kernel child ledger, spoofer death record + SEND-only shape,
  `je_spawn_failpoint_fired()==1`, supervisor `reason==EXIT && code==JE_SVC_DONE_EXIT`)
  with the report as diagnostics only; the identity baseline is an ownership
  predicate (page_state + process/VM identities), not a count compare.
- **Planned:** PR-G (broker), PR-H (TSS I/O bitmap / DEVICE / IRQ3), PR-I (UART
  driver demo), then MP2. **Blocked:** nothing.

**Open hazards / not claimed:**
1. Forged-counter, torn-report, and impostor-child adversarial injection fixtures
   are NOT separate end-to-end tests; the kernel-state per-image corroboration
   defends against them structurally, but they are not independently exercised.
2. A slot-0 `pause` poll stalled READY user threads in the PR-D round-2 fixture;
   root cause still undetermined (`sti; hlt` / tick-sleep waits work, and PR-F uses
   only those).

## Superseded worker handoffs (PR-F stages) — kept as history

The Stage 1 / Stage 2 / Stage 2-fix worker handoff blocks below were written
against the intermediate `wip/m4f-*` branches before integration; the block above
is the current, integrated status. The worker notes are preserved in
`docs/adr/debates/m4f-workers*` and `m4f-reviews.md`.

## M4 PR-F Stage 2 FIX — observed 2026-11-01T05:32:14+00:00

Repository `auto44e/jeios`; branch `wip/m4f-fix`; inspected commit
`6520f1515d06a83ba6fcf400e029a8a21e616b0a`; changes remain uncommitted for
lead integration.

- **Implemented:** monitor-time reaping in the IF=0 scan section, a bounded
  kernel-owned image ledger, pre-reap terminal evidence capture, exact
  kernel-only success corroboration, exact `JE_SVC_DONE_EXIT` supervisor
  termination, and explicit image-specific population/death checks.
- **Implemented:** real post-allocation rollback failpoint (step 4), VM
  headroom assertion before ARM, exact one-shot `H_NOSLOTS`/handle-zero
  observation, wrap-safe monitor and ARM watchdogs, and a second supervisor
  identity scan before terminal reaping.
- **Implemented:** survivor liveness enforcement, exact single spoofer stale
  probe, exact supervisor self-handle check, complete report seqlock
  publication, non-success sleep-channel diagnostics, mode-12 READY evidence,
  and explicit second reap plus full baseline comparison.
- **Tested:** `ASAN_OPTIONS=detect_leaks=0 make check` passed; `make smoke`
  passed with `M4f-s1`, zero-leak, and `QEMU smoke: PASS`; required ARM
  (`make aarch64 && make smoke-aarch64 ARM_REQUIRE=1`) passed; `make
  integration` passed modes 1–12. Mode 12 printed READY phase `1`, FAULT
  reason `0`, the D12 loss marker, zero-leak, and no scenario-completed
  marker.
- **Planned:** lead integration, repeat the local gates on the integrated
  tree, and independent human review. **Blocked:** none known.

Changed invariants and unresolved hazards: report counters are diagnostics and
never authorize success; child identity and terminal outcomes come from
kernel state. The ledger records only successful spawns and clears entries
after reap while preserving bounded aggregate history. The adversarial
forged-counter, torn-report, and impostor-child cases are not separate
end-to-end fixtures in this pass and are not claimed as tested. Remote CI and
human qualification are unobserved; this is AI-assisted work only.

## M4 PR-F Stage 2 — supervisor, child services, and D12 monitor — observed 2026-11-01T03:06:15Z

Repository `auto44e/jeios`, branch `wip/m4f-impl`, inspected commit
`5ffd26c854a3603de838452c79e81d7996dbd7ff`; this worker left the requested
changes uncommitted for lead integration.

- **Implemented:** ring-3 images 2–6, the fixed `.je_report` RW placement,
  startup-v1 service launch, readiness/config/heartbeat protocols, bounded
  restart and crash-loop scenarios, spoofer evidence, rollback failpoint
  handshake, and the default NX success path.
- **Implemented:** the slot-0 monitor uses the saved supervisor identity
  (`je_proc_status`), reads the report only after the PTE RW/NX probe, never
  calls `je_kernel_idle_step` after seeding, and treats every non-LIVE status
  as supervisor loss unless a stable DONE report and kernel corroboration prove
  success.
- **Implemented:** D12 teardown calls kill-all with no spare, explicitly reaps,
  checks no DYING users remain, checks endpoint quiescence, and compares the
  full allocator page-state snapshot plus process/VM identities against the
  pre-seed baseline.
- **Implemented:** `TEST_MODE=12` writes the mode word into the supervisor RW
  report page after seeding; the supervisor faults after restart-service and
  survivor readiness. The mode-12 harness expects the refusal and zero-leak
  markers and rejects `JeiOS fatal:`.
- **Tested:** `make smoke` passed locally at this observation. A direct
  `make iso TEST_MODE=12` plus QEMU boot passed locally with
  `M4f: supervisor lost (reason=0x...); no recovery path in this profile` and
  `M4f: zero-leak identity baseline verified`.
- **Tested:** `ASAN_OPTIONS=detect_leaks=0 make check`, `make integration`, the
  default and 16 MiB no-NX profiles, and the 1536 MiB NX profile all passed
  locally. Integration covered modes 1–12, including mode 12.
- **Planned:** rerun the full suite after lead integration or any code changes;
  inspect the complete diff and preserve the exact local-vs-remote evidence.
- **Blocked:** no known blocker. Remote CI and independent human qualification
  are not observed by this worker.

**Changed invariants and hazards:** `vb_link_tables` now clears reused
paging-table frames before publishing entries; allocator ownership reuse alone
does not guarantee byte-zeroed tables. The supervisor report is untrusted
diagnostic evidence; kernel proof uses death records and live endpoint-handle
rights. The baseline does not claim hardware-wide support, SMP safety, or
fault isolation for ring-0 modules. The monitor intentionally leaves child
DYING records unreaped until the supervisor decision so the corroboration scan
can inspect their first death records.

## M4 PR-F Stage 1 — D12 kernel mechanism — observed 2026-10-31T23:05:35+00:00

Repository `auto44e/jeios`, branch `wip/m4f-impl`, inspected commit
`b3ae3b7b136346290bda91baadc4909ad203b3fb`; this worker left the requested
changes uncommitted for lead integration.

- **Implemented:** the real slot-0 `je_sched_kill_all_user` mechanism. It
  validates every user-thread identity/run-link and process state in a first
  pass, then marks all non-spared LIVE READY/BLOCKED_IPC users through the
  unchanged `mark_dying` path. DYING records are validated and skipped;
  RUNNING/SLEEPING/ZOMBIE, generation-zero, and mismatched links return
  `JE_ERR_INVALID` without mutation. READY wait-pending reservations use the
  existing idempotent release owner.
- **Implemented:** Stage-2-only `svc_report_read` scaffolding in `main.c`.
  It probes the fixed report page as present RW/NX before CR3 switching,
  reads seq/body/seq, restores the kernel CR3 before return, and validates
  fixed-format fields. It is unused until the supervisor monitor is added.
- **Implemented:** the pure supervisor-image `report_vaddr_in_rw_segment`
  predicate and commented image-table hook. Existing images are not checked
  against the supervisor-only placement contract.
- **Implemented:** hosted `test_sched_killall` coverage and Makefile wiring.
- **Tested (local):** `ASAN_OPTIONS=detect_leaks=0 make check` passed, including
  `test_sched_killall: PASS (66 checks)`, ELF/image checks and the existing
  hosted suites. `make smoke` rebuilt `build/0/jeios.iso` and passed QEMU
  smoke through `M4e-s1` and the existing M3/M4 fixtures.
- **Not tested:** the new report reader and image predicate are not exercised
  by the current boot because the supervisor image/monitor is Stage 2. No
  remote CI or independent human qualification is claimed.
- **Planned:** Stage 2 supervisor image, monitor identity scan, report success
  decision, kill-all/reap/baseline refusal path, and TEST_MODE 12.
- **Blocked:** nothing for this bounded Stage 1 task.

**Changed invariants and hazards:** kill-all is transactional on validation
failure and only slot 0 may invoke it with IF clear; it does not add a
`mark_dying` hook or alter `mark_dying`/`je_sched_reap_dead`. The report reader
still relies on its documented single-CPU, present-mapping/no-fault-recovery
precondition after the explicit PTE probe. The current QEMU smoke does not
prove supervisor-loss behavior.

## M4 PR-E (#53) — ring-3 C image path + `mkimage.py` — observed 2026-10-31T20:59:44Z

Branch `feat/m4e-cimage`, based on main `1413ed8` (#64 merged; main CI green);
pre-wiring `1df911a`, head `8748bac` (PR #65).

- **Implemented:** ADR 0010 D10. The first real ring-3 C program (`c_hello`) built
  by a host cross-compile, validated + embedded by `tools/mkimage.py`, booted
  through startup-v1. See IMPLEMENTATION "M4 PR-E".
- **Tested (local):** `make check` (test_image 88, test_mkimage 562), smoke
  (M4e-s1, blob sha256=351bc37034ee86c3), integration (11 fault modes), the three
  x86 NX profiles ×11, and ARM64 base + 3 fault variants. Two lead mutants caught.
  Cross-lineage adversarial review (Claude on impl, Codex on tests) found one
  frozen-spec defect (lowest-rule-wins) and several hardening gaps, all fixed;
  see `docs/adr/debates/m4e-reviews.md`.
- **Remote CI (observed):** bootstrap + aarch64 **green** @ `8748bac`. A first
  push (`34c43e9`) failed bootstrap because `--orphan-handling=error` turned the
  CI linker's empty GOT/PLT orphans into an error; fixed in `8748bac` by
  discarding that section family in `user.ld` (image byte-identical).
- **Planned:** PR-F, the supervisor (D11/D12 supervisor-loss handling). Map ready
  at the lead's `/tmp/m4f/map.md`.
- **Blocked:** nothing.

**Open hazards:**
1. M4e-s1 does not assert which image the child ran (only READY + exit 0); image
   id is hard-coded in the launch asm. Revisit if PR-F selects images dynamically.
2. A slot-0 `pause` poll stalled READY user threads in the PR-D round-2 fixture.
   Root cause undetermined; `sti; hlt` waits work.
3. PR-F's D12 must revisit the M4d supervisor-guard fixture teardown.

## M4 PR-D (#53) — PROC_KILL — observed 2026-10-31T16:56:37+00:00

Branch `feat/m4d-kill`, based on main `62c0115` (#63 merged; main CI green).

- **Implemented:** ADR 0010 D8. See IMPLEMENTATION "M4 PR-D".
- **Tested (local):** check, smoke, integration, the three x86 profiles, NX ×5, and four ARM modes. Two mutants were run, and both were caught.
- **Planned:** PR-E, the ring-3 C image path and host tool. The map is ready: plain host gcc plus a linker script is verified to produce deterministic, SSE-free, W^X images.
- **Blocked:** nothing.

**Open hazards:**
1. A slot-0 `pause` poll stalled READY user threads in the round-2 fixture. The root cause is not determined; `sti; hlt` waits work.
2. PR-F's D12 must revisit the M4d supervisor-guard fixture teardown.

## M4 PR-C (#53) — spawn/LAUNCHER/startup-v1/CLOCK_TICKS — observed 2026-10-31T13:20:28+00:00

Branch `feat/m4c-spawn`, based on main `83782c0` (#62 merged; main CI green).

- **Implemented:** ADR 0010 D5 and D6. Details are in IMPLEMENTATION, "M4 PR-C".
- **Tested locally:** check, smoke, integration, the three x86 profiles, NX five times in a row, and all four ARM modes.
- **Planned:** PR-D (`PROC_KILL`). Its code map is ready.
- **Blocked:** nothing.

**PR-D honesty item.** ADR 0009 A6-CLARIFY-2 and ADR 0010 D8 refer to a test-only forced-kill hook, but it **does not exist** in the code. `je_endpoint_kern_wait_cancel` has no callers. PR-D must correct this with a dated note.

**Verification lesson.** `make all` does not rebuild `build/0/jeios.iso`. Run `make smoke` (or rebuild the ISO) before calling `tools/smoke.py` directly. Otherwise you boot a stale image.

## M4 PR-B (#53) — PROCESS records + reaper + typed dispatch — observed 2026-10-31T03:30Z

Branch `feat/m4b-process`, based on main `5dbd0d8` (#60 and #61 merged; main CI green at `5dbd0d8`).

- **Implemented:** ADR 0010 D3, D4 and D-DEV. Details are in IMPLEMENTATION "M4 PR-B".
- **Tested locally:** check, smoke, integration, the three x86 profiles and all four ARM modes. The new M4b-s1 markers appear on the NX profile.
- **Planned:** PR-C, covering `CLOCK_TICKS`, LAUNCHER, `PROC_SPAWN`, the startup register ABI and spawn rollback. The code map and pre-wiring plan are ready.
- **Blocked:** nothing.
- **Recorded decision:** spawn's internal NOMEM/BUSY map to `H_NOSLOTS`, per ADR 0010 Consequences (no new statuses).

## M4 PR-A (#53) — capacity raise — observed 2026-10-31T02:11:09+00:00

Branch `feat/m4a-capacity`. It is stacked on `docs/adr-0010-m4` (#60, ADR 0010), so #60 must merge first.

- **Implemented** (ADR 0010 D2):
  - New limits: P = 8 processes, T = 16 threads (M2 workload kept at 7), E = 24 endpoint records, H = 40 handle slots.
  - The capacity formulas are enforced by `_Static_assert`.
  - `je_vm_build_quota` adds a per-table quota. `je_vm_build` keeps the old default of 15.
  - M2 is restored exactly, with a new capacity-exhaustion phase.
  - The ring-3 BADINDEX bound probe now tests index 40, and a new in-range-free probe expects STALE.
  - Stale ROADMAP and ADR 0009 status text was refreshed.
- **Tested, local only:**
  - `make check`, `make smoke`, `make integration`, all three x86 profiles and all four ARM modes pass.
  - Free pages at 16 MiB: 3067. M2 preemptions: 0x49, against a floor of 16.
  - Remote CI results are recorded in the PR.
- **Planned:** PR-B: PROCESS records, the automatic idle reaper, typed dispatch, the D-DEV quota and `PROC_STATUS`. The code map is prepared; see the PR-B notes in the PR.
- **Blocked:** nothing.
- **Owner check:** the M2 capacity phase is new code inside the previously tested M2 boot test. It was approved under D-OWNER-1, which includes "re-baselined assertions", and is flagged for your gate.

## M4 contract (#53) — ADR 0010 accepted — observed 2026-10-31T00:02:04+00:00

Repo auto44e/jeios, branch `docs/adr-0010-m4`, based on main `dc41da5`. #52 is CLOSED
(MP1 complete; remote ARM64 CI observed at `bf2ecd0` and post-merge at `dc41da5`).

- **Implemented:** nothing at runtime. This is a documentation and contract PR.
  - `docs/adr/0010-m4-supervisor-and-devices.md` (accepted).
  - Back-pointers added to ADRs 0007, 0008 and 0009.
  - Transcript `docs/adr/debates/m4-contract.md`: the design debate plus three ADR text-review rounds.
- **Owner decisions:** D-OWNER-1 through D-OWNER-8 were approved on 2026-10-30. They cover:
  - the capacity raise;
  - PROCESS records and automatic reaping;
  - `PROC_KILL` in the production ABI;
  - typed `query`/`rights`/`dup`;
  - the TSS I/O bitmap and kernel-owned IRQ3;
  - supervisor loss as a fail-stop refusal;
  - `CLOCK_TICKS`;
  - recording and fixing the ADR 0009 D5 deviation.
- **Recorded deviation:** ADR 0009 D5 requires a per-process endpoint creation quota. It was never implemented in M3c. It is fixed in M4 PR-B (ADR 0010 D-DEV).
- **Tested:** not applicable. No code changed, so no emulator run was needed. `git diff --check` is clean.
- **Planned:** PR-A through PR-I per ADR 0010 D-SEQ, then MP2 (mobile #2).
  - **Next bounded task:** PR-A. It raises capacity, adds the per-table handle quota field, measures static sizes and re-baselines the M2/M3 assertions.
  - **PR-A passes when:** `make check`, `smoke`, `integration`, the three x86 profiles (including 16 MiB) and remote CI are green, and every D2 formula is asserted.
- **Blocked:** nothing.
- **Known stale text, deferred:** the `abi.h` comment saying `cap_count` must be 0 (fixed in PR-B). ADR 0009's status line still says "proposed". ROADMAP still has lines saying M3c is in progress.

## MP1 completion (#52) — DTB parse + ARM64 CI job — observed 2026-10-30T02:29:52+00:00

Repo auto44e/jeios, branch `feat/mp1-dtb-parse`, based on main `5f84ff7`. The full
local sweep below ran at `90693b7`. At `fe7beb1` (a harness-only ELF-header fix)
the four ARM smoke modes were re-run and PASS. The PR records the exact head and
its remote CI run. Issue #52 is
OPEN until the remote `aarch64` job is observed passing at the PR head.

- **Implemented:**
  - arch-neutral bounded parser `kernel/core/fdt.c` plus the contract `include/jeios/fdt.h`
  - ARM glue: DTB-pointer guard in `boot.S`; a single 64 KiB copy, then parse, then `je_boot_ctx_check` in `main.c`
  - `JE_ARM_DTB_BAD`
  - a hardened `tools/smoke_arm.py`: full-line markers, final-log revalidation, whole-run timeout, `--require` preflight, exact `pa_bits=44`, CRC equal to the fixture
  - mutually exclusive mk toggles plus `ARM_SMOKE_TIMEOUT`
  - fixture `tests/fixtures/qemu_virt82_dtb.h` (generated by `tools/gen_dtb_fixture.py`)
  - the `aarch64` job in `.github/workflows/ci.yml`

  Details and the provenance table are in IMPLEMENTATION.md "MP1 step 3".
- **Tested, local only:**
  - `ASAN_OPTIONS=detect_leaks=0 make check`, `make smoke` and `make integration`: PASS
  - the three x86 CI profiles (`--cpu qemu64,-nx --expect-nx absent`, the same with `--memory 16M`, and `--memory 1536M --expect-nx present`): PASS
  - `make aarch64` in all four modes: warning-free apart from the existing RWX linker warning
  - `make smoke-aarch64 ARM_REQUIRE=1` in the default, `ARM_VECTOR_TEST=1`, `ARM_BSS_CHECK=1` and `ARM_DTB_BAD=1` modes: all PASS on QEMU 8.2.2
- **Planned:** remote CI observation on the PR. **Blocked:** nothing.
- **Correction recorded:** the 2026-10-19 pickup item claimed remote CI ran the ARM compile. That claim was false; see the CORRECTION note under it.

**Review status.** Worker mode: every component was written by one model lineage and adversarially reviewed by the other, over 4–7 rounds. The last two small test and harness fixes were written by the lead; each one kills the mutant or reproduction the reviewer reported. Transcripts are in `docs/adr/debates/mp1-dtb.md` and the worker notes in `mp1-dtb-workers/`. This is AI-assisted review, not independent human qualification.

**Open hazards and MP2 obligations:**
- MP2 must `DC IVAC` the copy buffer before enabling caches.
- The MP2 allocator must honor `rsv_dynamic` and `rsv_nomap`.
- Only the QEMU virt-8.2 DTB at 256 MiB is qualified.
- Node names match exactly (no `chosen@0`).
- The length-8 `/reserved-memory` cells test gap found in review is closed (W2r6; mutant killed).
- The harness ELF parser rejects extended section numbering rather than handling it.

**Next bounded task:** after merge, M4 / MP2 per docs/NEXT_PHASE.md. Pass/fail: the gate criteria named there.

## Standards and organizational support — observed 2026-10-26T08:28:29-04:00

Inspected auto44e/jeios main `99fb5687b9c13e40bdca6253c780d2c6f0312e05`; PR #57 is merged in this
baseline. Branch: docs/standards-org-support. Owner adopted the ISO standards
plan and requested features supporting organizational ISO 27001 efforts.
Implemented here: STANDARDS.md, ORG_SECURITY.md and entry-point links only.
Planned: scoped POSIX/C, security evaluation, quality, human-centred design and
international-text requirements; ORG-01–09 inventory/identity/policy/audit,
patching/crypto/backup/response/evidence functionality. No runtime behavior,
certification, normative clause-level audit or new compliance enforcement claimed.
No tested code/frozen contracts changed. All implementation owners remain
unassigned. Existing MP1/M4/M5a priorities and mobile pins remain unchanged.

Validation: `make check` blocked by environment LeakSanitizer inspection failure;
`ASAN_OPTIONS=detect_leaks=0 make check` passed (exit 0), leak scanning disabled.
Changed local Markdown links and `git diff --check` checked before publication.
No boot smoke (documentation-only), cross-architecture run or remote CI result
claimed. Single-agent contribution; no independent human qualification.
Next bounded task: ORG-G0 audit/evidence contracts and requirement inventory
alongside M4, with owners, privacy/loss behavior and negative acceptance tests.
Open issue: reviewed normative mappings and implementations remain future work.


## Ecosystem architecture scaffold — observed 2026-10-24T05:51:56+00:00

Inspected auto44e/jeios main `5648a4bd129ed713756feb72d97d4011788504cd`.
Branch: docs/riscv-scaffold-20261024. Owner clarified that RISC-V and other
CPU/GPU/APU/NPU support covers the entire ecosystem. Added ARCHITECTURE_SUPPORT.md,
RISCV.md, config/architecture-profile.template.json, config/riscv64.json and
kernel/arch/riscv64/README.md; linked README, roadmap, implementation and agent entry
points. Implemented: documentation/configuration scaffolds only, not build inputs.
Planned: RV0–RV4 CPU foundation plus driver, service/runtime, SDK/package and
product integration qualification. No executable port, firmware import, changed
ABI or frozen contract. No accelerator/device or whole product qualified here.
No mobile integration pin change. Existing MP1/M4/M5a priority remains unchanged.
Next bounded task: reviewed RV0 pins/handoff/failure contracts; separate named
accelerator profile proposals follow M4/M8 and cross-layer evidence gates.

Verification on this patch: `make check` blocked by LeakSanitizer environment
inspection failure. `ASAN_OPTIONS=detect_leaks=0 make check` then encountered a
zero-byte/non-executable build/0/test_syscall artifact. Forced regeneration with
`ASAN_OPTIONS=detect_leaks=0 make -B check` passed host tests, ELF and isolation
checks (exit 0); leak scanning was disabled. JSON parsing, local Markdown links
in changed files and `git diff --check` checked before publication. No RISC-V
compiler/emulator, accelerator hardware, boot smoke or new-head remote CI run
claimed. No independent human qualification. PR is reviewable, not merged.

## Contributor guidance refresh — observed 2026-10-19T11:09:52+00:00

Inspected jeios main at `9bb0c1cdac351e4f937c39c4431ecfd959eb14cd`. Owner requested additional
guidance for other contributors: expansive risk-based local CI/testing, PR result
data and an easy-to-hard assistance scale adjustable even mid-session. Added
contributor/agent guidance and a PR evidence template; core holds the shared guide.
Easy mode includes setup, plain-language OS/coding explanations and contribution/
decision walkthroughs; hard mode still follows all standards, docs and gates.
Owner clarification: fully guided contributors can work through natural
plain-English conversation without prior code/terminal/Git syntax knowledge.
Added a shared development getting-started guide with conversational and manual
paths, setup/build/test/PR steps and a mobile entry point.
This is documentation only, not new CI enforcement, runtime support or an owner
onboarding requirement. Existing milestone status and owner gates are unchanged.

Local verification on this docs-only patch over the inspected revision:
`make check` hit the environment LeakSanitizer /proc inspection restriction;
`ASAN_OPTIONS=detect_leaks=0 make check` passed, with leak scanning disabled.
Changed Markdown links and `git diff --check` checked before publication.
No emulator rerun for docs/template changes; no new-head remote CI result claimed
here. Next: review/merge the guidance; implementation priorities remain #52/#53/#54
and mobile #2. CI automation/enforcement remains core #18/#19.

## Current phase refresh — observed 2026-10-19T10:44:37+00:00

Inspected `auto44e/jeios` main at `11a02adf744874328371b7f9f3bc801623995980` and mobile main at
`c7b42cd8a575fafc67c7a8e7742218dee3981e83`. Core PR #51 and mobile PR #9 are merged; no open PRs at
initial inspection. Core #2–#8 and mobile #1 are closed and Done on the board.
Implemented/tested: M3a/b/c scoped AMD64 userspace and IPC; ARM entry/console/vectors.
Not implemented: general init/service supervisor, filesystem, full ARM userspace.

Observed remote CI: core run 39237040923 success at the inspected SHA; mobile
run 39072964571 success at its inspected SHA (planning validation only).
ARM execution evidence remains the earlier local QEMU record, not remote boot CI.
This documentation update does not rerun or expand that execution qualification.

Planned next: [NEXT_PHASE.md](NEXT_PHASE.md), core #52 MP1 DTB/boot CI, #53 M4
services/devices, #54 read-only M5a, and mobile #2 MP2/MP3 parity. #13–#16 and
#18/#19 remain open. Two-process capacity and process-loader/lifecycle extensions
need review before the supervisor/service/survivor demonstration. Frozen ADRs and
tested-path owner gates remain unchanged. No new runtime behavior is claimed.

Historical status and exact earlier test environments follow. This block supersedes
their next-task/open/closed wording, without rewriting the original evidence.


Local verification for this docs-only refresh: plain `make check` built the kernel but stopped because LeakSanitizer could
not inspect /proc tasks in this environment. `ASAN_OPTIONS=detect_leaks=0 make
check` then passed hosted suites, ELF and ABI checks; local leak detection was
disabled. Remote baseline success above is separate. No emulator rerun.
Changed Markdown links and `git diff --check` passed. A broad link scan also
found a pre-existing relative AGENT_START.md reference in the raw historical
transcript docs/adr/debates/m3c-step6-mechanism.md; transcript text is preserved.
Project board verified core #52/#53/#54 and #18/#19 as Todo; mobile #2 as
In Progress. Current overview updated while preserving its earlier history.

## Current status — observed 2026-10-19 (read this first; supersedes earlier status blocks)

**M3c is COMPLETE and issue #8 is CLOSED** — the full D-SEQ (steps 1–7) is merged
on `main` at `bac8c3b`. M3b (#7) is likewise complete (its remaining refcount /
destruction / instance-vs-slot acceptance was delivered as M3c's endpoint lifetime
— the first destroyable object), and should be closed. The ARM64 MP track reached
MP1 exception vectors. Full sweep (`make check` / `smoke` incl `M3c-s3a..s7` /
`integration` / 3 CI NX incl 16M / `make aarch64`) is green.

This session's merged work: M3c D-SEQ (PRs #40–#48), ADR 0000 decision-record
policy + part-4 attached-correspondence (#44, #46), the retroactive sweep (#50),
ARM64 MP1 vectors + D5-3 trap frame (#49), and the single-agent-verbosity
contributor norm (AGENTS.md + CONTRIBUTING.md). The M3c retrospective + exact
qualification evidence is in [M3C_RETRO.md](M3C_RETRO.md).

### PICKUP ITEMS for the next session (owner paused here)
1. **MP1 DTB parse (finishes MP1).** The ARM64 `je_arm_dtb_phys` pointer is
   RECORDED but never parsed. ARM64.md MP1 requires a *bounded, copied, defensively
   validated* DTB parse: RAM / reserved ranges / kernel image / DTB / init
   reservations; reject malformed structure/string-offsets/overflows/overlaps;
   bound node depth + property counts; trusted-bootloader output parsed defensively.
   Copy the DTB into a kernel-reserved region before the MMU (ADR 0002 D3). This is
   a fresh MP1 sub-step in `kernel/arch/aarch64/` (not owner-gated; new ARM64 code).
2. **Wire the ARM QEMU boot into remote CI.** Today remote CI runs x86 bootstrap +
   the ARM *compile*; the ARM vector-test *boot* (`make smoke-aarch64
   ARM_VECTOR_TEST=1`) is verified locally only. Honest gap flagged in PR #49.
   **CORRECTION (verified 2026-10-29 against `main`'s `.github/workflows/ci.yml`
   at `5f84ff7`):** the claim above that remote CI ran "the ARM *compile*" was
   FALSE when written — the workflow had no ARM job at all; `make aarch64` had
   only ever been run locally. The original sentence is preserved as history;
   the #52 PR adds the first `aarch64` CI job.
3. **Board hygiene:** confirm #7 and #8 closed (done this session); keep the
   project board in sync.
4. **Then the roadmap gates beyond M3** (core: the next core kernel gate; mobile:
   MP2 MMU/timer/scheduling, which now has MP1 vectors beneath it) per ROADMAP.md,
   and the longstanding open contracts (#13–#16 storage/F2FS/POSIX/LDAP, #18/#19
   CI-enforcement + coding-profile assurance).

---

## Earlier status — observed 2026-10-18

Implemented and merged beyond M3a: **M3b** (typed handle table, ADR 0008) is in
progress on x86-64, and a second architecture, **ARM64 (MP1)**, now boots.

- **M3b** (issue #7, ADR 0008 accepted, Option A): the pure handle table, the
  handle syscalls (`handle_self`/`close`/`query`/`rights`), the rights ABI, AND
  **attenuation** (`handle_dup`, op 9) are all merged. Attenuation made rights
  enforcement genuinely ring-3-observable: a process derives a rights-reduced
  handle and `handle_rights` on it returns DENIED (QEMU `M3b-s1` proof). SELF is
  the only object kind; dup is SELF-only (structurally type-gated — a destroyable
  type would be a latent UAF without a retain, which M3c must add). **#7 stays
  OPEN** (landed "Refs #7"): its remaining acceptance — bounded per-object
  **reference count + overflow**, destruction-on-last-reference, and the
  **service-instance vs slot generation** split — needs a destroyable object and
  is M3c work.
- **ARM64 MP1 step 1** (jeios-mobile#1, ADR 0002): boots on QEMU `virt-8.2` /
  `cortex-a72` (direct `-kernel`, no firmware) — entry EL1, stack, TCR_EL1 interim,
  BSS clear, DTB pointer recorded, bounded PL011 console, ordered markers. Separate
  `kernel/arch/aarch64/` + `mk/aarch64.mk` (`make aarch64`, `make smoke-aarch64`);
  the x86-64 flow is untouched.
- **ARM64 MP1 step 2** (ADR 0002 D5-3): VBAR_EL1 exception vectors + the
  owner-approved 304-byte D5-3 trap frame (`gpr[31]`, sp, elr, spsr, esr, far,
  vector) now installed in `kernel/arch/aarch64/` (`vectors.S`, `trapframe.h`). A
  deliberate `brk #0xA55A` is proven on real QEMU to vector to the EL1h sync slot
  with the exact syndrome (ESR=0xf200a55a); `make smoke-aarch64 ARM_VECTOR_TEST=1`
  asserts the raw ESR/SPSR on the wire. `boot.S` now writes SCTLR_EL1 explicitly
  (SA=1 + RES1 set) — a found step-1 gap (SA reset is UNKNOWN). Non-reentrant,
  non-returning handlers; nested fault → pure-asm bounded `je_arm_double_fault`.
  **Remaining MP1:** the bounded DTB *parse*/validation. **Deferred to MP2+:** the
  MMU, timer, scheduling; the TBI behavioral test.
- **M3c** (issue #8, **ADR 0009 accepted**): in progress. The contract is frozen
  (endpoint/channel object, split refcount + destruction, bounded data-only
  messages, waits, transactional attenuated transfer, bootstrap fixture; two
  owner-gated decisions — terminal-destruction/no-incarnation-counter, and the
  process-death release walk as its own owner-signed PR). **D-SEQ step 2
  (lifecycle)** is implemented: `kernel/core/endpoint.c` + `endpoint.h` — the
  endpoint pool, channel record, **split refcount** (`handle_refs` vs
  `inflight_refs`), retain/release, destruction-on-last-ref, `REFOVERFLOW`, the
  endpoint-aware alloc/dup/close seam (table stays pure), and retain-before-commit
  endpoint dup (while `dup_self` stays SELF-only). Step 2 kept create kernel-only;
  **step 3 (owner-signed) is now done**: the process-death **release walk** (via a
  generic `je_vm` teardown hook run inside an **IRQ critical section** in
  `je_vm_destroy` — A3-1), the **typed close dispatcher** (ring-3 HANDLE_CLOSE
  routes endpoint handles through the endpoint layer — A3-3), and **ring-3
  `endpoint_create` (op 10)** returning both handles **packed in RDX** (A3-2)
  shipped together. Proven in QEMU: `M3c-s3a` (kernel self-test: survivor-peer
  PEER_CLOSED, record freed on survivor close) and `M3c-s3b` (ring-3 create →
  typed close → fault → reap reclaims the channel; pool baseline held across
  >POOL_CAP cycles). ADR 0009 was amended (A3-1/A3-2/A3-3, owner-approved) for the
  IRQ-save death-path change and the packed-RDX ABI shape. Earlier, ADR 0009 D4a
  appended endpoint rights (`SEND=0x2`…), an owner-approved ABI change that flipped
  the tested `dup(self,0x2)` result INVALID→DENIED (Option A; see IMPLEMENTATION.md
  "M3c rights-bit discrepancy"). **Step 4 (non-blocking data IPC) is now done**:
  `try_send` (op 11) / `try_recv` (op 12) move bounded, copied, **data-only**
  messages through per-end inbound ring queues (Model A: send on end i → peer
  inbound), honoring the D6 copy ordering (send: preflight → copy_from_user into
  stack staging → enqueue; recv: peek → copy_to_user → infallible commit, so a
  copy-out fault leaves the message queued). Statuses QUEUE_FULL/SHORT_BUFFER/
  PEER_CLOSED/EMPTY (10–13) added; `JE_ABI_MSG_MAX=64`, DEPTH=4 (policy bounds,
  ~2.1 KB pool). D9 data-only is enforced (R10 cap_count must be 0). An
  owner-approved touch to the tested lifecycle path discards/zeroes inbound
  queues on close/free (free condition unchanged). Proven ring-3 by the `M3c-s4`
  self-loopback marker (direction, FIFO, SHORT_BUFFER, copy-out-fault rollback →
  delivered once, QUEUE_FULL, data-only reject, drain-then-PEER_CLOSED) — which
  does NOT prove cross-address-space delivery (one process holds both ends; that
  needs transfer). **ABI forward note:** the single RDX scalar can't return
  received capability handles, so **step 5 MUST add a new op / out-register
  extension, not overload `try_recv`** (R8/R9 and RDX[63:32] reserved now).
  **Step 5 (capability transfer) is now done** (frozen by ADR 0009 A5): `try_send`
  carries 0..2 caps (descriptors queued + inflight-retained on the target, D3.2);
  `try_recv` mints fresh attenuated handles in the receiver's table (D8). D9 =
  cycles allowed but reclaimed on last-handle removal (A5-1); stale cap → handle 0
  (A5-2); `maybe_free_record` got the `allocated` guard + detach-before-release
  discard order (A5-3); ABI uses the pre-reserved R8/R9 path (A5-4). Proven: hosted
  cap mint/dead-cap/cycle-reclaim + the `M3c-s5` kernel self-test (cross-TABLE
  transfer over a seeded channel, message across distinct tables, S+C reclaimed).
  **Transfer sequencing:** the sender must retain the transferred end until the
  receiver mints (closing first → dead cap, A5-2). **Ring-3 two-process transfer
  deferred to step 7** (recorded decision: multi-process choreography is step 7's
  subject; the spin-based two-stub proof raced the single-CPU cooperative
  scheduler). **Remaining D-SEQ:** step 6 (waits, owner-gated — edits `sched.c`),
  7 (bootstrap fixture + the deferred ring-3 two-process transfer proof). #8 closes
  when the sequence completes; #7's deferred refcount/overflow is already
  discharged (endpoints are genuinely destroyed + reclaimed on process death).
  **Step 6 (blocking recv waits) is now done** (owner-gated; frozen by ADR 0009 A6
  + A6-CLARIFY-1/2). `try_recv` with a nonzero R10 timeout blocks until WOKEN /
  PEER_CLOSED / TIMED_OUT / CANCELLED (A6-2 internal-only in v1); R10==0 is the
  unchanged non-blocking poll. Mechanism: a per-end waiter + one
  `je_endpoint_wait_arbitrate` CAS, `je_sched_block_current` (pre-park
  `je_uframe_check` + 2-byte `int 0x80` rewind), a `wait_pending` continuation
  prologue in `je_syscall_dispatch` that finishes from the owned reference (no
  handle re-resolve, copy-out after the context switch), guarded
  `BLOCKED_IPC→READY` wake, one idempotent `je_endpoint_wait_release` owner (called
  by `mark_dying`). New ABI statuses `TIMED_OUT=14`/`CANCELLED=15`; R10 timeout
  bounded by `JE_IPC_MAX_WAIT`. Proven: `M3c-s6` (TIMED_OUT end-to-end, ring 3) +
  hosted `test_endpoint` 27–28 (the pure waiter layer, all four causes via the
  CAS, reserved-head, release). **Deferred to step 7:** the full two-process ring-3
  WOKEN handshake (the wake bridges are wired; step 7 schedules two cooperating
  user processes) — honest, same as the step-5 ring-3 deferral.
  **Step 7 (bootstrap fixture) is now done — M3c (#8) is functionally complete.**
  `M3c-s7` injects a kernel-seeded channel into two ring-3 processes and discharges
  BOTH deferrals end-to-end: the two-process WOKEN handshake (B confirmed parked
  before A exists -> A's send is a causally-forced wake) AND the two-process
  capability transfer over the WOKEN continuation (A sends c1 as a cap, B mints it
  on wake, A closes c1 BEFORE B uses the mint -> retained-ownership proof via the
  post-close send reaching A's c0), then a bounded ping-pong + DONE teardown, both
  exit 0, pool reclaims to 0. Blocking recv (step 6) is what made the two-process
  ping-pong deterministic (a parked receiver yields; no spin deadlock). No new
  kernel surface. **D-SEQ steps 1-7 all complete.**
  **Remaining D-SEQ:** none.

Full qualification evidence for M3a is in `docs/M3A_RETRO.md`; the per-PR ledger is
`docs/IMPLEMENTATION.md`. Everything above is QEMU/hosted-verified and
peer-cross-reviewed (AI-assisted, not independent human qualification); the owner is
the merge gate.

**Decision-record policy (active 2026-10-19, `docs/adr/0000-decision-record-policy.md`):**
every recorded decision from now on carries three parts — (1) the decision + why,
(2) predictions if other choices had been made, (3) flex options to revisit (tag
`FLEX (planning review)` for ones to take to the main planning agent at the
pause). A one-time **retroactive sweep** PR brings ADRs 0001–0009 + prior
owner-gated calls into this format; it is scheduled right **before** the final
doc-sync + retro at the M3c/MP2 pause. ADR 0009's A5 amendment (step-5 transfer:
D9 reclamation invariant, stale-cap S1 null-marker, the `maybe_free_record` guard,
the R8/R9 transfer ABI) is the first decision written in the new format.

## M3a #5 step (b): recoverable user copies + exception fixup — observed 2026-10-17T19:50:00+00:00

Branch `feat/m3a-5b-user-copy`. The FINAL M3a piece. `kernel/core/usercopy.c` +
`kernel/arch/x86_64/usercopy.S` add `copy_from_user`/`copy_to_user` (ADR D9) and
the dev-only `copy_probe` syscall (op 3). Every copy prevalidates the span
(canonical, no wrap, in-arena, per-page present+permission) before touching it,
copies through ONE approved assembly site, and a per-thread copy scope + the
#PF-handler fixup (`je_usercopy_try_fixup`, called from `je_trap_dispatch`)
recover ONLY a not-present (P=0) fault at the EXACT copy-site RIP with an active,
owner-matched, direction-matched scope and CR2 in the declared span. Any other
kernel #PF — protection fault, different RIP, no scope — stays fatal. A recovered
fault → JE_ABI_INVALID (treated as a copy failure, not masked).

Dispatch order: `cs==8` vector-14 → try fixup → else existing kernel PANIC; then
the user int 0x80 / fault-terminate paths unchanged. The dev-only fixup-injection
op (op 4) and its user stub are compiled ONLY under `-DJE_DEV_COPY_INJECT`
(TEST_MODE 11); a `make check` `nm` assertion proves those symbols are absent
from the default kernel.

Peer: design done via a 2-round cross-talk DEBATE (the new peer-debate tool). The
debate's key refinement: recover only P=0 not-present faults, treat a protection
fault after validation as fatal (don't mask a validator bug). Codex's follow-up
check corrected the injection to run from a real CPL3 syscall context (genuine
owner/scope), flagged a stale bootstrap self-test call (removed), and had me
tighten `cs&3==0` to exact `cs==8`.

Tested in QEMU (this env): `make check` PASS (op-3 decode test + ABI isolation +
dev-inject-absent-from-default-ELF assertions), `make smoke` PASS — the boot runs
the copy_probe battery (valid+zero OK; null/kernel-range/len>256 INVALID; valid
bytes verified moved) with peer B live. `make integration` PASS — 11 modes; mode
11 proves the fixup RECOVERS a copy-site #PF (dev inject) while modes 4/5 still
PANIC on a kernel-origin fault (not wrongly recovered). All three CI smoke
profiles pass; M2's 4 markers unchanged. Honest limits recorded: no SMAP/SMEP, so
the net covers the approved site only; fail-closed under uncorrupted-kernel
invariants, not absolute. This COMPLETES the ADR 0007 M3a userspace boundary
(#2–#5). AI-assisted review only; not independent human qualification.

## M3a #5 step (a): v0 syscall dispatcher — observed 2026-10-17T17:40:00+00:00

Branch `feat/m3a-5a-syscall-dispatch`. The DPL3 `int 0x80` gate now dispatches a
real experimental v0 syscall (previously it routed straight to death). New
`kernel/core/syscall.c` (`je_syscall_decode` pure + unit-tested; `je_syscall_dispatch`
glue), `include/jeios/abi.h` (separate, explicitly-numbered ABI status/ops,
never the internal `status.h`). trap.c routes user-origin `int 0x80` to the
dispatcher before the fault path; kernel-origin 0x80 stays fatal.

Ops: `abi_query(which)` (version/profile in RDX), `yield()` (reschedule via the
kind-aware path, RAX=OK written before switching), `exit(code)` (DEATH_EXIT +
code). Unknown op → UNSUPPORTED (full 64-bit decode); recognized op with a
nonzero reserved arg → INVALID, no side effect. Every non-switching return goes
through `je_sched_return_to_user` → re-validates+sanitizes the frame, terminating
(DEATH_BAD_RETURN) rather than IRETQ a bad frame.

Owner-approved tested-code touch (kept M2+step4 green): the step-4 death record
gained `reason {FAULT,EXIT,BAD_RETURN}` + `exit_code` (the `0xbad` magic vector
removed); a sibling `je_sched_exit_current` was added rather than renaming the
tested `je_sched_fault_terminate`.

Peer: pre-write design vote (both agents) caught the direct-return-skips-validation
gap (fixed by `je_sched_return_to_user`). Codex cross-review found no
escape/UAF/ABI-leak and drove three closures: test asserts RDX=0 on INVALID/
UNSUPPORTED; a new QEMU stub proves BAD_RETURN *through the syscall return path*;
and these doc updates.

Tested in QEMU (this env): `make check` PASS (+`test_syscall` + abi.h isolation
grep), `make smoke` PASS — boot proves exit(42)→clean EXIT, abi_query/yield
return results to ring 3, unknown op→UNSUPPORTED+survives, bad-frame-on-return→
BAD_RETURN death, all with peer B live and page accounting restored.
`make integration` PASS (10 modes). All three CI smoke profiles pass locally;
M2's 4 markers unchanged. Next: step (b) — copy_from/to_user + copy_probe +
exception-fixup copy sites (the only genuinely tricky remaining piece). NOT yet
claimed: any user-copy/pointer handling, a stable ABI. AI-assisted review only;
not independent human qualification.

## M3a #4 step 4: fault-local process death + D6 int 0x80 gate + H1 fix — observed 2026-10-17T15:30:00+00:00

Branch `feat/m3a-4-step4-fault-local`. #4's core acceptance: a synchronous USER
fault — or a user `int 0x80`, or bad-return-state — terminates ONLY the
offending process; the kernel and peers keep running.

- `je_trap_dispatch` routes a user-origin ((cs&3)==3) synchronous fault in the
  explicit terminate-list {0,1,3,4,5,6,7,10,11,12,13,14,16,17,19} OR a user
  `int 0x80` to `je_sched_fault_terminate`. NMI(2)/#DF(8)/#MC(18)/reserved and
  any kernel-origin fault stay fatal PANIC (unchanged).
- `je_sched_fault_terminate` marks the current user thread DYING (death record),
  returns a LIVE successor's frame. It does NOT free the dying task's kstack/CR3
  (kernel C still runs on that RSP0 kstack); the bootstrap reaper frees it later.
  Triple-checks cs/ss + kind==USER + frame==rsp0_top-sizeof before acting.
- **H1 security fix:** `je_sched_interrupt` previously called `abort_scheduler()`
  (whole-kernel halt) when a *next* user thread's saved frame failed
  `je_uframe_check` — a userspace DoS (ring-3 sets a bad RSP, never faults,
  kernel dies at its next dispatch). The shared `select_and_install_next` now
  terminates that thread and retries another candidate; bootstrap is always a
  live fallback. A user task can no longer halt the kernel via a bad saved frame.
- D6 gate: `idt[0x80].attributes=0xee` (present, DPL3, IST=0, 64-bit interrupt
  gate — IF cleared on entry). Only vector 0x80 is user-reachable; kernel 0x80
  PANICs; every other gate stays DPL0 (user int of any other vector -> #GP ->
  terminate). No syscall handler yet — a user int 0x80 is an unsupported op
  routed to death (ADR amendment option 3 satisfied).
- Reaper `je_sched_reap_dead` (bootstrap, slot 0, asserts IF=0): for each DYING
  user slot, assert not-current / not-active-CR3 / not-active-RSP0, then
  `je_sched_drop_user` (restore kstack guard) + `je_vm_destroy`. New DYING slot
  state; `vm.c` UNTOUCHED (the vm record stays READY until destroy; D5's
  DYING/REAPABLE are modeled in the scheduler slot). Invariant recorded:
  single-CPU, PCID off, IF=0 in the fault handler — SMP would need a per-CPU
  off-stack handshake.

Tested in QEMU (this env): `make check` PASS, `make smoke` PASS — the normal boot
runs the two-task acceptance: A (reads an unmapped arena page) dies by user #PF
(vector 14, cs=0x33, CR2=the unmapped page); B (heartbeat loop) is proven to
advance AFTER A's death (heartbeat + dispatch + preemption all strictly increased
from the post-death snapshot); A is reaped from bootstrap while B stays live; then
the user `int 0x80` and bad-RSP(H1) death causes are each exercised and
terminated fault-locally with B still running; B torn down with page accounting
back to the pre-step-4 baseline. `make integration` PASS — 10 modes; mode 10
FLIPPED from step 3: user `xorps` #UD now terminates the task (fault-local) and
the kernel reaches the normal halt (asserts no "JeiOS fatal:"). All three CI
smoke profiles pass locally; M2's 4 markers unchanged (owner sign-off condition).

Peer: pre-write design vote (both agents) found the H1 kernel-DoS in merged main
and the "first entry rides a real trap" rule; Codex cross-review then confirmed no
UAF/escape and flagged hardening (reaper now self-asserts IF=0; ss checked in
terminate) + test gaps (int 0x80 and H1 death now runtime-tested, not just
claimed) + wording, all addressed.

Honest claims: synchronous user faults, bad-return-state, and user int 0x80 kill
only the offender; a peer is shown to progress after the death; the faulted
process is reclaimed (per-cause reaps + aggregate page-count equality); the D6
gate is present and leads to death. NOT claimed: two-task isolation qualification,
syscalls/ABI, copy-in/out, a process exit path, general/complete reclamation
across all paths, SMP. #4 acceptance is met; #5 is the integrated qualification +
syscalls. AI-assisted review only; not independent human qualification.

## M3a #4 step 3: CPL3 entry/return — real ring-3 execution — observed 2026-10-17T12:20:00+00:00

Branch `feat/m3a-4-step3-cpl3-entry`. The scheduler now IRETQs into ring 3.
Added: thread kind (kernel/user) + `je_thread_create_user` (synthesizes the
initial user frame on the process's held kstack, requires NX per D2);
`je_sched_interrupt` validates the incoming frame by kind and, for a user next
thread, runs `je_uframe_check` (pure, hostable, unit-tested) to validate+sanitize
RFLAGS, sets TSS.RSP0, loads the process CR3 before the single IRETQ; kernel
threads get RSP0=0 + kernel root. `je_sched_reap` excludes user threads (kstack
is vm-owned — avoids double-free); `je_sched_drop_user` detaches + restores the
kstack guard before `je_vm_destroy`. D7 `je_arch_cpu_policy_init` sets
CR0.EM/MP/TS, clears CR4.OSFXSR/OSXMMEXCPT/OSXSAVE/FSGSBASE, nulls DS/ES/FS/GS.
Gates stay DPL0 — the D6 `int 0x80` gate lands in step 4 (ADR amendment: present
by end of #4, wired to fault-local death; option 3, owner-approved).

Peer process: pre-write design vote (both agents) caught the reap double-free and
the first-entry-must-ride-a-real-trap rule; Codex cross-review then found four
defects, all fixed: incomplete segment/FSGSBASE normalization, D7 marker
overclaim (now proven by an executing #UD test, mode 10), an RFLAGS/DF
contract-vs-impl mismatch (DF now cleared, test asserts it), and an ignored
`drop_user` teardown result (now checked).

Tested in QEMU (this env): `make check` PASS (+`test_uframe`), `make smoke` PASS
(a built-in ring-3 loop reached CPL3, survived >=16 preemptions, advanced its
heartbeat ~9-17M times, torn down clean), `make integration` PASS (10 modes; new
mode 10 = user `xorps` at CPL3 -> `#UD` from a cs=0x33 frame, hardware D7 proof).
All three CI smoke profiles pass locally; M2 markers unchanged (sign-off
condition). Next (step 4): fault-local offender death + install/wire the D6
`int 0x80` gate to it. NOT yet claimed: inter-process isolation, fault-locality,
the gate, syscalls. AI-assisted review only; not independent human qualification.

## M3a #4 step 2: real per-process page tables + CR3 (CPL0) — observed 2026-10-17T11:45:00+00:00

Branch `feat/m3a-4-step2-vm-backend`. The real arch backend
(`kernel/arch/x86_64/vm_backend.c`) now implements the frozen #3 `je_vm_ops`
against actual page tables, producing a real per-process CR3. `paging.c` grew a
small seam: `je_kernel_pd_phys`, `je_user_alias_set/get` (RW↔RO_NX CAS, A/D
masked), `je_arch_cr3_read/load`, `je_arch_kernel_root_phys`. Topology matches
the merged #3 audit: `PML4[0] P|U → PDPT[0] P,!U (shared kernel PD) → PDPT[1]
P|U → PD[0] P|U → PT`. Intermediates also carry W (effective write = AND of W
across levels under CR0.WP); leaves encode CODE=RX, RODATA=R/NX, DATA=RW/NX.

Owner-approved, done: (1) hardened `je_arch_page_guard`/`restore` to refuse any
non-exact-RW leaf (fixes a latent bug where the guard path could silently
un-demote a published RO_NX alias); (2) `_Static_assert(JE_PHYSICAL_LIMIT <=
JE_VM_ARENA_BASE)` + `<= 4 GiB` pinning the uint32_t root. Two real bugs found
during bring-up and fixed: guard rejected fresh stack pages until A/D (bits 5,6)
were masked; CR3 load triple-faulted until all intermediate entries carried W.
Cross-review (Codex) then found and I fixed three more: OOB on an invalid
`frame.process` in install/clear_pte, missing alias-enum validation, and
`read_user_pte` not rejecting a present leaf lacking U.

Tested in QEMU (this env): `make check` PASS, `make smoke` PASS (mode 0 drives
the full build→audit→CR3 load→arena read→data RW→restore→destroy path with clean
page accounting), `make integration` PASS — all 9 fault modes, incl. new mode 8
(read arena under kernel root → `#PF err 0 cr2=0x40000000`) and mode 9 (write
USER_CODE under process root → `#PF err 3 cr2=0x40000000`).

SCOPE: CPL0 only. NO ring-3, NO `sched.c` CR3 switch, NO TSS.RSP0 — step 3.
Does not claim U/S ring-3 enforcement or user-mode NX fetch (unprovable at CPL0).
Next (step 3): scheduler installs CR3 + TSS.RSP0, validates the user return
frame, IRETQs to ring 3. AI-assisted review only; not independent human
qualification.

## M3a #4 step 1: ring-3 GDT descriptors — observed 2026-10-17T11:00:00+00:00

First step of M3a #4 on branch `feat/m3a-4-step1-gdt-user` from `main`
`11375f1`. Appends user (DPL=3) descriptors to the GDT (grown 5→7 entries):
`gdt[5]` user data (selector 0x2b), `gdt[6]` user code (selector 0x33, L=1),
after the existing kernel/TSS selectors (ADR 0007 D6). No user task executes
yet — this only makes the selectors available for the CPL3 entry work in later
#4 steps.

Implemented: GDT growth + the mode-2 fault-test fix. Because the GDT now covers
index 6, the old mode-2 selector 0x28 became valid (would not fault); the test
and tools/integration.py now use 0x38 (index 7, past the limit) → #GP error 0x38.
A pre-write design review caught that leaving 0x28 would make the test hang
rather than fail. IMPLEMENTATION.md and DEVELOPMENT.md mode-2 references updated.

Tested (this env; QEMU/GRUB available): `make check` PASS, `make smoke` PASS,
`make integration` PASS — mode 2 observes #GP vector=0x0d error=0x38; all seven
fault modes pass. Descriptor bytes decoded (data P=1 DPL=3 type=0x2; code P=1
DPL=3 type=0xa L=1). No M1/M2 regression; no sched.c change in this step.
Planned next (#4): step 2 real per-process page-table backend + CR3; then CPL3
entry/TSS.RSP0; fault-local death; the two-task QEMU containment proof. The
`int 0x80` DPL3 gate is deferred to #5 (a handler-less DPL3 gate would be
user-triggerable kernel death). Owner signed off on the upcoming sched.c change
(per-process reap + ring-aware frame validation) conditioned on no regression.
AI-assisted review only; not independent human qualification.

## Boot toolchain verified in this environment — observed 2026-10-17T08:08:00+00:00

The QEMU/GRUB boot toolchain is installed and working on the current build
machine (Ubuntu 24.04 "noble"), so the serial-marker boot evidence that gates
M3a #4 and later runtime claims can be produced here:

- `qemu-system-x86_64` 8.2.2 (`qemu-system-x86`), SeaBIOS present
  (`/usr/share/seabios/bios.bin`). The kernel is a BIOS/legacy-PC target; OVMF
  (UEFI) is NOT required and its absence is irrelevant.
- `grub-pc-bin` (i386-pc modules at `/usr/lib/grub/i386-pc`), plus
  `grub-mkrescue`/`xorriso`, so `make iso` builds a bootable multiboot2 image.
  Note: the kernel is multiboot2-only (header magic `0xe85250d6`), so QEMU
  `-kernel` (multiboot1) CANNOT load it directly — GRUB is required.

Verified now at `main`: `make smoke` PASS (normal boot prints the ordered markers
through "initialization complete; halted", NX enforced) and `make integration`
PASS (all seven CPU-fault-injection cases, including the double-fault/IST path).
This supersedes the earlier note that emulation tools lived only in transient
`/tmp/jeios-tools` wrappers; a working toolchain is now present via system
packages. A fresh environment still needs `qemu-system-x86` and `grub-pc-bin`
installed before boot evidence can be produced. Documentation only; no code or
kernel behavior changed.

## M3a #3 hosted VM/loader implemented — observed 2026-10-17T06:30:00+00:00

Inspected `auto44e/jeios` branch `feat/m3a-3-vm-loader` from `main`
`2c4a90d`. No open PRs at inspection. This change adds the issue #3
per-process address-space builder and bounded image loader as a HOSTED-ONLY
model (ADR 0007): `include/jeios/vm.h`, `kernel/core/vm.c`, the hosted fake
backend `tests/vm_fake.c`, the unit test `tests/test_vm.c`, and the Makefile
`test_vm` target. `vm.c` is also compiled into the kernel ELF.

Implemented: `je_vm_image_validate` (D8 arithmetic/overlap/stack-guard/entry
rules), `je_vm_build` (NX gate, 2-process limit, atomic underflow-safe admission
with the concrete 5-page kstack run, D3 publication order
zero→copy→unmap→demote→flush→install for USER_CODE/USER_RODATA, RW/NX for
USER_DATA), a reverse-order build journal with teardown
clear→flush→restore→free, and `je_vm_audit` (arena-bounded walk; per-leaf
ownership, kind/perm, alias state, completeness and one-to-one; cross-process
root/extent disjointness; contiguity of registered extents verified).

Tested (hosted, ASan/UBSan): `make check` adds `test_vm` PASS covering
validation, admission, audit, permissions, zero-before-expose, the
fail-at-every-step rollback proof (exact je_pages restoration), and teardown. A
targeted probe confirms oversized-segment rejection and no pointer-overflow UB.
Planned: issue #4 (CPL3 entry, real CR3/TSS.RSP0, syscalls) consumes this
model's contracts and the held kstack run; CI/#18 and coding-policy/#19 alongside.
Blocked/unverified: NO real paging, CR3/TLB, CPL3 execution or U/S/NX hardware
enforcement is claimed here — all #4 in QEMU. No independent human qualification;
remote CI for this head must be observed separately.

Authorship (multi-agent model, AGENTS.md): the lead agent wrote `vm.c` and the
header; the fake backend `vm_fake.c` was authored by one external coding agent
and the test `test_vm.c` by another, so implementation and test have distinct
authors. Two rounds of cross-review (each agent reviewing code it did not write)
found and fixed a validation quota underflow, a teardown-flush ordering gap, a
journal rollback hole, NULL-blob/pointer-overflow UB, an extent-id assumption,
and an over-claiming audit; a third round confirmed the fixes. AI-assisted
review, NOT the independent human qualification the critical profile requires.

Validation commands: `ASAN_OPTIONS=detect_leaks=0 make check`, `git diff --check`
passed at this base; local PASS is not remote CI.

## Multi-agent collaboration model — observed 2026-10-17T06:05:00+00:00

Owner direction 2026-10-17: work may be done by multiple AI coding agents as
peers (shared authorship, democratic decisions on open questions, mutual
pushback), bounded by the greater OS vision. The governing rules are recorded in
[AGENTS.md](../AGENTS.md) ("Multi-agent collaboration model"). Key points: the
frozen ADRs/ROADMAP and the honesty/critical-profile discipline are the
non-negotiable guiding star; authorship is spread across agents (ideally
implementation, test and fake written by different agents); open design choices
are decided by recorded majority with pushback expected in every direction; and
three hard overrides beat any vote — a verified fact (compile/test/CI), any change
touching a frozen contract or tested code (escalated to the owner), and the
honesty rules (local≠remote CI, implemented/tested/planned/blocked, no invented
reviewers, AI-assisted review is NOT independent human qualification).

Implemented: contributor-guidance/process documentation only. No kernel, build or
boot behavior changed; M1/M2 AMD64 remains the implemented baseline and the mobile
scaffold is unchanged. Planned: continue M3a #3 under this model. Blocked/
unverified: nothing new; this entry adds no code and makes no runtime claim.
Validation: `ASAN_OPTIONS=detect_leaks=0 make check`, `git diff --check` and
relative-link checks passed locally at this base; remote CI observed separately.

## M3a boundary-contract ADR — observed 2026-10-17T00:38:00+00:00

Inspected `auto44e/jeios` branch `docs/adr-0007-m3a-boundary` created from `main`
at `33604256cc3b47634ded2b7c341aa430a90b0867`. No open PRs existed in either
repository at inspection. This change adds
[docs/adr/0007-m3a-userspace-boundary.md](adr/0007-m3a-userspace-boundary.md) and
will be submitted as an unmerged review PR for [issue #2](https://github.com/auto44e/jeios/issues/2).

Implemented: documentation only — an accepted ADR that freezes the M3a
userspace-boundary contract (scope, NX-required launch profile, 2 MiB arena and
ownership rules, admission bounds, lifecycle, ring-3 entry/return, enforced
no-FPU/SIMD policy, built-in image + experimental ABI v0, user-copy discipline,
nonblocking syscall model, and the reserved-but-unimplemented M3b/M3c surface). No
kernel, build, test or boot behavior changed. Runtime remains core M1/M2 AMD64 and
the mobile planning scaffold.

Planned: M3a implementation against this contract — issue #3 (private VM + bounded
image), #4 (CPL3 scheduling + local faults), #5 (syscalls + user copies), #6 (M3a
qualification). CI/enforcement sequenced with #18 and coding-policy evidence with
#19. Blocked/unverified: no M3a code, no independent review, no hardened
qualification, no FPU/SMP/userspace runtime; remote CI for this new head must be
observed separately. M3A.md's M3b/M3c numbers remain proposals, not frozen here.

Decision rationale (for all agents/people): the project's discipline is to freeze
the contract in an ADR before writing kernel code; M3A.md explicitly deferred its
numbers/ABI to "the first ADR"; AGENT_START lists #2 (boundary contracts) as the
first work-sequence item and the prerequisite for #3–#6. ADR 0007 is therefore the
correctly-sequenced, highest-value first change. It supersedes the
"proposed/recommendation" wording of M3A.md sections 2–5 for M3a only.

Validation: `ASAN_OPTIONS=detect_leaks=0 make check` passed core, boot-parser,
allocator and ELF checks on GCC 13.3.0 at this base commit (documentation-only
change; no boot code touched, so the emulator workload was not rerun). Relative
Markdown targets, conflict markers and `git diff --check` checked before
publication. Local PASS is not a substitute for remote CI on the new head.
Historical evidence below retains its original revision/time; this entry
supersedes earlier current-status wording.

## Ecosystem coding-policy refresh — observed 2026-10-17T00:12:28+00:00

Owner direction: 2026-10-16T20:01:26-04:00, reaffirmed
2026-10-16T20:08:52-04:00. Inspected `auto44e/jeios` branch
`docs/posix-ldap-20261016` at `1c394d0aa878ce1404df1c70fe25b3232a276d3e`.
Core PR #17 and mobile PR #8 were observed open/unmerged. This update extends
PR #17; core #19 is open for policy audit/enforcement alongside CI #18.
No board membership or reviewer assignment is claimed.

Implemented: documentation policy and contributor guidance only; runtime remains
core M1/M2 AMD64 and mobile planning scaffold. Planned: ecosystem-wide NASA/JPL-
inspired rules, risk profiles, baseline audit and staged automated enforcement.
Blocked/unverified: no complete existing-code audit, independent review or NASA
certification; no new analyzer/workflow or native mobile qualification.
Core CODING_STANDARD.md is the shared proposal in PR #17; mobile references it.
Next: core M3a and mobile MP0, with bounded #19/#18 enforcement work alongside.

Validation: `ASAN_OPTIONS=detect_leaks=0 make check` passed core, boot-parser, allocator and ELF checks. The previously observed local LeakSanitizer process-inspection limitation remains; ASan/UBSan stayed enabled.
Documentation links, conflict markers and `git diff --check` checked separately
before publication. No emulator rerun for documentation-only edits. New-head
remote CI must be observed separately. Historical evidence below retains its
original revision/time; this entry supersedes earlier current-status wording.

## CI planning refresh — observed 2026-10-16T23:57:44+00:00

Owner request: 2026-10-16T19:55:53-04:00. Inspected core PR #17 head
`507c06ab397add3d42ba2440670ace7af04ddeab` and mobile PR #8 head
`b2ab0f0a75cc6e8902408eb222474de592adfdc5`; both remain open/unmerged.
This follow-up updates core branch `docs/posix-ldap-20261016` in PR #17.

Added [CI_STRATEGY.md](CI_STRATEGY.md), near/long-term stages, replayable failure
injection, outcome/flake reporting and source references. Core #18 is open as the
first implementation task; no coding agent launched or board membership claimed.
Existing CI already tests hosted code and AMD64 QEMU; new jobs/fuzz infrastructure,
ARM64 tests and broader recovery qualification remain planned. No workflow or
kernel behavior changes. M3a and mobile MP0 remain next; all prior requirements
remain preserved. Current-head CI for this new commit must be observed separately.

Validation: `ASAN_OPTIONS=detect_leaks=0 make check` passed core, parser, allocator
and ELF checks. Leak scanning remains unavailable locally; ASan/UBSan active.
Relative Markdown targets, conflict markers and `git diff --check` passed.
No local emulator rerun for this documentation-only follow-up. Prior evidence retains
its original commit and timestamp; this header supersedes then-current PR wording.

## Current refresh — observed 2026-10-16T23:08:34+00:00

Inspected `auto44e/jeios` main at `416fb2b20923f3bddd47d26d3ed8f7efa249635d`.
Core #12 merged 2026-10-15T00:47:05Z; #1/#10/#11 also merged.
No open PRs existed in either repository at initial inspection. This update uses
`docs/posix-ldap-20261016` and will be submitted as an unmerged review PR.
Core #2-#9 and #13-#16 open; #15/#16 created by this refresh.

Implemented: core M1/M2 AMD64 foundation; mobile planning/manifest scaffold only.
Planned: POSIX.1-2024 and built-in LDAPv3 are now core requirements, owner decision
2026-10-16T19:04:18-04:00. Organizational enrollment remains optional.
Blocked: native POSIX/directory runtime awaits isolation, services, storage,
networking and security prerequisites; mobile also awaits ARM64 qualification.
Next bounded tasks remain core M3a (#2 contracts first) and mobile MP0 (#1).
New standards ADR/qualification planning can proceed without claiming runtime work.

Preserve all historical decisions below, including immutability/off switch,
versioned updates independent of immutability, F2FS selection, crypto relock,
zero trust, customization, Android planning, multitouch and hardware MFA.
Earlier references to then-open PRs or candidate-only LDAP are historical.
Core `make check` built and passed core tests but failed at LeakSanitizer
process inspection (`/proc/.../task` unavailable). Rerun
`ASAN_OPTIONS=detect_leaks=0 make check` passed core, boot-parser mutation corpus,
allocator and ELF checks; ASan/UBSan remained active.
Relative Markdown targets, whitespace and conflict-marker checks passed.
GitHub CI for the new PR head is separate from local results.
Project board verified: new core #15 and #16 are present as Todo; prior merged
PRs are Done. Refreshed stale core #2-#9 baseline guidance and mobile #5 parity.
No boot behavior changes; no emulator/device tests or POSIX/LDAP runtime tests claimed.


## F2FS approval and agent tasks — 2026-10-14T20:43:47-04:00

Owner approved F2FS as the provisional mobile default, subject to actual JeiOS
qualification. This supersedes earlier candidate-only wording; device support,
feature flags and production readiness are not implied. Btrfs/OpenZFS remain
long-term default targets; ext4 remains the initial core bring-up backend.

Ready-to-start bounded work:
- [Core #13](https://github.com/auto44e/jeios/issues/13): backend contract ADR,
  hosted fake-block-device and conformance harness.
- [Core #14](https://github.com/auto44e/jeios/issues/14): F2FS format/provenance
  review and bounded read-only hosted metadata probe with negative tests.
- [Mobile #7](https://github.com/auto44e/jeios-mobile/issues/7): mobile F2FS
  workload/qualification profile and shared-backend integration plan.

These issues are open/unassigned at creation; no coding agent has been launched
by creating them and project-board membership has not been verified. Hosted
prototypes can proceed independently while runtime integration waits for M3/M4/M5
and relevant mobile execution/driver gates. No writes to real user media.
M3a remains the next core kernel gate; MP0 remains the mobile architecture gate.


## Latest storage decision — 2026-10-14T20:40:22-04:00

Owner selected multiple first-class long-term default backends: Btrfs, OpenZFS
and a suitable mobile filesystem. F2FS is the proposed mobile candidate; suggested
profile assignments remain subject to qualification. Native APIs provide coherent
workflows, with backend-specific capabilities and tested guarantees. Owner-provided
system defaults are supported through explicit interfaces, boot/recovery contracts
and honest experimental profiles. Changing defaults is not an in-place conversion.

Shared STORAGE.md records scope and staged gates. ext4 remains initial bring-up;
core M3a/mobile MP0 remain next. Documentation-only decision; no filesystem,
migration tool or plugin runtime was added. New text checked for whitespace and
conflict markers; no executable tests rerun. No merge performed.


## Latest owner decision — 2026-10-14T20:30:05-04:00

Protected/versioned system deployments are the seamless default, with full
customization and an authorized immutability off switch. OS and built-in-app
forward updates/rollback must remain intuitive and available independently of
write protection. Shared STORAGE.md specifies native lifecycle/schema contracts,
mutable-mode change handling, retained versions, coherent activation and recovery.
Data migration and trust/anti-rollback limits remain explicit; no promise that
arbitrary direct edits or external effects can be automatically reversed.

Planning only; no update manager, filesystem or native app runtime was added.
Core M3a/mobile MP0 remain next; reserve storage/update contracts before M5 format
freeze. Documentation whitespace/conflict-marker checks only; runtime tests
not rerun. Existing PR branches updated; no merge performed.


## Latest compatibility requirement — 2026-10-14T20:15:24-04:00

Owner expects the future AOSP-derived Android layer to carry additional crypto
relock/unlock adaptation for existing apps. Mobile ANDROID.md now specifies
framework/ART/Binder/JNI and native-resource coordination, bounded state saving,
domain teardown/restoration and app-specific qualification. Core ENCRYPTION.md
records host ownership of keys/policy and the compatibility boundary.
No hidden key retention or weaker host policy to keep a foreign app running.
Graceful restoration is a target; unsupported unsaved-state recovery is disclosed.

Planned only; no Android runtime or relock implementation added. Core M3a/mobile
MP0 remain next. No executable tests rerun for these documentation additions.
Changes remain on existing PR branches; no merge performed.


## Latest native-app requirement — 2026-10-14T20:12:45-04:00

Owner requires native JeiOS apps to gracefully handle cryptographic relock/unlock.
The shared ENCRYPTION.md now makes this an SDK/toolkit/application conformance
contract: protected drafts/checkpoints, bounded lifecycle events, OS-enforced
revocation, process reconstruction and fresh authority after unlock. Apps cannot
veto locking, and callback acknowledgement alone does not prove memory cleanup.
Interrupted external operations require reconciliation, not automatic replay.

Planned only; no API/runtime implementation added. Core M3a and mobile MP0 remain
next. Documentation structure/whitespace checked; no executable tests rerun.
This update stays on existing PR branches, with no merge performed.


## Latest requirement — 2026-10-14T20:08:17-04:00

Owner wants ordinary device/session locking to support cryptographic relock:
withdraw protected user-domain keys and plaintext access to regain a defined
pre-first-unlock-like state without reboot. Shared core ENCRYPTION.md specifies
proposed lock profiles, key/lifecycle/DMA cleanup, strong-credential reopening,
background-service exceptions, failure handling and acceptance gates.
This is an ecosystem-wide requirement, including mobile and compatibility apps.
No universal reboot equivalence or implemented encryption is claimed.

Planning only; core M3a and mobile MP0 remain next. Added prose was checked for
whitespace/conflict markers; reference URLs came from retrieved official Apple/AOSP
documentation. No kernel/build/emulator tests were rerun. Existing documentation
PR branches are updated; no merge is performed.


## Latest scope clarification — 2026-10-14T19:59:09-04:00

The approachable-defaults/deep-control philosophy applies to the entire OS and
ecosystem, not just security. DESIGN.md now records it as the overall product
vision across UI, apps, hardware, storage/networking, media, performance/power,
services, identity/sync, updates and developer workflows. Per-area presets and
expert controls are composable; UI detail, functionality, permissions and hardware
support stay distinct. Security is one application of this broader philosophy.
Earlier security-specific notes remain valid within that scope.

Documentation only; core M3a and mobile MP0 remain next. No runtime capabilities
or new test execution are claimed. This updates the existing review branches;
no merge is performed. Added prose checked for whitespace/conflict markers;
no new links or executable changes.


## Latest requirements clarification — 2026-10-14T19:55:55-04:00

Owner requires seamless everyday security plus granular power-user/developer and
company policy control, spanning deliberate permissive experimentation through
highly restricted deployments. The shared zero-trust proposal now distinguishes
UI complexity, security policy and hardware assurance; defines proposed profiles,
scoped exceptions, GUI/CLI/API parity, visible managed policy and honest restoration
limits. Profile names/settings are proposals. No runtime enforcement was added.
M3a and mobile MP0 remain next. This clarification updates existing documentation
PRs (core #12, mobile #6); no merge is performed by this update.

Validation: new Markdown structure, whitespace and conflict-marker checks passed.
No new links were introduced. Build/emulator tests were not rerun for this
documentation-only clarification; prior evidence remains at its recorded commits.


## Historical status — observed 2026-10-14T23:50:13.156Z

Repository main baseline: `e897e02f9038e164efbb590cfc0229e8300bff54`. GitHub was read at this
checkpoint: PRs #1, #10 and #11 are merged; no pre-existing open core or mobile PR
was returned. This documentation update will have its own reviewable PRs and is
not merged by this snapshot. M1/M2 remains the documented implementation baseline;
M3a is next, then M3b/M3c. Mobile remains planning/scaffold; MP0 is next.
No runtime implementation or test rerun is claimed by this refresh.

Owner added ecosystem-wide [zero trust](ZERO_TRUST.md) on 2026-10-14.
It is a planned cross-cutting framework; current CPL0 workers are not isolated.
The previously recorded build/emulator/CI evidence below remains historical and
scoped to its original commits; current-head CI has not been checked in this refresh.

### Verified merge history

| Event | GitHub event time (UTC) | Merge commit |
| --- | --- | --- |
| PR #1 | 2026-10-13T20:33:01Z | `03aa2f380d8e49262abd1720a3b8db37517cbb31` |
| PR #10 | 2026-10-13T23:05:02Z | `5f290b351181d8fa23b17ac5f493ff66ae9415b8` |
| PR #11 | 2026-10-13T23:10:59Z | `e897e02f9038e164efbb590cfc0229e8300bff54` |

Earlier date-only checkpoints below retain their original precision. Their
branch names, open-PR statements and test outcomes describe those past sessions.
The current baseline above supersedes historical continuation instructions.

Read AGENTS.md, IMPLEMENTATION.md, ROADMAP.md and [AGENT_START.md](AGENT_START.md).

## Status refresh and historical records

Owner preference recorded 2026-10-14: retain detailed timestamped history alongside
a clearly labeled current status. Every refresher must record an ISO-8601
observation time with timezone, repository/branch and exact inspected commit,
implemented/tested/planned/blocked states, PR/issue state, evidence and next task.
Separate an event's timestamp from the time it was verified. If an old record has
only a date, preserve that precision; never invent a time. Keep superseded plans
and previous failures as labeled history with links to their replacements.

Current status takes precedence over historical branch names or open/closed
wording. Preserve previous test results at their original commit and environment;
do not present them as rerun or current-head CI. State unverified items explicitly.
Use enough detail for an agent to resume without reconstructing the conversation.

## Owner intent

Fresh C kernel/core; modularity; modern hardware with an early AMD64 compatibility
path; graceful failures; GrapheneOS/CalyxOS-inspired security/privacy; eventual
mobile/IoT; NT-inspired object/service organization; macOS-inspired coherence;
Linux-style ricing and independent per-area UI complexity. Keep detailed records
for future agents. No existing kernel-source fusion. Owner explicitly selected Apache-2.0 on 2026-10-13.

## Implemented and tested

| Gate / component | Actual behavior | Boundary |
| --- | --- | --- |
| M1a exceptions | 256 IDT gates, normalized frames, TSS/IST for DF/NMI, bounded fatal diagnostics | Kernel faults halt; no user recovery |
| M1b boot context | Bounded Multiboot2 memory/module parser, reserved ranges, no retained loader pointers | Rejects overlapping maps; 128 records, 40 reservations |
| M1b page protection | New 4 KiB mappings, CR0.WP, NX where present, code/rodata read-only, guard pages | Early bootstrap still temporary RWX; later mappings are supervisor-only |
| M2 physical pages | First-fit contiguous extents, exact free validation and reservation exclusion | First GiB only; <=256-page allocation; caller serialization |
| M2 heap | 64 allocation records, <=1 MiB/object, zero-on-allocation and failure rollback | Page-granular; no slab/quarantine/generation safety |
| M2 execution | PIC/PIT IRQ0, eight slots including idle, seven guarded kernel workers | One CPU, CPL0, round-robin; no SIMD context or priorities |
| M2 lifecycle | Yield, tick sleep, exit and safe stack reclamation from bootstrap | No general wait queues, cancellation or SMP locks |
| Runtime checks | All GPRs, timer preemption, sleeps, retained heap bytes and page accounting | Self-test workload then halt, no application manager |

## Verification performed locally

- `ASAN_OPTIONS=detect_leaks=0 make check`: PASS with GCC 13.3.0 and binutils 2.42.
  Leak inspection is unavailable in this managed environment; ASan/UBSan remain
  active. Parser includes 20,000 mutations and truncated heap buffers. Allocation
  tests include exhaustion, fragmentation, exact frees, zeroing and rollback.
- Normal QEMU boot: PASS with NX, 128 MiB, all seven workers completing/reaped.
- NX-absent boot: PASS, including explicit weaker profile reporting.
- Low-memory (16 MiB, NX absent) and large-memory (1536 MiB, first-GiB clipping)
  profiles: PASS locally; both run the full seven-worker workload and are
  included in CI.
- `make integration`: seven real CPU fault cases pass with exact vector/error
  pairs, including stack exhaustion -> double fault on the emergency IST.
- ELF checks validate AMD64, Multiboot2, entry/load limits, separate segment flags
  and no unresolved runtime dependencies. Hardware permission tests are separate.
- GCC `-fanalyzer` with warnings as errors over every kernel C source: PASS.
- Python syntax, relative documentation links and `git diff --check`: PASS.
- No real-machine, ARM, GPU, userspace security or application ABI testing.

Emulation tools are the previous extracted QEMU 8.2.2 / GRUB 2.12 / xorriso 1.5.6
under `/tmp/jeios-tools`, with wrappers in `/tmp/jeios-bin`. Local commands use
PATH, LD_LIBRARY_PATH and QEMU_MODULE_DIR for relocation plus
`GRUB_PC_DIR=/tmp/jeios-tools/usr/lib/grub/i386-pc`. These transient paths are not
repository dependencies. Normal installed tools use DEVELOPMENT.md commands.

## Published verification evidence

Implementation commit `96d4dee037389e7b30132ad8fd7425c80a90ea1d` passed both
GitHub push and pull-request CI, including default sanitizer checks, normal and
NX-absent boots, 16/1536 MiB profiles and every fault-injection case.

- Pull request: https://github.com/auto44e/jeios/pull/1
- PR verification: https://github.com/auto44e/jeios/actions/runs/38317024232
- Branch verification: https://github.com/auto44e/jeios/actions/runs/38316970273

This subsequent handoff-only evidence update changes no tested implementation.
Check the current PR checks when continuing; never substitute a local PASS for
observed remote execution, or the original M0 checks for these new phase gates.

## Critical implementation contracts

- Boot/main context has interrupts disabled through map/allocator/thread creation.
  IRQ0 is unmasked/programmed only after scheduler state and stacks are ready.
- Handler entry preserves 15 GPRs; IRETQ restores the full 64-bit hardware frame.
  Never use SSE/AVX/FPU in this kernel before context management exists.
- Page tables describe only known complete RAM pages above 1 MiB and below 1 GiB.
  Boot, image, modules and low memory remain reserved by the allocator. Loader
  tables are copied before paging changes, and never dereferenced afterward.
- `je_pages`/`je_heap` portable functions are not internally thread-safe. Kernel
  wrappers use interrupt exclusion on the BSP. IRQ handlers must not allocate.
- Thread stacks are zeroed before guarding. An exited thread must never free its
  own live stack. Bootstrap remaps guards and frees only zombie stacks after all
  workers are done. Never free a stack still referenced by a scheduler frame.
- Sleep sets state under interrupt exclusion before the software scheduling
  interrupt. Idle checks readiness with interrupts off before adjacent STI/HLT.
- A fault kernel uses its own `build/N/` tree. Do not accidentally boot it as the
  normal image, and do not treat any timeout as proof of successful containment.

## Known limitations and hazards

No process boundary, handle/capability enforcement, secure boot, cryptography,
secure entropy, stack canaries, heap quarantine, KASLR, userspace copy recovery,
SMP, APIC, IOMMU, device lifecycle, filesystem, networking, GPU or desktop. The
remaining protection goals in DESIGN.md are future requirements. Heap pointers
can become stale after address reuse; no temporal safety is promised. One-page
stack guards do not catch arbitrary stack jumps. Large allocator scans/zeroing
occur with interrupts disabled and have no hard real-time latency guarantee.

Legacy PIC/PIT support is qualified only in the tested PC emulator profiles.
Unexpected IRQs/NMIs are fatal; broader spurious IRQ and platform fault handling
remain future work. The memory map is conservatively rejected if ranges overlap.
No RAM above 1 GiB is used even on larger systems. All seven workers are trusted
kernel threads in one address space. Never call them isolated processes/apps.

## Next bounded work: M3a userspace boundary

Start a new implementation branch from updated main. PR #11 is merged; read
its mobile design documents on main. Specify user/kernel address
layout, user code/data/stack mapping rights, private kernel stacks, TSS RSP0,
entry/return state, and copy-in/copy-out fault semantics before adding syscalls.
Keep CPU feature exposure restricted to state the scheduler preserves. Add a
small checked ELF loader or a deliberately bounded built-in user image, not a
large application compatibility layer at the same time.

Acceptance: two actual CPL3 tasks with distinct page tables run; a user task's
illegal access terminates only that task; kernel/other-task memory stays protected;
malformed syscall pointers cannot panic the kernel; argument lengths/overflows
are tested; no raw kernel pointer ABI. Add explicit handle rights, generation
and lifetime contracts in M3b, then bounded IPC in M3c. General wait queues/locks,
stack-canary/entropy planning and spurious interrupt handling need review before
calling M3 hardened. Do not jump straight to desktop, GPU or phone support.

## Open decisions

Hardened hardware baseline; actual old-PC support;
stable native ABI; toolkit/compositor strategy; service placement measurements;
and vendor GPU/phone targets. No external kernel source has been imported.

## Requirements addendum: 2026-10-13

Owner requested configurable init, PQ end-to-end encryption with honest RAM
protection, nearby device sharing and a filesystem recommendation. Added INIT.md,
ENCRYPTION.md and STORAGE.md; linked DESIGN.md, SECURITY.md, ROADMAP.md and README.
All are proposed contracts. Kernel implementation and next gate M3a are unchanged.
Ext4 is the first persistent candidate, not a completed driver or final production
default. Btrfs/OpenZFS remain evaluation options. No external source was imported.

Outstanding decisions at that point (license since resolved by ADR 0005): exact crypto protocol/library and entropy
source; hardware memory-protection qualification; init schema and numeric limits;
ext4 feature profile and writable-backend feasibility; storage encryption format.
Do not mistake illustrative service manifests for an implemented interface.

Validation for this docs-only change: `ASAN_OPTIONS=detect_leaks=0 make check`,
relative Markdown link validation and `git diff --check` passed locally. No boot
code changed, so emulator tests were not repeated locally. Prior kernel execution
evidence remains scoped to the earlier code; check the current PR for remote CI.

## Identity and storage expansion: 2026-10-13

Owner added local-to-fleet user/device management, E2E sync, multiple pairing
methods, family/multi-user isolation, raw disk access without formatting and
broad filesystem interoperability. Ext4 is now owner-approved as the provisional
starting OS filesystem default, still unimplemented and not production-qualified.

Added IDENTITY.md and expanded STORAGE.md. Updated DESIGN, ENCRYPTION, INIT,
SECURITY, ROADMAP and README links/contracts. No kernel behavior changed; next
bounded implementation task remains M3a. Management authority does not imply a
personal-data key grant, but privileged endpoint control remains a trust limit.
Persistent file-like raw-disk views require metadata in RAM, a sidecar or an
explicitly initialized region; never silently format or write unknown media.

New open decisions: principal/session ABI, recovery and PQ group protocol,
membership consistency, enrollment/federation profiles, fleet policy precedence,
raw block lease/alias tracking and sidecar format, exact supported filesystem
versions/features. Future agents must qualify each format's writes separately.
Validation: `ASAN_OPTIONS=detect_leaks=0 make check`, relative Markdown link
validation and `git diff --check` passed locally. No boot code changed; emulator
tests were not repeated. Remote checks must be observed separately on the PR.

## Explicit domain and Group Policy requirements: 2026-10-13

Added DOMAINS.md and links from README, IDENTITY, DESIGN and ROADMAP. Native
domain services and Microsoft AD DS interoperability are distinct targets.
The design covers join/leave/recovery, policy inheritance and precedence,
delegation, effective-policy reporting, offline expiry and compatibility gates.
No runtime implementation or outside directory modification occurred. Open ADRs:
policy composition/order, directory consistency, enrollment trust and exact AD/GPO
compatibility matrix. Next bounded kernel task remains M3a.

Local validation: `ASAN_OPTIONS=detect_leaks=0 make check`, Markdown relative-link
validation and `git diff --check` passed. Docs-only; no new boot tests required.
Remote CI status must be observed independently on the PR.

Owner clarification in the same work session: domain interoperability includes
macOS and Linux. Expanded DOMAINS.md with provider/endpoint directionality,
Samba AD and FreeIPA/LDAP/Kerberos evaluation, platform adapters and a tested
feature matrix requirement. No automatic cross-platform GPO compatibility claim.
Repeated local checks and documentation validation passed for this clarification.

## Apache-2.0 and accelerator/media requirements: 2026-10-13

Implemented documentation/license change: owner selected Apache-2.0. Added the
official LICENSE, original-source SPDX notices, updated ADR 0005, contributor
instructions and previously pending-license references, including STORAGE.md.
Added M3A.md (proposal only) and ACCELERATORS.md; linked README, DESIGN, ROADMAP,
HARDWARE and SECURITY. No executable behavior, codec, driver or SDK was added.

Planned: open-standards-first GPU/NPU APIs across qualified platforms; configurable
conservative offload policy; userspace FFmpeg, HandBrake and BRAW integration.
Blocked pending later prerequisites: native drivers/runtime, buffer/DMA isolation,
CPU extended-state support, vendor SDK feasibility and component license review.
Next bounded kernel gate remains M3a; see M3A.md for proposed pass/fail criteria.
Its API/layout/profile choices remain recommendations, not owner-approved ABI.

Validation observed locally: plain `make check` built successfully and passed core
tests, then LeakSanitizer failed because process/thread inspection is unavailable.
`ASAN_OPTIONS=detect_leaks=0 make check` passed core, boot-parser mutation corpus,
allocator and ELF checks; address/undefined-behavior checks remained enabled.
Relative Markdown links and `git diff --check` passed. Source changes are license
comments only; emulator tests were not rerun locally. New remote CI is separate
from this local evidence and must be checked on the published head.

## Consolidated coding-agent handoff: 2026-10-13

Added AGENT_START.md with the complete requirements map, issues #2–#9, dependency
order, proposed-versus-decided distinctions and sustained checkpoint rules.
Updated AGENTS, README, ROADMAP, DEVELOPMENT and IMPLEMENTATION entry points.
No runtime changes or new subsystem implementations. M3a remains next.

Observed remote evidence: Apache/design commit
`0e46993e480c075f2396015174d69d59dc6e7b37` passed PR CI run
https://github.com/auto44e/jeios/actions/runs/38390326429 .
This is prior-head evidence, not a claim about the subsequent docs-only head.

Local verification for this handoff: `ASAN_OPTIONS=detect_leaks=0 make check`,
relative Markdown links and `git diff --check` passed. No boot behavior changed,
so emulator tests were not repeated locally. Verify new remote CI separately.
Project-ready issues exist; board membership requires a separate verified action.

## Multitouch and hardware MFA requirements: 2026-10-13

PR #1 is merged; this documentation branch starts from main merge commit
`03aa2f380d8e49262abd1720a3b8db37517cbb31`. Added INPUT.md and SECURITY_KEYS.md;
updated README, DESIGN, IDENTITY, ROADMAP and the AGENT_START requirements map.
Owner requires OS-wide touchscreen/trackpad multitouch and built-in hardware MFA
keys. No runtime behavior changed; M3a remains next. Open ADRs include input event
ABI/gesture arbitration, authenticator broker/RP identity, recovery and numeric
limits. Do not claim pointer fallback is native app multitouch or FIDO is PQ.

Validation: `ASAN_OPTIONS=detect_leaks=0 make check`, Markdown relative links and
`git diff --check` passed locally. Docs-only; no boot tests repeated. New PR CI
must be observed separately. No hardware input/key tests have run.

## Fresh-kernel mobile repository: 2026-10-13

Owner superseded the initial AOSP/Linux host proposal: mobile uses the fresh C
kernel, AOSP is a reference/future app compatibility layer across the ecosystem.
Created auto44e/jeios-mobile via authorized browser flow and populated native
ARM64/Pixel/Android/parity docs, core pin, target registry, validator and tests.
Removed the superseded seed from this branch except a relocation pointer.
No AOSP/kernel/vendor import, ARM64 boot, Pixel image or Android execution.

Shared core code remains here; mobile MP0 proposes exact ARM64 contracts before
MP1 code. QEMU virt first; Pixel 9/10 are independent research candidates. Core
M3a remains next. Mobile issues #1-#5 track contracts, execution, Pixel, Android
and parity. Validation: core make check (ASAN_OPTIONS=detect_leaks=0), relative
links and git diff --check; mobile manifest/negative tests separately passed.
No device action occurred. Project membership is verified separately in the UI.

## PR #11 conflict resolution: 2026-10-13

Merged main `5f290b351181d8fa23b17ac5f493ff66ae9415b8` into the mobile
planning branch after PR #10 landed. Resolved five overlapping documentation
additions by retaining both multitouch/hardware-MFA and fresh-kernel mobile
requirements; DESIGN sections 26 and 27 remain distinct. Updated stale foundation
branch guidance. No kernel, build, test or security invariant changed.

Observed local validation: `ASAN_OPTIONS=detect_leaks=0 make check` passed core,
boot-parser, allocator and ELF checks; Markdown relative links, conflict-marker
scan and `git diff --check` passed. Leak inspection remains disabled for the
managed environment; address/undefined-behavior sanitizers remain enabled.
Mobile main `55505743500809b5714a725cb23e155a569791b1` passed
`python3 tools/validate.py` and all four `unittest` tests. Its immutable core pin
remains the tested M1/M2 foundation; documentation merges do not require moving
that implementation pin. Boot tests were not repeated locally for docs-only
changes. Check remote CI on the new PR head before merging. M3a and mobile MP0
remain the next bounded tasks; no ARM64/Pixel/Android execution is implemented.

## Historical handoff header — 2026-10-13 (superseded)

Updated: 2026-10-13. Project `auto44e/jeios`; current documentation branch
`docs/mobile-ecosystem` (PR #11). M1/M2 foundation PR #1 is merged into main.
Read AGENTS.md, IMPLEMENTATION.md, ROADMAP.md and the relevant DESIGN.md sections.
Start with [AGENT_START.md](AGENT_START.md) for the consolidated requirements and
linked work queue. Multitouch/security-key PR #10 is merged into main at
`5f290b3`; this branch incorporates that merge and preserves both feature plans.
The previous M0 state is preserved in git history; this file describes the M1/M2
implementation and subsequent documentation checkpoints. Do not infer future capabilities from design prose.



## 2026-11-06 — repair-first fix-gate CLOSED (F01–F14)

State: implemented/tested (AI-assisted, not independent human qualification).
The current-code stabilization gate (`docs/audit/2026-11-fix-gate.md`) is closed.
Dispositions and evidence are in the [closure ledger](audit/2026-11-fix-gate-closure.md);
per-batch evidence under `docs/audit/evidence/2026-11-F0*.md`.

- F01/F02 (PR #94): trustworthy mutation runner + builds.
- F03 (PR #97): reaper sign fix + `je_arch_irqs_enabled` seam.
- F04–F06 (PR #98): ARM audit two-pass deref safety; usercopy guards; F06
  defensive subset. The 4 deferred F06 contract decisions are owner-dispositioned
  in this PR (vb_flush: assert PCID/PGE-off at boot, keep no-op; zeroization:
  keep distributed policy, documented; boot-block: don't tighten, documented;
  no-NX console line: reworded unambiguously).
- F07–F11 (PR #99): JE_EP_CORRUPT split (owner-approved; a cross-review-caught
  user-reachable CORRUPT re-routed to DENIED); explicit one-thread-per-process
  (owner-approved); vm-audit/device/image negative coverage; recv staging clamp;
  ARM late-canary + stalled-timer proofs.
- F12–F14 (this PR): warning sweep 49→0 (core); SPDX/comment/FDT/tick-global
  cleanup; spawn decoupled from the failable harness ledger via a registered
  no-op-default observer; **ADR 0014** records the pre-MP3 lifecycle-ownership
  decision + migration trigger (owner-gated, deferred — a pre-MP3 blocker) and
  amends ADR 0013's trigger-less flex; the closure ledger.

Verified at the final head: x86 `make check` + `make integration`; ARM
`make aarch64` + 11/11 smoke matrix (+ F11 injected profiles FAIL as required);
`make mutants` 27 killed / 1 equivalent / 0 survived / 0 blocked / 1 skipped;
`tools/mutants.py --selftest` OK; NX-present and no-NX smoke both pass.

Next bounded task: with the gate closed, resume the roadmap — MP2 ARM scheduler
implementation per ADR 0013 — but the **ADR 0014 pre-MP3 lifecycle extraction is
an owner-gated blocker for MP3** (ARM userspace/IPC). Tracked non-blocker
follow-ups: `GAP-EP-WAIT-INFLIGHT` killer, x86 trap-routing hosted seam, the
`(uint8_t)hcap` recv truncation, the recv shared-helper refactor, F13's main.c
fixture split, device-observation-state split. Mobile pin unchanged.
