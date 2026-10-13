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
`develop/m1-m2-core`; see IMPLEMENTATION.md and HANDOFF.md for exact evidence.
M1 has validated maps/reservations, fatal exceptions and hardware permission tests.
M2 has bounded page/heap allocation, timer preemption, guarded kernel threads,
yield/sleep/exit and deferred stack reclamation. This is not a general scheduler,
SMP implementation, userspace sandbox, or complete hardening milestone.

Next is M3, split into independently reviewable subgates:

- **M3a:** private user address spaces, ring-3 entry/return, user-copy fault
  containment and a bounded user program loader. Prove two actual CPL3 tasks
  cannot access each other's memory or kernel state. A task fault must be local.
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

These design additions do not change the next implementation gate, M3a.

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
M3a remains next, followed by M3b handles and M3c IPC/init.

[ACCELERATORS.md](ACCELERATORS.md) defines open-standards-first GPU/NPU support
across planned platforms, qualified per device. M4 supplies device/buffer/DMA
boundaries; M5/M6 supplies application/storage prerequisites and software media
ports with qualified CPU state; M7 adds desktop integration; M8 qualifies GPU/NPU
backends and measured automatic offloading. FFmpeg, HandBrake and Blackmagic RAW
are userspace integration targets, not current codecs or kernel features.
