# JeiOS

JeiOS is an experimental operating system with a fresh C kernel. Its design
combines Unix interoperability, an NT-inspired object/service model, and an
integrated, deeply customizable desktop. Privacy, explicit authority, recoverable
operations and qualified hardware support are long-term goals.

**Current stage: M1/M2 single-CPU kernel foundation.** The AMD64 BIOS prototype
boots, handles fatal CPU exceptions, validates Multiboot2 memory information,
enforces read-only/NX mappings where supported, allocates RAM, and preemptively
runs seven guarded kernel threads. Its diagnostic workload verifies registers,
sleep/wakeup, heap contents and resource reclamation, then halts.

There is no userspace, application sandbox, SMP, filesystem, graphics, networking,
dynamic module loader or ARM implementation. This is not a daily-use or hardened OS.

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
- [Implementation ledger](docs/IMPLEMENTATION.md): current M1/M2 mechanisms, evidence and limits.
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

The OS core uses C11 with small architecture-specific assembly bridges. Early
AMD64 is the first compatibility target; AArch64 requires a separate port.
Modern features are enabled only after their OS prerequisites are implemented.
The desktop, native SDK, privacy controls and mobile/IoT ecosystem remain planned.

Original JeiOS code is licensed under [Apache License 2.0](LICENSE), selected by
the owner on 2026-10-13. Third-party components retain their own licenses and
require provenance/compatibility review. No external kernel source has been
imported. See [the licensing decision](docs/adr/0005-licensing.md).

