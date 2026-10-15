# Coding-agent starting guide

Updated 2026-10-14. Read this with ../AGENTS.md, HANDOFF.md and IMPLEMENTATION.md.
Owner requested a sustained coding-agent handoff. This guide records requirements
and continuation order; it does not claim later milestones are implemented.

Current status and timestamped history: [HANDOFF.md](HANDOFF.md). Ecosystem-wide
[zero trust](ZERO_TRUST.md) is an owner requirement; enforcement remains planned.

## Product philosophy across every subsystem

Owner clarification 2026-10-14: seamless everyday defaults plus granular,
customizable power-user/developer/organization control govern the whole OS and
ecosystem. Read the overall product vision near the start of DESIGN.md.
Security profiles are one application; apply the same progressive disclosure,
per-area customization, automation, coherent cross-device behavior and honest
recovery principles to all subsystem work. These are requirements, not implemented
features. Keep interface complexity separate from actual authority and capability.

## Establish the correct baseline

Foundation PR: https://github.com/auto44e/jeios/pull/1
PR #1 merged into main at `03aa2f380d8e49262abd1720a3b8db37517cbb31`.
Start new implementation branches from updated main; check for newer concurrent
work before editing. The additional multitouch/security-key requirements are
proposed documentation only. Verify CI for the exact implementation head used.

Implemented: M1 exceptions, validated memory discovery/protected mappings, and M2
bounded allocation plus seven trusted preemptible kernel workers on one AMD64 CPU.
Not implemented: userspace, capabilities/IPC, SIMD context, SMP, filesystem,
networking, GPU/NPU/media runtime, login, sync, encryption or domains.

## Work sequence and project-ready tasks

| Order | Issue | Dependency | Completion evidence |
| --- | --- | --- | --- |
| 1 | [#2: boundary contracts](https://github.com/auto44e/jeios/issues/2) | M1/M2 baseline | Reviewed ADR, register/state/VM/ABI contracts |
| 2 | [#3: private VM and bounded images](https://github.com/auto44e/jeios/issues/3) | #2 | Ownership, zeroing, permissions and rollback tests |
| 3 | [#4: CPL3 scheduling and local faults](https://github.com/auto44e/jeios/issues/4) | #2, #3 | Actual CPL3 execution, timer preemption, survivor progress |
| 4 | [#5: syscalls and user copies](https://github.com/auto44e/jeios/issues/5) | #3, #4 | Pointer/arithmetic failure tests and narrow fault recovery |
| 5 | [#6: qualify M3a](https://github.com/auto44e/jeios/issues/6) | #2–#5 | Adversarial QEMU and regression evidence on exact head |
| 6 | [#7: M3b handles](https://github.com/auto44e/jeios/issues/7) | #6 | Type/generation/rights/lifetime tests |
| 7 | [#8: M3c IPC and init](https://github.com/auto44e/jeios/issues/8) | #7 | Bounded transfer/wait/close/cancel semantics |
| Backlog | [#9: later owner requirements](https://github.com/auto44e/jeios/issues/9) | Relevant roadmap gates | Split into bounded tasks when prerequisites exist |

All eight issues start planned/open, not in progress or done. No assignee or
calendar deadline was invented. Project destination:
https://github.com/users/auto44e/projects/1/views/1
Issue creation does not imply membership in the project board.

Read [M3A.md](M3A.md) for the concrete proposal. Its temporary 2 MiB user arena,
limits, ABI numbers, interrupt entry and NX-required launch policy are proposed
engineering choices, not previously implemented interfaces or a frozen owner ABI.
Resolve these in the first ADR. Keep M3a/M3b/M3c distinct and reviewable.

## Owner requirements and authoritative design locations

| Requirement | Source and placement | Implementation boundary |
| --- | --- | --- |
| Fresh C11 core, modular services, Unix interoperability/NT-style object model | [DESIGN](DESIGN.md), ADR 0001/0002 | No foreign kernel-source fusion; ring 0 is trusted |
| Privacy, explicit authority, graceful failures; customization with good defaults | [SECURITY](SECURITY.md), [DESIGN](DESIGN.md) | UI complexity never grants rights; no telemetry/cloud-login requirement |
| OS-wide touchscreen/trackpad multitouch | [INPUT](INPUT.md), M4/M7/E1 | Qualified transports, secure routing, native toolkit/accessibility; foreign apps may use pointer fallback |
| Built-in hardware MFA security keys | [SECURITY_KEYS](SECURITY_KEYS.md), M4-M9 | CTAP/FIDO2 broker, native/WebAuthn boundaries, recovery; preboot/PQ capabilities qualified separately |
| Fresh-kernel mobile and ecosystem Android app support | [MOBILE](MOBILE.md), dedicated auto44e/jeios-mobile | Owner chose native C kernel; QEMU ARM64 first, Pixel candidates unqualified; AOSP is reference/future app layer |
| Native configurable init and supervision | [INIT](INIT.md), M3c/M4 | Minimal fixed-plan init first, full supervisor later |
| Single-device local login through multi-device/family/multi-user/fleet management | [IDENTITY](IDENTITY.md), M3 onward | Separate principal/device/session/tenant and management vs data-key authority |
| E2E protection built into identity/sync design; several pairing methods | [IDENTITY](IDENTITY.md), [ENCRYPTION](ENCRYPTION.md) | Qualified key lifecycle, entropy and reviewed protocols; no ad hoc crypto |
| PQ protection, nearby sharing and honest RAM/endpoint trust limits | [ENCRYPTION](ENCRYPTION.md), M5/M6/M9/P2 | Encryption is not universal secrecy from a privileged running OS |
| ext4 as provisional initial OS default | [STORAGE](STORAGE.md), M5 | Owner decision; still no filesystem code or production qualification |
| Raw-disk access without forced formatting; temporary/sidecar file-like views | [STORAGE](STORAGE.md) | Explicit range authority/leases and metadata; never silently format |
| Broad open-source filesystem interoperability | [STORAGE](STORAGE.md) | Qualify each format/version/read/write profile independently |
| Native domains and AD/Group Policy-like management | [DOMAINS](DOMAINS.md) | Native directory distinct from Microsoft AD DS compatibility |
| Windows/macOS/Linux domain interoperability | [DOMAINS](DOMAINS.md) | Explicit provider/endpoint matrix; Samba AD and FreeIPA/LDAP/Kerberos separately scoped |
| GPU/NPU across all planned platforms; open standards by default | [ACCELERATORS](ACCELERATORS.md), M4/M8 | Native drivers and tested per-device support; no universal availability claim |
| Smart CPU→GPU/NPU offloading, configurable APIs, conservative defaults | [ACCELERATORS](ACCELERATORS.md) | Eligible backend implementations, resource/latency/power budgets, precision and CPU fallback |
| Built-in FFmpeg, HandBrake and Blackmagic RAW support | [ACCELERATORS](ACCELERATORS.md), M5–M8 | Userspace integration; separate upstream licenses and native SDK feasibility |
| Apache-2.0 selected for original project code | [LICENSE](../LICENSE), [ADR 0005](adr/0005-licensing.md) | Implemented licensing; third-party provenance/terms still reviewed |
| Old AMD64 compatibility, AArch64/mobile/IoT and coherent customizable desktop | [HARDWARE](HARDWARE.md), [ROADMAP](ROADMAP.md), [DESIGN](DESIGN.md) | Qualified profiles and dependency gates; no shortcut past isolation |

## Sustained implementation discipline

Work one bounded gate at a time; continue routine implementation and fixes without
reasking already decided owner preferences. Keep small reviewable commits. Record
new architectural decisions and rationale in ADRs. Ask only when a material owner
decision or access limitation genuinely blocks useful work. Do not silently relax
security, replace standards-first goals with mandatory vendor dependencies, or
claim a later gate because its design document exists.

Before each change, inspect current code, open issues/PRs and tests to avoid
redoing work or overwriting another agent. Prioritize executable user-page aliases,
unsaved architectural state, safe TSS/CR3 switching, exact copy fixups, lifetime
and reclamation. A security invariant that cannot be upheld blocks the feature;
record the failure and next bounded fix rather than forging a passing gate.

At each checkpoint update HANDOFF, IMPLEMENTATION and ROADMAP in the same change:
implemented/tested/planned/blocked, exact commit and commands, observed results,
changed contracts, known failures and next task. Update related security/hardware
claims and issue state only to match evidence. Keep local and remote CI distinct.
Run `make check`; in this managed environment use
`ASAN_OPTIONS=detect_leaks=0 make check` only if LeakSanitizer cannot inspect tasks,
and disclose that limitation. Boot changes require `make smoke integration` with
new meaningful QEMU tests, exact expected results and a live survivor for fault
containment. Run `git diff --check` and validate documentation links.

Before ending a work session leave reproducible build/run instructions, current
branch/commit, a clean committed checkpoint where possible, remaining test failures,
resource/accounting invariants and next step. Do not merge/release, assign reviewers,
change credentials, contact vendors or import restricted source without applicable
authorization. The owner asked to approve PR #1; that is not an instruction to
invent an independent reviewer or silently bypass self-review restrictions.

## Cryptographic relock requirement — 2026-10-14

ENCRYPTION.md now requires a configurable ordinary-lock path to withdraw protected
user-domain keys and plaintext access without a power cycle. Include process/cache/
DMA cleanup, credential-gated reopening, background exceptions and truthful failure
status. Screen lock alone is insufficient; reboot equivalence is not assumed.
Key-domain and lifecycle design precede storage/identity format freeze; runtime
qualification remains a later gate. No implementation was added.

## Protected system and version management — owner decision 2026-10-14

STORAGE.md records seamless protected/versioned system deployments by default,
full owner customization including an immutability off switch, and independent
intuitive OS/built-in-app update/rollback in both protected and mutable modes.
Native apps must participate through lifecycle and schema compatibility contracts.
Write protection, deployment history, trust policy and data encryption are distinct.
Mutable-mode direct edits require explicit conflict/checkpoint handling; rollback
must not silently revert user documents or bypass data/anti-rollback constraints.
Accepted direction, not implemented behavior; M3a remains next.

## Multiple default filesystems — owner decision 2026-10-14

STORAGE.md now records Btrfs, OpenZFS and a mobile backend as first-class long-term
default choices selected by qualified device profiles. F2FS is the proposed mobile
candidate. Owners can select a conforming custom backend, including as system
default with additional boot/recovery qualification, or use a visibly experimental
profile. Native APIs keep workflows coherent without claiming identical features.
ext4 stays the initial bring-up target. No automatic reformat/conversion or new
implementation is implied. M3a remains next.
