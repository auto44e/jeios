# JeiOS

Standards adopted as design requirements (2026-10-26): [standards and evidence](docs/STANDARDS.md)
and [organizational ISO 27001 support features](docs/ORG_SECURITY.md). Implementation
and formal qualification remain gated; these documents add no runtime behavior.

Ecosystem scope: [CPU, GPU, APU and NPU architecture support](docs/ARCHITECTURE_SUPPORT.md)
across kernel, drivers, runtimes, SDK, applications and product profiles. Planning
scaffolds do not establish runtime or hardware qualification.

RISC-V: [RV64 scaffold and gated port plan](docs/RISCV.md) added 2026-10-24.
Planning only; no RISC-V execution or hardware support claimed. Existing core
and ARM64 implementation priorities remain unchanged.

New contributor? Start with the [development getting-started guide](docs/GETTING_STARTED.md),
including a plain-English conversational path with an agent.

JeiOS is an experimental operating system with a fresh C kernel. Its design
combines Unix interoperability, an NT-inspired object/service model, and an
integrated, deeply customizable desktop. Privacy, explicit authority, recoverable
operations and qualified hardware support are long-term goals.

**Current stage: M3 complete on AMD64; ARM64 MP1 partially implemented.**
The experimental single-CPU kernel has isolated userspace, checked syscalls,
typed rights-bearing handles, bounded capability IPC, blocking receive and a
kernel-seeded two-process bootstrap fixture. ARM64 entry, console and EL1 vectors
run in QEMU; bounded DTB parsing remains before MP1 completion.

There is no general service supervisor, SMP, filesystem, desktop, networking or
Android runtime. This is not a daily-use or hardened OS. See the
[M3 evidence](docs/M3C_RETRO.md) and [next phase](docs/NEXT_PHASE.md).

## Start here

Coding agents: begin with [the starting guide](docs/AGENT_START.md) for the current
baseline, all owner requirements, issue dependencies and checkpoint rules.

- [Detailed design](docs/DESIGN.md): full product architecture and future contracts.
- [M3a implementation proposal](docs/M3A.md): proposed boundaries and M3b/M3c contracts.
- [Accelerators and media](docs/ACCELERATORS.md): open standards, conservative offloading,
  and planned FFmpeg/HandBrake/Blackmagic RAW integration; no runtime support yet.
- [Init/service design](docs/INIT.md), [encryption and nearby sharing](docs/ENCRYPTION.md),
  and [filesystem recommendation](docs/STORAGE.md): planned contracts and acceptance gates.
- [Identity and device management](docs/IDENTITY.md): local login, E2E sync, family and fleet contracts.
- [Domains and centralized policy](docs/DOMAINS.md): native joining and separate AD/GPO compatibility targets.
- [Multitouch input](docs/INPUT.md) and [hardware security keys](docs/SECURITY_KEYS.md): planned OS-wide integration.
- [Fresh-kernel mobile project](docs/MOBILE.md): shared core, capability parity and future Android app layer.
- [Implementation ledger](docs/IMPLEMENTATION.md): mechanisms, evidence and limits.
- [Hardware policy](docs/HARDWARE.md) and [security model](docs/SECURITY.md).
- [Development](docs/DEVELOPMENT.md): build, emulator tests and fault modes.
- [Roadmap](docs/ROADMAP.md): gated milestones and dependency order.
- [Agent handoff](docs/HANDOFF.md) and [AGENTS.md](AGENTS.md): continuation instructions.
- [References](docs/REFERENCES.md): primary architecture sources.

## Build and verify

On x86-64 Linux with GCC, binutils, make and Python 3:

```sh
make check
```

With GRUB PC utilities, xorriso and QEMU installed:

```sh
make smoke integration
```

Normal outputs are in `build/0/`; each fault mode uses a separate directory.
See DEVELOPMENT.md for sanitizer requirements, CPU/memory profiles and limitations.

The OS core uses C11 with small architecture-specific assembly bridges.
AMD64 is the first compatibility target; the AArch64 port has reached
early QEMU boot and exception handling, with MMU/scheduling/userspace still ahead.
Modern features are enabled only after their OS prerequisites are implemented.
The desktop, native SDK, privacy controls and mobile/IoT ecosystem remain planned.

Original JeiOS code is licensed under [Apache License 2.0](LICENSE), selected by
the owner on 2026-10-13. Third-party components retain their own licenses and
require provenance/compatibility review. No external kernel source has been
imported. See [the licensing decision](docs/adr/0005-licensing.md).

## Core standards direction

[POSIX.1-2024 and LDAPv3](docs/STANDARDS.md) are accepted core requirements,
implemented in stages across desktop/mobile. Both remain planned; neither current
conformance nor directory login is claimed. Organizational enrollment is optional.
