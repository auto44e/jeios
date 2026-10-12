# Gated roadmap

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

## Immediate bounded next task

Implement M1a: install an x86-64 IDT and dedicated fault path before enabling
interrupts. Define trap-frame layout and C entry alignment; avoid compiler
floating-point/SIMD state. Demonstrate an intentional fault in QEMU producing a
specific serial record rather than a triple fault. The production/default M0
smoke marker must still pass when no fault is injected. Use a build/test option
for fault injection, not an unconditional crash.

Then M1b: validate/copy the Multiboot2 memory map and reserve bootstrap ranges.
Only after this may M2 allocate RAM. Add host parser fuzz/boundary tests and
emulator scenarios. Trap containment must precede touching unchecked firmware
pointers. Do not add disk writes, external networking, or GPU command submission
as a shortcut to visible progress.

## Decision gates

Before imports: owner chooses license and contributors record provenance.
Before stable ABI: specify native wire layout, rights/lifetime and concurrency.
Before hardened profile: define exact minimum hardware and firmware/update policy.
Before accelerated desktop: select native toolkit/compositor strategy in an ADR.
Before foreign compatibility: scope exact ABI/application set and security mapping.
