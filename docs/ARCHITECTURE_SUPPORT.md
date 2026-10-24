# Ecosystem architecture support

Owner direction: 2026-10-24. This policy covers the entire JeiOS ecosystem:
shared kernel, platform and device drivers, system services, language/toolchain
and application runtimes, SDK/build/package tools, and desktop, mobile, server
and IoT product profiles. It applies to RISC-V and other CPU, GPU, APU and NPU
architectures. It is a planning scope, not a claim that any untested target works.
Existing milestone evidence remains authoritative; this adds no executable support.

## Architecture is more than a CPU name

| Class | Qualification scope |
| --- | --- |
| CPU | Exact ISA/extensions and ABI, boot firmware, traps, memory model/MMU, interrupts, context state and native userspace |
| GPU | Named device/revision, driver and firmware, memory/command validation, graphics/compute runtime, display where applicable and recovery |
| APU / integrated SoC | Qualify CPU and each integrated accelerator separately, then shared-memory/coherency, DMA, power/thermal and reset interactions |
| NPU | Named device/revision, driver and firmware, model/compiler/runtime format, supported operations, numerical tolerance, buffer ownership and cancellation/reset |

CPU compatibility does not imply accelerator compatibility. A RISC-V host may
use a separately qualified accelerator; an accelerator's internal instruction set
does not make it a general-purpose host target. Do not model APUs as a new CPU ISA
or claim an entire vendor family from one device. Other architectures enter the
same process without a universal hardware promise.

## Shared qualification ladder

1. **Planned:** identify product use, component owners, dependencies, provenance,
   required guarantees and missing evidence. No runnable support implied.
2. **Buildable:** pinned tools produce inspected artifacts for exact profiles.
   Compilation is not execution evidence.
3. **Experimental execution:** reproducible native/emulated CPU or named-device
   execution, positive and negative tests, and declared limitations.
4. **Integrated ecosystem profile:** kernel, drivers, services, required runtimes,
   SDK/package selection and product workflows pass together at recorded revisions.
5. **Qualified profile:** review, security, recovery, update and regression gates
   pass for the named hardware/software combination; record maintenance ownership.

Each component has its own evidence level. A product's claim is limited by its
required components; optional hardware must be explicitly unavailable or use a
qualified fallback. These are planned reporting conventions, not CI enforcement
or a replacement for existing ROADMAP gates. Emulator qualification remains
separate from physical-device qualification.

## Work across the ecosystem

| Layer | Required work before an integrated claim |
| --- | --- |
| Kernel/platform | CPU boot/isolation parity, portable core audit, per-platform firmware and memory/interrupt contracts |
| Devices/drivers | Discovery, rights, buffer grants, DMA containment or explicit restricted profile; timeout, reset, teardown and device-loss evidence |
| Services/runtimes | Graphics/compute/ML capability discovery, versioned interfaces, bounded resource use, cancellation and tested fallback semantics |
| SDK/toolchains | Target tuples/ABI, cross-build instructions, headers/libraries, artifact inspection and reproducible dependencies |
| Applications/packages | Architecture and accelerator requirements, compatible runtime versions, explicit install/launch rejection and tested workloads |
| Products | Desktop/mobile/server/IoT integration manifests, power/update/recovery and end-to-end evidence on each claimed profile |
| CI/docs | Per-profile build/execution jobs, evidence artifacts, honest blocked/skipped results and synchronized support documentation |

Keep portable logic shared; isolate hardware backends. Avoid inventing one generic
accelerator ABI before device needs and security contracts are reviewed. Graphics,
compute and ML may share buffer lifecycle mechanisms but retain distinct capability
and correctness requirements. Fallbacks must preserve the requested guarantees;
reject a request explicitly when they cannot. Contributor assistance level never
changes these requirements.

## First bounded tracks

The [RISC-V plan](RISCV.md) is the initial CPU scaffold. RV0–RV3 establish the
native foundation; RV4 opens integration, not automatic whole-ecosystem support.
Follow with explicit service/runtime, SDK/package and product gates above. This
coexists with AMD64 and ARM64 work and does not reorder MP1/M4/M5a priorities.

For GPU/APU/NPU support, start with one named device/profile proposal and its
firmware/license, tooling availability, isolation requirements and test strategy.
Driver implementation depends on the applicable M4 device/buffer contracts;
acceleration qualification also follows M8. No device is selected or qualified
by this document. Avoid empty driver implementations that return success.

Use [the profile template](../config/architecture-profile.template.json) to record
cross-layer scope and evidence. It is a documentation scaffold, not consumed by
the build, package resolver or runtime. Keep unknown pins null and evidence empty.
A future validator must reject incomplete profiles for qualification; merely
editing a status field never grants support. Record exact source revisions,
commands, environment, expected/observed results, sanitized logs and known gaps.
Publish links in the core handoff and each affected product repository before
making a cross-repository support claim. Mobile pins remain unchanged until tested.
