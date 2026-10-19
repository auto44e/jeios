# Gated roadmap

Coding-agent continuation: [AGENT_START.md](AGENT_START.md) maps owner requirements,
current baseline and linked issue dependencies. Future designs remain unimplemented.


No milestone has a calendar deadline. Each gate requires code, failure tests,
implementation-status updates, and a handoff with observed evidence. Existing
M0 stubs do not count as completing future modules.

| Gate | Scope | Pass criteria | Dependencies |
| --- | --- | --- | --- |
| M0 | Freestanding AMD64 bootstrap, serial, core helpers, design | Build + ELF checks + host tests; boot smoke separately observed | Initial scaffold |
| M1 | Exceptions, boot block validation, RAM reservations, correct mappings | Known exception produces bounded diagnostic; malformed boot maps rejected; kernel/data permissions match qualified CPU | M0 |
| M2 | Page/heap allocation, timers, IRQs, kernel threads and context | Exhaustion/rollback tests; preemption/context correctness; lock/context rules; NX and guarded stacks where supported | M1 |
| M3 | User address spaces, handles, IPC, syscalls, minimal init | Two isolated tasks run; illegal access contained; stale/denied handles rejected; no raw pointer ABI | M2 |
| M4 | Service supervisor, device graph, buffer grants, basic driver | Crash-loop budget; generation revocation; device timeout; DMA containment qualified or explicit legacy policy | M3 |
| M5 | Read-only initfs then durable storage and configuration | Checked image parser, durability/error behavior, interrupted commit/recovery tests | M4 |
| M6 | POSIX subset, networking, native service SDK | Tested interfaces, per-app network mediation and revocation, portable tool builds | M3-M5 |
| M7 | Framebuffer/software compositor, shell/settings, native UI contracts | Accessible interaction, scoped prompts, data themes, recovery shell, complexity/authority independence | M5-M6 |
| M8 | Hardware acceleration and advanced CPU state | Named GPU driver, reset isolation, feature migration/context tests, benchmarked fallbacks | M4 and per-feature gates |
| M9 | Secure boot/update/encryption and hardened profile qualification | Signed/recoverable images, anti-downgrade policy, keys outside repo, power-loss and security review evidence | M5 plus platform support |
| P1 | AArch64 board/emulator | Own boot/traps/timer/MMU tests; shared core checks; no invented feature support | M1 contracts; platform port parallel to later work |
| P2 | Enhanced ARM / confidential workloads | Platform-qualified features, context/ABI/fault tests, explicit guarantees | P1 and relevant M3/M8/M9 contracts |
| E1 | Mobile | Named board/device, power/touch/display/modem separation and update path | P1, M7-M9 plus device drivers |
| E2 | 64-bit MMU IoT | Headless profile, scoped management, offline recovery, power loss tolerance | Relevant M4-M9 mechanisms |

## Current gate status and next task

M0, M1a, M1b and the scoped M2 BSP foundation are implemented on
main (foundation PR #1 merged 2026-10-13); see IMPLEMENTATION.md and HANDOFF.md for exact evidence.
M1 has validated maps/reservations, fatal exceptions and hardware permission tests.
**Update 2026-10-19:** M3a, M3b and **M3c are all complete and merged** — the full
M3 user-address-spaces / handles / IPC / syscalls / minimal-init gate is met.
M3a (userspace boundary) is qualified (`docs/M3A_RETRO.md`); M3b (typed handles +
rights attenuation, ADR 0008); **M3c** (bounded capability IPC, blocking waits, and
the two-process bootstrap fixture, **ADR 0009**, issue #8 CLOSED) — full D-SEQ
(steps 1–7) merged on `main` at `bac8c3b`; retro + exact evidence in
`docs/M3C_RETRO.md`. A second architecture, **ARM64**, reached **MP1 step 2**
(ADR 0002 D5-3): entry/console (step 1) + EL1 exception vectors + the D5-3 trap
frame (step 2), proven on real QEMU; the bounded DTB parse finishes MP1 (next
session — see HANDOFF pickup items).
M2 has bounded page/heap allocation, timer preemption, guarded kernel threads,
yield/sleep/exit and deferred stack reclamation. This is not a general scheduler,
SMP implementation, hardened sandbox, or complete hardening milestone — M3's claim
remains "experimental single-CPU userspace + capability IPC in qualified emulator
profiles, proven by test," not a release qualification.

M3 is complete; next are M4+ (core) and the MP2+ ARM64 track (MMU/timer/scheduling,
now atop MP1 vectors), per the gate table above and the HANDOFF.

- **M3a:** private user address spaces, ring-3 entry/return, user-copy fault
  containment and a bounded user program loader. Prove two actual CPL3 tasks
  cannot access each other's memory or kernel state. A task fault must be local.
  The boundary contract is frozen in [ADR 0007](adr/0007-m3a-userspace-boundary.md)
  (accepted 2026-10-17); issues #3–#6 implement and qualify against it. Issue #3
  (hosted per-process VM builder + bounded image loader) is implemented and
  hosted-tested — see IMPLEMENTATION.md "M3a #3". Issue #4 is being landed in
  reviewable, individually QEMU-verified steps: step 1 (ring-3 GDT descriptors),
  step 2 (real per-process page tables + CR3, CPL0-verified), step 3 (CPL3
  entry/return — real ring-3 execution under preemption, CR3 + TSS.RSP0, D7
  enforcement), step 4 (fault-local process death + the D6 int 0x80 gate + the H1
  kernel-DoS fix — a synchronous user fault / user int 0x80 / bad-return-state
  kills only the offender, a peer is proven to keep running, the dead process is
  reclaimed). Steps 1–4 are merged/landing; #4 acceptance ("illegal access kills
  only the offender; peers survive") is met in QEMU. Issue #5 is the integrated
  two-task isolation qualification + the real syscall handler (copy-in/out) behind
  the int 0x80 gate, landing in steps: step (a) — the v0 syscall dispatcher
  (abi_query/yield/exit, UNSUPPORTED/INVALID, ABI isolated from internal status,
  validated syscall returns) — and step (b) — copy_from/to_user + copy_probe with
  per-page prevalidation and the exact-RIP exception-fixup copy sites — are both
  done and QEMU-verified. With #2–#5 complete, the ADR 0007 M3a userspace
  boundary is implemented end to end (private address spaces, ring-3 execution,
  fault-local death, bounded syscalls, recoverable user copies), proven in QEMU
  and peer-cross-reviewed (not independent human qualification).
- **M3b:** typed handles with generation counters, rights attenuation, explicit
  object lifetime and checked syscall layouts. Test stale/forged handles and
  denied operations without side effects.
- **M3c:** bounded capability-aware IPC, waiting/cancellation semantics and init
  process startup, with race/timeout/lifetime tests.

Before calling M3 hardened, resolve entropy/canary strategy, general wait queues
and synchronization, spurious IRQ handling and a security review of the user/kernel
transition. Broader hardening remains a separate acceptance claim. Do not expand
storage, network, GPU or desktop scope as a substitute for these boundaries.

## Decision gates

Before imports: record provenance and review compatibility with the selected
Apache-2.0 project license; preserve each dependency's terms.
Before stable ABI: specify native wire layout, rights/lifetime and concurrency.
Before hardened profile: define exact minimum hardware and firmware/update policy.
Before accelerated desktop: select native toolkit/compositor strategy in an ADR.
Before foreign compatibility: scope exact ABI/application set and security mapping.

## Additional owner requirements (2026-10-13)

These design additions do not change the current implementation focus (M3b, then M3c).

- M3c/M4: [INIT.md](INIT.md), configurable supervisor with good defaults,
  generation-safe readiness, bounded restarts, recovery and explicit authority.
- M5a-c: [STORAGE.md](STORAGE.md), initfs/RAM filesystem, read-only ext4 candidate,
  then independently qualified writable storage. Production default remains open.
- M5/M6/M9/P2: [ENCRYPTION.md](ENCRYPTION.md), key lifecycle before format freeze,
  optional nearby sharing, reviewed PQ protection, qualified memory encryption
  and separate confidential-execution gates. No new crypto code is authorized
  for production use merely by this design; library/protocol review is required.

- M3/M4 through M9: [IDENTITY.md](IDENTITY.md), local principals/sessions first,
  then durable identity records, multi-method pairing and E2E sync qualification.
  Family/tenant membership and fleet scale require additional acceptance evidence;
  no cloud account is required for unmanaged local login.
- M5 and later format gates: owner-approved provisional ext4 default, raw block
  I/O with exclusive range leases, temporary/sidecar views without reformatting,
  and a qualified filesystem interoperability matrix. Additional formats are
  planned individually; no blanket current read/write support is implied.

- Enterprise follow-on after M3-M6/M9 prerequisites: [DOMAINS.md](DOMAINS.md),
  native directory/join lifecycle and centralized policy gates, followed by
  independently qualified AD DS client and selected GPO mapping gates.

  The interoperability matrix includes Windows/macOS/Linux endpoints, Microsoft
  and Samba AD providers, and separately scoped FreeIPA/LDAP/Kerberos integration.
  Qualify cross-platform agents and policy mappings individually.

## Accelerator/media requirements and license decision (2026-10-13)

Apache-2.0 is selected for original project code; see ADR 0005.
[M3A.md](M3A.md) supplies a proposed implementation sequence, not a completed gate.
M3a and M3b are complete; M3c IPC/init is in progress (ADR 0009; endpoint lifecycle landed). The ARM64 MP track runs alongside (MP1 booted).

[ACCELERATORS.md](ACCELERATORS.md) defines open-standards-first GPU/NPU support
across planned platforms, qualified per device. M4 supplies device/buffer/DMA
boundaries; M5/M6 supplies application/storage prerequisites and software media
ports with qualified CPU state; M7 adds desktop integration; M8 qualifies GPU/NPU
backends and measured automatic offloading. FFmpeg, HandBrake and Blackmagic RAW
are userspace integration targets, not current codecs or kernel features.

## Multitouch and security keys (2026-10-13)

[INPUT.md](INPUT.md): M4 input transports/lifecycle, M7 compositor/toolkit/secure
UI and accessibility, E1 touch device qualification. [SECURITY_KEYS.md](SECURITY_KEYS.md):
M4 authenticator transports, M5/M6 credential broker/storage, M7 enrollment UI,
M9 authentication/recovery qualification. Preboot support is a separate gate.
These requirements extend the later backlog; M3a remains the next kernel task.

## Fresh-kernel mobile track

[MOBILE.md](MOBILE.md) links auto44e/jeios-mobile: MP0 ARM64 contract, MP1-MP3
native virtual execution/isolation, MP4 Pixel feasibility, MP5-MP6 physical mobile
bring-up/services, AC0-AC2 future Android compatibility. Fresh C kernel remains
shared; no AOSP/Linux host or lunch target. (M3a and M3b are complete; M3c is in progress; ARM64 MP1 booted.)

## Zero trust throughout the ecosystem — 2026-10-14

[ZERO_TRUST.md](ZERO_TRUST.md) maps enforcement to M3a isolation, M3b rights,
M3c IPC, M4-M6 brokers/network/storage, M7-M9 consent/identity/recovery and mobile
qualification. Treat this as a requirement across gates, not a completed feature
or a reason to skip M3a. See HANDOFF.md for dated current status and history.

## Core standards commitment — 2026-10-16

[STANDARDS.md](STANDARDS.md): target POSIX.1-2024 as a core application environment;
M6 remains an initial tested subset followed by complete-profile qualification.
Built-in LDAPv3 provider/identity integration is also core, with optional enrollment.
M3-M6 and security prerequisites still apply; M3a and M3b are complete and M3c is the current focus.
Track [POSIX #15](https://github.com/auto44e/jeios/issues/15) and
[LDAP #16](https://github.com/auto44e/jeios/issues/16). Certification, AD DS/GPO and
cloud enrollment are separate goals, not implied completion.

## Curl-inspired CI direction — 2026-10-16

[CI_STRATEGY.md](CI_STRATEGY.md) records the owner's short/long-term testing goal:
portable regressions, compiler diversity, sanitizers/static analysis, replayable
failure injection and fuzzing, supplemented by native kernel execution and storage
crash-recovery qualification. [Core #18](https://github.com/auto44e/jeios/issues/18)
is the first bounded CI implementation task. Existing gates remain mandatory;
M3a and mobile MP0 remain next. This update adds planning only, not workflow jobs.

## Ecosystem coding discipline — 2026-10-16

[CODING_STANDARD.md](CODING_STANDARD.md) applies NASA/JPL-inspired rules with
stricter critical-component profiles. [Core #19](https://github.com/auto44e/jeios/issues/19)
tracks audit and enforcement with CI #18. Incremental adoption supports each gate;
it does not replace M3a or establish NASA certification.
