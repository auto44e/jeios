# Current-code stabilization gate: response to PRs #89–#92

Observed 2026-11-06T02:10:28-05:00. Core `main` inspected at
`e4bd9c24d69de4fc6798d733b1f2e6398522748f`; mobile `main` at
`9221701ab2d5333978a294f54574ee384f4216e3`.

Owner direction: review the latest codebase-improvement merges, publish a response
for working agents, and fix the existing implementation before adding new ones.
This is the response and bounded acceptance plan, **not a claim that fixes landed**.
Single-agent source review plus focused hosted probes; not an exhaustive proof of
correctness, not a rerun of the peers' 28-mutant experiment, and not independent
human release qualification.

## Decision and execution order

**Stabilize the existing code before MP2 scheduler implementation, MP3, M5a/SH1,
new ports, or other new functionality.** Existing branches may be preserved;
regression tests, narrow test seams, repair work and required contract clarification
are in scope. Do not combine fixes with feature expansion or a wholesale rewrite.

Read this plan with [the reconciled audit](2026-11-code-audit.md),
[raw peer reports](code-review/README.md), and accepted
[ADR 0013](../adr/0013-mp2-aarch64-scheduling.md).
The owner's request establishes the repair-first sequence. Specific changes to
frozen behavior still need a concrete proposal and owner review; this plan does
not silently amend ADRs or approve arbitrary refactors. A response-PR merge does
not mark any repair below complete. Do not repeatedly ask to investigate, write
tests, or prepare the already requested fix PRs.

Implement separate reviewable batches in this order:

1. **F01–F02: trustworthy tests/builds.** Establish reliable baseline, timeout,
   isolation and rebuild behavior before relying on mutation results.
2. **F03–F06: safety defects and memory-lifecycle contracts.** Include reproducer,
   repair and targeted regression evidence together.
3. **F07–F11: close current security-boundary and native execution evidence gaps.**
4. **F12–F14: bounded interface/ownership cleanup and remaining dispositions.**
5. One final closure ledger at an exact revision; then explicitly resume the
   roadmap. Related trackers: core #18/#19/#87, mobile #2.

All numbered rows are required closure work: implement the specified correction,
or record a source-backed no-change disposition accepted in review. No silent
skipping, no passing percentage threshold, and no requirement to implement future
SMP/PCID/platform features just to close a current limitation.

## Findings that must be corrected before agents copy them

| Prior claim | Final disposition against this main |
| --- | --- |
| Audit A2 / Codex A3: ARM table cycles recurse forever | **Incorrect mechanism.** Both walkers recurse only while `level < 3`, incrementing level each call. A hosted root self-cycle returned `JE_ERR_INVALID`, exit 0 within a 10-second timeout. Keep graph-validation/work-budget tests, but do not present infinite recursion as demonstrated. |
| ARM audit is safe because `audit_table` checks table membership | **Separate confirmed defect:** `audit_alias_scan` dereferences child table addresses without that check. On an otherwise valid fixture, setting `root[511] = 0x30000000 | 3` lets a valid earlier leaf trigger an alias scan into the unvalidated sibling: ASan SEGV at `paging.c:456`, exit 1. Fix validation order and every dereference, not merely cycle tracking. |
| C2: a parked waiter's record can be freed under it | The peer removed the existing `inflight_refs++` and the test survived. This is a **test gap under mutation**, not proof that the unmodified current code has that UAF. Retain/refine the implementation and add the killer. |
| Raw Codex B10: add BSS alignment linker assertions | Main already has start/end alignment assertions and layout constraints in `aarch64/linker.ld`. Check ordering explicitly if needed; do not duplicate existing assertions or claim they are absent. |
| Earlier response: ADR 0013 absent / mutation automation not implemented | Historical at #88; **superseded** by #91 and #89. ADR is merged, scheduler implementation is still absent. Six enabled mutant anchors and four disabled gap markers exist; placeholders are not executable killers. |
| #92: pinned base guarantees a pinned/reproducible toolchain | Base digest is pinned, but `apt-get update/install` uses unversioned packages from moving repositories. The QEMU check constrains only major/minor. Record image/package digests and fix the reproducibility claim; cached-image parity is not clean-rebuild reproducibility. |
| Hosted tests prove CPU barriers/cache/TLB semantics | They do not. Existing QEMU MMU/vector/IRQ tests already cover parts of this; extend named gaps. TCG is not physical cache/barrier qualification. |

## Repair work packages and acceptance criteria

### F01 — mutation runner must not manufacture green evidence

Files: `tools/mutants.py`, `tools/mutants.json`, assurance CI.
Current code has no unmutated test preflight, no subprocess timeout, edits the
caller's sources/build targets, accepts any nonzero runtime exit as a kill,
allows `build_fail_is_kill`, and permits unjustified equivalent/empty selections.
A focused temporary fixture with a test that always exits 1 was reported KILLED.
`finally` is useful but does not guarantee restoration after SIGTERM/SIGKILL or
protect concurrent editors. The four skipped entries are not coverage.

Required: run each killer unmutated in the **same configuration**, require PASS,
then apply its mutation in an isolated snapshot/build directory. Validate schema,
unique IDs, contained source/target paths, nonempty selection, nonempty equivalence
rationale, exact-match transforms and expected failure evidence. Remove the
compile-error-as-runtime-kill escape hatch (separate compile-time contracts if
needed). Bound build/run time and terminate child process groups. Baseline failure,
spawn error, missing executable, unexpected signal and generic infrastructure
failure are BLOCKED; a timeout only kills an explicitly declared liveness mutant
with a passing bounded baseline. Preserve useful logs and machine-readable results.
Never rewrite another contributor's worktree; cancellation must leave it unchanged.

Acceptance: tool self-tests cover baseline failure, expected assertion kill,
survivor, compile failure, timeout, unknown/duplicate ID, source drift, malformed
manifest, unjustified equivalent, zero enabled tests, signal cleanup and two
concurrent invocations. Restore/isolation hashes match. All six existing anchors
still produce genuine kills. Replace placeholders only when actual killers exist;
correct the misleading ARM cycle entry and add the foreign-sibling regression.

### F02 — ordinary builds and container execution must be trustworthy

Files: `Makefile`, hosted tests, `.docker/*`, `.github/workflows/ci.yml`.
Fix the missing `kernel/core/sched.c` prerequisite of `test_sched_killall`; deleting
its binary in the mutant runner does not repair ordinary incremental builds.
Use compiler-generated dependencies or an equivalent complete dependency contract
for indirectly included sources and headers. Ensure assert-based tests cannot
silently become vacuous with `NDEBUG` (some already `#undef NDEBUG`; inspect each).

Pin a verifiable builder artifact by digest, or pin package versions against an
immutable package source with an explicit update process. Record complete package
versions, image identity, host architecture and source SHA. Correct README/workflow
claims until clean rebuild identity is demonstrated. A shared RW `/src/build`
can reuse host or previous-image binaries: use clean isolated outputs keyed by
source/toolchain/configuration. Container ephemerality alone is not build isolation.
Validate soak count as a bounded positive integer before building; zero iterations
must not report success. Signal handling must exit after cleanup, not continue a
soak loop. Preserve task exit status and failed-build logs; use `-i/-t` deliberately
for the documented interactive shell, without changing noninteractive CI behavior.
Do not claim unconditional cleanup after host death/SIGKILL. Keep leak detection
exceptions explicit; include a supported leak-enabled lane rather than treating
`detect_leaks=0` as equivalent coverage.

Acceptance: touching included `sched.c` really recompiles its hosted test;
NDEBUG cannot yield a vacuous PASS; invalid/zero soak counts reject; failed task,
interrupt and repeated tasks return correct status and leave no normal-path
containers. Poisoned/stale host build outputs cannot make a container test pass.
Run native baseline and container checks with exact tool/image metadata.

### F03 — reaper sign and scheduler failure/lifetime tests

Files: `kernel/core/sched.c`, `include/jeios/sched.h`, arch IRQ seam and tests.
The positive error returns in `je_sched_reap_dead` violate its documented negative
error/count contract; callers check `< 0`. Prefer the narrow negative-return fix
with a complete caller audit over an unrelated API redesign. Add an arch-owned
IRQ-state query or equivalent hosted seam instead of trying to toggle privileged
IF from a hosted process.

Acceptance: not-ready/wrong-context, active CR3, active RSP0, drop failure and
successful 0/N reap paths are tested; idle treats failures as fatal. Verify no
wrong frame/stack is freed and no slot is reported reaped after failure. Catch
expected fail-stop in hosted tests without hanging, configurable IF, and exact
victim IDs recorded by the waiter-release fake. Cover single kill statuses,
kill-all identity vs supervisor flag, link/state mismatches, null frames, teardown
hooks and ordering. Retain existing AMD64 M3/M4 integration proofs.

### F04 — ARM audit dereferences, graph shape and bounded work

Files: `kernel/arch/aarch64/paging.c`, `paging.h`, `tests/test_paging_bits.c`.
Validate context/table count, root membership/alignment and **every child table
address before dereference**, including alias traversal. Prefer a complete bounded
structural pass followed by permission/alias checks over scanning unvalidated
siblings from a leaf callback. Validate unique/allowed table topology with a
bounded visitation structure or bounded explicit work list. Record a conservative
work bound; four levels alone do not prevent combinatorial repeated traversal.
Preserve independent descriptor literals, identity mapping, W^X and audit/enable
gating. Do not replace the independent oracle with builder-generated expectations.

Acceptance: foreign root/late sibling, self-cycle, ancestor cycle, repeated table,
invalid count, duplicate ledger entry and malformed descriptor reject without
invalid reads or hangs. The reproduced foreign-sibling case must return INVALID,
not crash. Failed audits revoke enable eligibility. Baseline hosted and all existing
ARM boot/MMU/IRQ profiles still pass; maintain named graph/tamper mutations.

### F05 — usercopy defensive contract and direct tests

Files: `kernel/core/usercopy.c`, copy header, syscall tests.
Preserve zero-length success without touching either pointer. Reject NULL kernel
buffer for nonzero length before assembly; it is an internal-call misuse, not a
demonstrated ring-3 exploit. Enforce/document nonnesting and IRQ/owner preconditions
without introducing an unimplemented SMP promise. A rejected nested call must not
clear the outer valid scope. Preserve exact fault-PC, PFEC, direction and owner
matching; do not broaden recoverable fault handling.

Acceptance: both copy directions, null kernel pointers, zero length, arena bounds,
wrap, page crossing, RX/RW/absent pages; positive and negative fixup cases for
exact/nearby RIP, CR2 endpoints, PFEC and wrong owner. Scope cleared on completion
and recovered fault. Keep real assembly/fault QEMU evidence separate from stubs.

### F06 — VM invalidation, zeroization, boot context and NX contracts

Files: `kernel/core/boot.c`, `alloc.c`, `vm.c`, x86 `paging.c`/`vm_backend.c`,
ARM `physmem.c`, applicable headers/contracts.

- Resolve `vb_flush`'s no-op against clear-PTE/flush/free ordering. Prove and enforce
  inactive-root/no-PCID/single-CPU prerequisites or implement a correctly scoped
  invalidation operation. `invlpg` on the wrong active root is not a fix. Do not
  add PCID/SMP in this repair. Test teardown/root/frame reuse in real QEMU.
- Audit every existing cross-owner frame path and mechanically enforce zero-before-
  exposure; choose a narrow ownership policy with review. Generic page-free not
  scrubbing is not itself proof of a current leak. Test user/page-table/heap reuse
  and error rollback; inspect scrub-before-release where that is the contract.
- Validate `ctx` counts/ranges before x86 paging indexes them, checked range ends,
  ARM contains/intersects overflow parity, and reservation result handling.
- Define readable/reservable boot metadata and module eligibility for the **actual
  loader profile** before tightening the parser. Do not blindly require a single
  type-1 entry if loader-owned reserved RAM is permitted by the contract. Retain
  bootstrap accessibility preconditions: a parser cannot retrospectively make its
  first read safe. Test below/at/above physical limits, holes, overflow, reserved
  ranges, adjacent ranges and reservation exhaustion; failure leaves output clear.
- Preserve legacy no-NX kernel boot where explicitly supported, but make degraded
  diagnostics unambiguous and refuse user-space mappings when NX is unavailable.
  Add backend-local validation where its contract permits direct invocation.
- Tie page-guard restore to a recorded guard owner/state or restrict the seam with
  a checked invariant. A zero PTE alone is not proof it was a scheduler guard.

Acceptance: profile boundary and negative tests plus existing low/large-memory,
no-NX, isolation, scrub and teardown integration scenarios. Contract changes need
explicit owner disposition; passing the hosted flush counter is insufficient.

### F07 — endpoint corruption must not become ordinary unsupported status

Files: `endpoint.c`, `endpoint_kern.c`, endpoint header, `syscall.c`.
Separate expected unwired configuration from internal corruption. Audit every
producer and mapper of `JE_EP_CORRUPT`, not only channel creation. Route a proven
internal dangling-record invariant to the established fail-stop path. Malformed
user handles retain defined BADINDEX/STALE/WRONGTYPE/DENIED behavior and must not
become a user-triggerable panic. Preserve ABI status precedence.

Acceptance: unwired configuration, stale public handle and corrupt internal record
are three separate cases, including through syscall glue. All expected ordinary
errors are unchanged; corruption cannot be disguised as UNSUPPORTED.

### F08 — waiter identity and real endpoint/capability entry points

Files: `sched.c`, `endpoint.c`, `endpoint_kern.c`, `syscall.c`.
Add SEND-only denial tests through `entry_probe`, `recv_peek_caps`, and
`wait_register`; test reserved-head exclusion, reference overflow and transfer
attenuation through production glue. Close both handles while a waiter holds its
inflight reference: record stays alive until release, then is reclaimed exactly
once. Check timeout/send/close/cancel arbitration and process-cap release.

Strengthen parked waiter identity (record/side and lifetime generation as needed)
and detect duplicate stale registrations before slot reuse. Reject zero-deadline
park misuse; validate record/side/frame before dereferencing. Preserve the current
single-CPU atomicity model. Make one-thread-per-process validation explicit rather
than relying on guard-page failure; define kernel-only sleep behavior explicitly.
Clarify/remove the ignored `owner` parameter in release-table APIs with call-site
review, not a misleading name change alone.

Acceptance: exact waiter/slot association, stale wake after slot reuse, duplicate
registrations, zero deadline and preempted teardown reject without resurrecting a
victim. Test current release-before-reuse behavior first. Enable waiter-inflight
and rights/attenuation/reservation mutants once they have real killers.

### F09 — VM audit and device authority negative coverage

Add independent `vm_fake` corruption hooks and one negative test for alias,
permission, one-to-one, guard, completeness and cross-process sharing rejection;
assert failed spawn publishes nothing and rolls back resources. Test the actual
`je_device_apply_dispatch` owner identity, nonowner, inactive/revoked device and
closed driver handle, ack identity, death revocation, assignment quota and invalid
caller. Assert no TSS/port authority remains after revoke/context switch.

Acceptance: each named broken check is killed by its relevant test; positive owner
operations still succeed. Enable the VM and device gap entries; one representative
mutant is not evidence for all rejection classes. Add current x86 IRQ3/IRQ7/timer/
yield dispatch coverage where missing; a GIC fake does not prove x86 trap routing.

### F10 — syscall glue, shared receive delivery and image registry

Create hosted dispatch seams with explicit no-hardware claims. Cover hidden-rights
masking and success exactly at message/cap maxima. Clamp the receive staging
capacity to its actual storage in both receive paths; current safety relies on
queue-size invariants, so classify this as defensive strengthening, not a proven
overflow. Characterize mint/copy/rollback/commit behavior before extracting one
shared delivery helper. Centralize equivalent RECV resolution only after locking
error precedence, reservation semantics and rollback in tests.

Use checked integer-address arithmetic for linker image spans; verify conversion
width and initialization rollback. Test built-in registry partial failure and
length mismatch. Keep descriptor validation distinct from running every existing
built-in image: map each image to its current QEMU marker/service-protocol test,
and add missing execution checks. Test `je_vm_image_validate`, not just registry
bytes, with malformed descriptor cases and an independent page-use model.

### F11 — ARM native proof gaps, not invented hardware coverage

Files: ARM `vectors.S`/`main.c`/`gic.c`/`paging_enable.S`, smoke oracles.
Audit current primary/high-register canary phases, including final-tick return and
scratch windows. Catch deliberately corrupted low/high registers late in the
interval; bound stalled-timer behavior with an explicit failure marker and external
timeout. Preserve frozen frame ABI and accepted ADR 0012/0013 mechanics.

Document GIC init failure as terminal or implement bounded quiescence if retry is
supported; test readback failure and reset semantics for the chosen contract.
Bound/document table-cache maintenance across noncontiguous allocations and test
the intended ranges. Retain existing linker alignment assertions; add missing
ordering checks only if not already implied by the layout.

Acceptance: full existing ARM CI matrix plus negative canary/vector/timer oracles.
Use disassembly checks as structural evidence, QEMU as execution evidence; neither
proves physical cache coherence. Physical-board qualification stays an explicit
later gate, not a fabricated prerequisite result for this hosted cleanup.

### F12 — declarations, diagnostics and precise contracts

Move cross-translation-unit declarations to owned headers included by definitions
and callers. Run `-Wmissing-prototypes -Wnested-externs -Wredundant-decls` with
reviewed freestanding/assembly exceptions; do not blanket-disable warnings.
Correct endpoint/scheduler stale comments, reconcile type-zero handle policy with
the existing type-agnostic table tests and dispatcher validation, and add SPDX
headers to original spawn files. Remove redundant FDT output zeroing only while
retaining the failure-zero contract. Encapsulate writable tick globals with an
explicit assembly access strategy; volatile is not synchronization. Inventory and
separate test-observation device state where safe, preserving production diagnostic
needs. Remove truly dead parameters/computations after tracing all callers.

Acceptance: warning sweep evidence, public declarations agree, no accidental ABI
change; raw historical audit/debate transcripts remain verbatim. Correct current
summary/manifest claims with dated addenda rather than rewriting source history.

### F13 — core/test seams and lifecycle ownership

Decouple `spawn.c` from the harness-defined image ledger; retain required registry
state in an owned production module and make optional observation explicit.
Move/split the x86-specific test harness into appropriately owned fixture modules
in a mechanical, evidence-preserving PR after defect fixes. Keep failure/marker
behavior and all `JE_TEST_MODE` profiles reproducible.

Prepare a bounded lifecycle-only ownership/seam change for process registration,
dying/reaping, VM and kstack lifetime, and arch-dependent operations embedded in
core scheduler/device code. Preserve the current x86 run queue. **Do not implement
or migrate to `sched_rr.c` as cleanup**; accepted ADR 0013's ARM scheduler is future
work after this gate. Record the pre-MP3 lifecycle decision and explicit migration
trigger in the proper reviewed contract. The decision/proposal is required now;
if implementation is deferred, obtain explicit owner disposition and retain it as
a pre-MP3 blocker. This prevents an unbounded refactor from masquerading as a bug fix.

### F14 — complete the assurance/disposition ledger

Add bounded deterministic corpora for FDT, boot, VM images, usercopy and Python
ELF packaging where gaps remain; retain existing FDT mutation/truncation coverage.
Use exact-size buffers, semantic negative cases and independent reference models.
Record branch/return-path coverage as a diagnostic, never “100% means secure.”
Run available compiler analysis and disposition findings. Coverage-guided fuzzing,
additional Clang/MSan/cppcheck lanes, future hosted concurrency/TSan, broader SMP
work, and new hardware lanes are separately tracked enhancements unless a concrete
current defect requires one. Do not make installing every proposed tool an
indefinite closure condition.

## Closure ledger required from working agents

Start every F row as OPEN. For each submitted PR record: F ID; source symbol and
baseline SHA; reproduced vs inferred failure; before/after results; named mutant;
local, CI, QEMU and hardware evidence separately; exact toolchain/configuration;
contract/owner decisions; residual limits; final implementation SHA and review.
A no-change disposition needs rationale and evidence, not “tests pass.”

Exit requires F01–F14 dispositions accepted, confirmed defects repaired, critical
security-boundary killers enabled, no unexplained baseline failures/survivors,
clean native and container builds, full current AMD64/ARM CI at the final SHA,
and synchronized STATUS/HANDOFF/issue evidence. An explicit owner waiver may
scope a residual item; agents cannot grant their own waiver. User-visible features
remain gated; mobile pin stays unchanged until separately qualified integration.

## Verification performed for this response

- Read merged #89 audit, #90 raw reports, #91 accepted ADR and #92 tooling against
  main; inspected cited implementation/test/workflow paths. No production edits.
- Main CI [42177874117](https://github.com/auto44e/jeios/actions/runs/42177874117)
  observed successful at `e4bd9c2`; this is baseline evidence, not this PR's CI.
- `make check`: blocked by LeakSanitizer process inspection in this environment.
  `ASAN_OPTIONS=detect_leaks=0 make check`: PASS, exit 0; ASan/UBSan retained,
  leak detection not validated.
- Focused hosted probes: root self-cycle rejected (exit 0); foreign late sibling
  caused ASan SEGV in `audit_alias_scan` (exit 1); always-failing temporary killer
  falsely classified KILLED. [Reproduction details](evidence/2026-11-fix-gate.md).
- Mutation baseline result is recorded in HANDOFF. No Docker/QEMU executables
  available locally; no container build/native boot/hardware rerun claimed.
- This is a scoped final reconciliation of the reported findings and new tooling,
  not a certification that all possible defects have been found.
