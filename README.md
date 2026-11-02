# JeiOS

[Included native software portfolio](docs/NATIVE_PRODUCTS.md): latest-stable source policy, C coreutils/GNU+uutils tracking, SSH/TLS, compilers, Make, curl, FIDO, zlib and QEMU. Planned integration; no runtime support added.

Owner-approved [two-phase native shell and selectable shell roadmap](docs/SHELLS.md): recovery + original JeiOS sh, included native Bash/Zsh, and bring-your-own shell. PowerShell deferred. Planned, not runtime support.

## Current ecosystem status — observed 2026-11-01T21:30:30-05:00

Core main `0f702036c52d40d03bb4c8919d0dcae53ba9f2b1`: AMD64 M3 and scoped M4 complete; PR #70
merged. ARM64 MP1 complete (#52); MP2 then MP3 remain. Main CI passed:
https://github.com/auto44e/jeios/actions/runs/41376309893 . M4 qualifies bounded
supervisor/broker/COM2 UART scenarios, not arbitrary hardware, DMA or a release.
Mobile main `f1d9b0d87746e9dc353a2f48e7b9d27e7fe86d07` has passing planning CI:
https://github.com/auto44e/jeios-mobile/actions/runs/39243515951 . Its core pin
remains older M1/M2; upstream progress does not qualify that integration.
No open PRs were observed before this refresh. M4 #53 is now closed as completed;
MP2 (mobile #2) is next execution work; M5a #54 remains planned.

Shell roadmap: SH1 recovery #71, SH2 original native sh #72, included native
Bash/Zsh and custom-shell registration #73; mobile parity #12. PowerShell deferred.
All shell implementation remains planned. Storage, networking, desktop, POSIX
completion, LDAP, encryption, fleet/ISO 27001 features, RISC-V, accelerators and
Android execution remain later gates. Standards adoption is not certification.
This current status supersedes older next-task/status wording below; timestamped
records remain evidence at their original revisions, not new test runs.


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

**Current stage: scoped AMD64 M4 complete; ARM64 MP1 complete.**
Protected memory, preemptive scheduling, userspace/handles/IPC, bounded process
launch/reaping, supervisor/broker and controlled COM2 UART scenarios are tested.
ARM64 boot/vectors/validated DTB run in remote QEMU CI; MP2/MP3 remain ahead.
No interactive shell, filesystem, networking, GUI, SMP or Android runtime exists.
See the current status above and NEXT_PHASE.md for qualification limits.

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
