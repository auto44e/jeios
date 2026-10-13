# Agent handoff: M1/M2 kernel foundation

Updated: 2026-10-13. Project `auto44e/jeios`; current documentation branch
`docs/mobile-ecosystem` (PR #11). M1/M2 foundation PR #1 is merged into main.
Read AGENTS.md, IMPLEMENTATION.md, ROADMAP.md and the relevant DESIGN.md sections.
Start with [AGENT_START.md](AGENT_START.md) for the consolidated requirements and
linked work queue. Multitouch/security-key PR #10 is merged into main at
`5f290b3`; this branch incorporates that merge and preserves both feature plans.
The previous M0 state is preserved in git history; this file describes the M1/M2
implementation and subsequent documentation checkpoints. Do not infer future capabilities from design prose.

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

Start a new implementation branch from updated main for M1/M2. Read mobile PR #11
while it remains open, or its merged docs once available on main. Specify user/kernel address
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
