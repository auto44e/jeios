# JeiOS code-improvability audit and curl-style local assurance

Observed 2026-11-06, repository `auto44e/jeios`, branch `main`, inspected commit
`c6914ca9176b360ded82ed70c0884fafddbaf0e6` (the merge of audit PR #86 and docs
response PR #88). This document is the second audit pass the planning agent asked
for after merging the first-pass response: a review of *all code written so far*
for improvability, plus the "curl-style" local CI/testing it also requested,
applied concretely as a committed critical-mutant harness.

**Nature of this review (honesty gate, per AGENTS.md).** This is an AI-assisted
review produced by a lead agent orchestrating two external coding agents at
maximum effort on complementary slices of the tree, with every load-bearing
finding re-verified in-tree by the lead before it was written here. It is **not**
independent human qualification and does not satisfy the critical-profile human
gate. It changes **no** production code, no frozen contract, and no
implemented/tested/planned/blocked status. The only code it adds is assurance
tooling (`tools/mutants.py`, `tools/mutants.json`) and its wiring (a `mutants`
Make target and a CI job); the kernel source is untouched. Findings that would
touch tested code or a frozen contract are marked **owner-gated** and are
recommendations for the owner to schedule, not changes made here.

## How this pass was run

Two peers audited disjoint slices so no single author's blind spot covered both a
behavior and its check:

- **Untrusted-input / architecture-safety slice** (Codex, reasoning effort
  xhigh): `fdt.c`, `image.c`, `boot.c`, `usercopy.c`, `alloc.c`, `handle.c`, the
  AArch64 `paging.c`/`gic.c`/`vectors.S`/`boot.S`, and the x86 `paging.c`/
  `vm_backend.c`/`trap.c`.
- **Logic-surface / test-quality slice** (Claude, model opus, effort high): the
  scheduler `sched.c`, `endpoint.c`/`endpoint_kern.c`, `syscall.c`, `vm.c`,
  `device.c`, `spawn.c`, `proc.c`, the x86 integration harness `main.c`, and all
  `tests/test_*.c`. This peer additionally ran its own 28-mutant probe on a
  throwaway `git archive` copy and reported 6 killed / 22 survived, which located
  the test-quality gaps precisely.

The lead then re-read the cited lines in the live tree and confirmed each
load-bearing claim before recording it below. Where a claim could not be
confirmed, it is omitted or flagged.

## Genuine strengths (verified in-tree)

These are not courtesy; they are the patterns the rest of the tree should copy.

- **FDT parser TOCTOU discipline** (`include/jeios/fdt.h`, `kernel/core/fdt.c`):
  copy-once, parse only the copy, then validate against architecture facts, with
  checked 64-bit layout arithmetic and bounded reservation scanning. `test_fdt`
  uses exact-size buffers, an exhaustive truncation sweep, an independent builder,
  and pre-poisoned output buffers — the correct parser-test shape.
- **Allocator bounds discipline** (`kernel/core/alloc.c`): address, window and
  alignment are validated *before* a page index is derived; `je_pages_init_pred`
  is default-deny. Both are now pinned as regression mutants (below).
- **VM journaled build ordering** (`kernel/core/vm.c`): zero → copy → remove the
  writable alias → demote permissions → publish the user PTE, with a full
  byte-for-byte rollback test at every failure step; `vm_fake` is an independent
  model that enforces the ADR 0007 D3 ordering itself.
- **Handle generation retirement** (`kernel/core/handle.c`): centralized
  precedence and saturating generation retirement that prevent stale-handle reuse;
  the reduced-width `test_handle_rw` build exhausts the generation space on
  purpose.
- **AArch64 page-table audit** (`kernel/arch/aarch64/paging.c`): independent exact
  descriptor literals, table-frame membership, alias scanning and W^X rejection,
  with named tamper mutants in `test_paging_bits` driven by an independent
  architecture oracle. (One residual recursion hazard is noted in A-tier below;
  it does not detract from the design.)
- **GIC model honesty** (`kernel/arch/aarch64/gic.c`, `tests/test_gic_bits.c`):
  IAR-once, spurious-gets-no-EOIR, modeled timer level behavior, *and explicit
  statements of what the hosted test does not prove* (no real vector entry, no
  Device-memory mapping). The `vm_fake`, `test_gic_bits` and `test_paging_bits`
  "what this does not prove" comments are the honesty standard the whole tree
  should adopt.
- **Usercopy fixup precision** (`kernel/core/usercopy.c`): the recovery hook
  requires the exact faulting RIP, matching direction/PFEC, a CR2 inside the
  declared span, and the owning VM — not a broad "any #PF in a copy" recovery.

## Findings, prioritized

Severity A = a correctness/safety defect with a plausible trigger. B =
robustness/clarity/consistency. C = a test that looks like it proves a property
but does not (with the exposing mutant). "Owner-gated" marks anything touching
tested code or a frozen contract.

### A — correctness / safety

- **A1. `je_sched_reap_dead` reports errors with the wrong sign.** *(verified;
  owner-gated, M3a tested scheduler.)* `kernel/core/sched.c:585` returns `int`;
  the contract (`sched.h`) and the caller `je_kernel_idle_step`
  (`sched.c:616`, `if (je_sched_reap_dead() < 0) abort_scheduler();`) expect a
  **negative** `je_status` on an invariant violation. But the error returns are
  `(int)JE_ERR_BUSY` and `(int)s`, and every `je_status` error is **positive**
  (1–7, `include/jeios/status.h`). So the `< 0` guard is dead: a still-live CR3/
  RSP0, or a `je_sched_drop_user` failure, is silently read as "N threads reaped"
  and the DYING slot is retried forever instead of halting. Fix: `return -(int)s`
  or change the signature to `je_status(uint32_t *reaped)`. Not hosted-testable
  as-is because of the `pushfq` IF check at `:591`; add a `je_arch_irqs_enabled()`
  seam so a hosted test can exercise the error path.
- **A2. AArch64 page-table audit can recurse forever on a table cycle.**
  *(verified; owner-gated, ARM paging tested path.)* `audit_table` and
  `audit_alias_scan` (`kernel/arch/aarch64/paging.c:450`, `:482`) follow table
  descriptors recursively. `table_frame()` membership is required, and a
  `table_leaves == table_count` tie-off accounts for leaves — but neither bounds
  *recursion depth*. A tampered table whose descriptor points back at an ancestor
  frame still passes `table_frame()` (it *is* a builder frame) and recurses until
  stack exhaustion, defeating the audit's fail-closed intent. Fix: a
  `JE_ARM_MAX_TABLES` visited bitmap (or an explicit bounded work stack) plus a
  hosted self-cycle test under a timeout. Tracked as the disabled gap mutant
  `GAP-ARM-AUDIT-CYCLE`.
- **A3. Nonzero usercopy with a NULL kernel buffer faults the kernel.**
  *(verified; owner-gated, M3/M4 tested usercopy.)* `copy_core`
  (`kernel/core/usercopy.c:99`) special-cases `len==0` and validates the *user*
  span/pages, but never rejects `kptr == NULL` with `len != 0`; the approved
  assembly then dereferences kernel address 0. Note the trigger is a
  kernel-internal caller, not ring 3 (user controls `uaddr`, not `kptr`), so this
  is a defensive-boundary hardening rather than a user-reachable exploit. Fix:
  `if (kptr == NULL) return JE_ABI_INVALID;` after the `len==0` check, plus direct
  `copy_core` NULL tests.
- **A4. The x86 VM flush contract is a no-op in the real backend.** *(verified;
  owner-gated.)* `vm.c`'s transaction states "clear PTE → flush → restore", but
  `vb_flush` (`kernel/arch/x86_64/vm_backend.c`) is `/* single BSP, CR3 reloads
  */` and does nothing. This is currently masked by the no-PCID, inactive-root
  lifecycle (a later CR3 load flushes), but the backend does not implement the
  abstraction's stated immediate-invalidation contract and would be unsafe under
  same-root reuse or PCID. Fix: make invalidation explicit (pass the affected
  root/range, or require a documented CR3-reload-before-reuse precondition and a
  real QEMU stale-PTE/reuse test). The hosted fake's flush counter proves nothing
  about hardware invalidation (see C: `VM-TLB-FLUSH`).
- **A5. Zeroization-before-reuse is distributed across callers, not enforced at
  the allocator boundary.** *(verified; owner-gated.)* `je_pages_free` returns raw
  frame contents; VM-owned frames are scrubbed by the x86 backend and heap
  allocations are zeroed on allocation, but a future consumer could map a freed
  kernel page before zeroing. Fix: define one explicit ownership policy (scrub on
  release, or a mandatory zero-at-map API, or release classes with a scrub
  callback) and a cross-owner reuse test.
- **A6. Multiboot source metadata is not proven to lie in RAM, and module ranges
  aren't profile-checked.** *(verified.)* `kernel/core/boot.c` reserves the boot
  block and module ranges but never verifies the boot block is inside a type-1
  RAM entry, nor that module `end <= JE_PHYSICAL_LIMIT`. The parser's
  "loader data reserved" claim can therefore cover MMIO/reserved space. Fix: a
  checked "range fully inside one RAM entry" helper applied to the boot block, and
  reject/classify out-of-profile module ranges; tests for each.

### B — robustness / clarity / consistency

- **B1. The x86 integration harness lives inside `kernel/core/`.** *(verified.)*
  `kernel/core/main.c` is ~3.8k lines, `je_kernel_main` alone ~1972 lines with 29
  `JE_TEST_MODE` blocks, and core `spawn.c:259` calls `je_proc_record_image`
  *defined in that harness* (`main.c:371`), which can `fail()`. Core depending on
  the test harness is backwards. Fix: a no-op-in-core observer hook, then move the
  harness to `kernel/arch/x86_64/selftest/` split per fixture. Owner-gated (it
  moves tested code), but high-value for the neutral-scheduler work.
- **B2. `extern` declarations in `.c` files.** *(verified via warning sweep.)*
  `spawn.c`, `syscall.c`, `endpoint_kern.c` (inside a function body), `main.c`
  carry `extern` decls the compiler cannot check against the definitions, so a
  signature change is silent UB. Measured cost to fix: `-Wmissing-prototypes
  -Wnested-externs -Wredundant-decls` reports only ~20 warnings across core. Move
  them into headers and add the warnings.
- **B3. One status, two meanings — `JE_EP_CORRUPT`.** *(verified; owner-gated,
  M3c.)* It covers both "vm not wired" (an expected configuration) and "a handle
  names an unallocated record" (use-after-free evidence); both map to
  `JE_ABI_UNSUPPORTED`, handing the dangling-handle case back to ring 3 instead of
  halting. Fix: split UNWIRED vs CORRUPT and route CORRUPT to `syscall_invariant`.
- **B4. Duplicated security checks drift from their tested copy.** *(verified.)*
  The RECV right is checked in four endpoint functions (`endpoint.c:268/290/721/
  927`); the peer mutants show the *untested* copies are the ones the syscall path
  actually uses. The recv mint→copy→rollback→commit sequence is likewise
  duplicated between non-blocking and blocked-recv finish in `syscall.c`. Fix: a
  single `resolve_with_right()` and one `deliver_peeked()` helper. Owner-gated
  (M3c).
- **B5. The no-NX profile prints "protected mappings ready".** *(verified.)* When
  NX is absent, x86 paging init succeeds and maps data executable;
  `kernel/arch/x86_64/vm_backend.c` installs executable data leaves, and
  `main.c` prints a "protected mappings … ready" line (it does separately print
  that NX is absent). User VM creation correctly refuses without NX, but kernel
  data stays executable and the message can be read as a W^X claim. Fix: an
  explicit degraded-profile object consumed by diagnostics; say exactly which
  guarantee is absent. (Aligns with the three-claim-class discipline from the
  first-pass debate.)
- **B6. Stale contract text.** *(verified, representative sample.)* `endpoint.h`
  still says "NOT here: messages, transfer, waits" and "includes ONLY handle.h"
  while `endpoint.c` includes `abi.h`; `sched.h` says "no dynamic slot reuse"
  while slots are reused; a generation comment says the field is "not yet READ"
  though it is read now. Harmless individually, but they mislead the next agent.
  Fix: a documentation-only contract-text sweep.
- **B7. Smaller items.** ARM `range_contains_page` lacks the overflow guard its
  sibling `range_intersects_page` has (`kernel/arch/aarch64/physmem.c`); the FDT
  output is zeroed twice around arg validation (`fdt.c`); boot `reserve()` return
  values are discarded (`boot.c:31`); `image.c` uses linker-symbol pointer
  subtraction that should go through `uintptr_t`; `je_timer_ticks` is an exported
  writable `volatile` alongside its accessor; ~20 test-observation fields live in
  the production `je_device` struct.

### C — tests that prove less than they appear to

Each item names the mutant that survives the current suite. The ones with an
*existing* killer are now committed as enabled regression anchors; the ones
*without* are committed as disabled gap markers (see the harness section).

- **C1. `je_vm_audit` — the sole ring-3 spawn gate — has 6 unchecked rejection
  classes.** *(verified; `vm.c` audit vs `spawn.c:197`.)* The peer's six
  audit-mutants (alias, perm, one-to-one, guard, completeness, cross-process) all
  survive because the only negative tests are for stale upper-level entries. Fix:
  `vm_fake` poke hooks for a leaf's perm/phys/present/alias/extent and one
  negative test per class. Tracked as `GAP-VM-AUDIT-ONE-TO-ONE` (representative).
- **C2. A parked waiter's record can be freed under it.** *(verified via peer
  probe.)* Dropping the inflight ref in `wait_register` survives the suite; with
  both handles closed the record is freed while a thread sleeps on it and the
  timer sweep dereferences freed memory. Fix: a close-both-handles-while-parked
  killer test. Tracked as `GAP-EP-WAIT-INFLIGHT`.
- **C3. Ring-3 port-I/O grant is untested.** *(verified.)* `je_device_apply_
  dispatch` has no hosted test, so "grant to any active device" instead of "to the
  owner" survives, as do ack-caller identity, revoke-on-death, and the assign
  quota preflight. Tracked as `GAP-DEV-DISPATCH-OWNER`.
- **C4. Hosted fakes cannot prove the hardware transition.** *(verified by the
  tests' own comments.)* `test_paging_bits` replaces the MMU-enable leaf with a
  recorder, so removing `tlbi`/`dsb`/`isb`/`dc ivac`/`WXN` survives; `vm_fake`'s
  flush is a counter, so a no-op x86 invalidation survives (`VM-TLB-FLUSH`);
  `test_gic_bits` cannot prove vector entry. These are honestly disclosed, not
  hidden — the fix is to *add* QEMU fault/cache/vector-routing tests alongside the
  excellent hosted oracles, not to change them.
- **C5. RECV denial is tested on a path ring 3 never reaches.** *(verified.)*
  `test_endpoint` exercises RECV denial through `recv_peek`, while the syscall
  path uses `recv_peek_caps`/`wait_register`/`entry_probe`, which are uncovered.
  Fix: a SEND-only-handle denial case on each real entry point.
- **C6. `usercopy.c` and `je_syscall_dispatch` have no hosted test at all.**
  *(verified.)* The usercopy span validators and fixup predicate are pure given
  `vm_fake` plus a scheduler stub; the syscall glue is where rights masking and
  the staging-buffer clamp actually run.

### Two test-infrastructure traps found (and why the harness is built the way it is)

The logic-slice peer hit two traps while probing, both verified in-tree and both
shaping `tools/mutants.py`:

1. **Stale-binary false survivor.** The `test_sched_killall` rule lists
   `kernel/core/device.c` but not the `kernel/core/sched.c` it `#include`s via the
   `-DJE_SCHED_HOSTED_TEST` `#define static` trick (`Makefile`, verified:
   prerequisites `… proc.c device.c`, compile line `… proc.c` only). So a mutation
   of `sched.c` would not re-trigger a rebuild and would *falsely survive*. The
   runner defends against this generally by **deleting the target binary before
   every rebuild**, forcing a recompile regardless of Make's prerequisite view —
   verified to matter in practice, since several enabled mutants live in
   `#include`d/indirect TUs and still KILL.
2. **`-Werror` turns a deleted line into a fake kill.** Deleting a check can leave
   an unused variable, which fails the build under `-Werror`; a naive runner would
   read the build failure as a kill. The runner classifies a build failure as
   **BLOCKED**, never a kill (per the planning agent: "a compile failure is not
   automatically a mutant kill"), and every deleter mutant here is written as
   `if (0 && (<cond>))` so the variable stays used and the test — not the compiler
   — is what fails.

## The curl-style deliverable: a committed critical-mutant harness

The first-pass debate rejected a kill-percentage gate as gameable and noisy and
chose a **named critical-mutant manifest** instead; this pass implements it as the
local, curl-style assurance the planning agent asked for.

- `tools/mutants.py` — for each named mutant: apply a precise reversible source
  transform that breaks one invariant; rebuild (force-recompile) the one hosted
  killer test; assert the test now FAILS (KILLED). It distinguishes
  **KILLED / SURVIVED / BLOCKED / EQUIVALENT**, restores the source
  unconditionally (even on error/SIGINT), requires each `find` string to occur
  exactly once (a drift guard), and exits nonzero on any SURVIVED or BLOCKED.
  Production code carries no mutation macros.
- `tools/mutants.json` — the manifest. **Six enabled regression anchors**, each
  with a killer test that exists today and each verified to KILL at this commit:
  `ALLOC-FREE-OOB`, `ALLOC-DEFAULT-DENY`, `HANDLE-RIGHTS-REQUIRED`,
  `HANDLE-ATTENUATE-SUPERSET`, `VM-IMAGE-OVERLAP`, `FDT-MAGIC` — spanning the
  allocator, handle rights, the VM image validator, and the untrusted DTB parser.
  **Four disabled gap markers** record the highest-value *missing* killers found
  above (`GAP-VM-AUDIT-ONE-TO-ONE`, `GAP-EP-WAIT-INFLIGHT`, `GAP-DEV-DISPATCH-
  OWNER`, `GAP-ARM-AUDIT-CYCLE`) so the manifest documents the known coverage
  debt while staying green; enable each when its killer test lands.
- `make mutants` runs the manifest; a dedicated `mutants` CI job
  (`.github/workflows/ci.yml`, gcc/make/python3 only, no QEMU) runs it on every
  push and PR. It is intentionally *not* folded into `make check` because it
  rebuilds targets.

Evidence at this commit: `make mutants` → `6 killed, 0 equivalent, 0 SURVIVED,
0 BLOCKED, 4 skipped`, exit 0; `make check` still passes unchanged.

### Recommended next assurance additions (beyond this PR)

The toolchain present is gcc 13 / gcov / ASan / UBSan. Absent: clang, cppcheck,
scan-build/clang-tidy, libFuzzer (`-fsanitize=fuzzer` is rejected by gcc), lcov/
gcovr, valgrind. So the realistic near-term additions are:

1. **A deterministic-corpus parser harness under ASan/UBSan** for `fdt.c`,
   `boot.c`, and `je_vm_image_validate` (not `image.c`, which is only the
   built-in registry — the real validator is `vm.c`). Seed with the QEMU DTB,
   every named negative case, and truncation prefixes; parse twice and compare for
   determinism. A libFuzzer target can be added later if clang lands.
2. **QEMU tests for the properties the hosted fakes cannot prove**: an x86 stale-
   PTE/TLB reuse test, ARM barrier/`WXN`/cache-fault tests, vector-routing, and a
   full-duration ARM register canary.
3. **A branch-coverage "every rejection path is tested" report** via gcov, listing
   each `return JE_ERR_*`/`JE_EP_*` line no test reaches — coverage as a
   test-selection signal, not a gate.
4. **The warning sweep from B2** (`-Wmissing-prototypes -Wnested-externs
   -Wredundant-decls`, ~20 fixes) and, if/when clang is available, `scan-build`
   and `cppcheck` on the critical C files.

## Consolidated priorities

Top code improvements (ranked; owner-gated items are the owner's to schedule):

1. Fix the `je_sched_reap_dead` sign bug (A1) — add a `je_arch_irqs_enabled()`
   seam so it is hosted-testable. *Owner-gated.*
2. Make the AArch64 MMU audit cycle-safe and bounded (A2). *Owner-gated.*
3. Reject NULL kernel buffers in usercopy and add direct boundary tests (A3).
   *Owner-gated.*
4. Move the `.c` externs into headers and add the three warnings (B2).
5. Implement/document the real x86 VM invalidation contract (A4). *Owner-gated.*
6. Validate the Multiboot source block and module ranges (A6).
7. Remove core spawn's dependency on the harness ledger, then relocate the harness
   out of `kernel/core/` (B1). *Owner-gated.*
8. Split `JE_EP_CORRUPT` and halt on a dangling handle; centralize the duplicated
   RECV/recv-delivery logic (B3, B4). *Owner-gated.*

Top assurance additions (ranked):

1. The committed mutant harness + CI job (this PR).
2. `je_vm_audit` negative tests (6 survivors on the only spawn gate) → enable
   `GAP-VM-AUDIT-ONE-TO-ONE`.
3. The endpoint waiter-inflight use-after-free killer (C2) → enable
   `GAP-EP-WAIT-INFLIGHT`.
4. `test_device` dispatch/ack/death/quota tests (C3) → enable
   `GAP-DEV-DISPATCH-OWNER`.
5. The deterministic parser harness and the QEMU hardware-property tests (C4).

## Provenance

Two-peer complementary audit at maximum effort (Codex xhigh on the untrusted-input
/architecture slice, Claude opus/high on the logic/test slice, the second peer
also running an independent 28-mutant probe), reconciled and re-verified in-tree by
the lead. AI-assisted review, not independent human qualification. No production
code, contract, or status changed; owner-gated items are recommendations, not
changes.
