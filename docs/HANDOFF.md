# Agent handoff: M1/M2 kernel foundation

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

