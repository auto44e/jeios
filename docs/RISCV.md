# RISC-V support plan and scaffold

Owner request: 2026-10-24. Status: planning scaffold only. No RISC-V build, boot,
userspace, physical-board or application support is qualified. Original code stays
C11 plus necessary architecture assembly, within auto44e/jeios; no second kernel.
This adds a port track without replacing current ARM64 or M4/M5a priorities.

RISC-V support is an ecosystem track under [ARCHITECTURE_SUPPORT.md](ARCHITECTURE_SUPPORT.md),
including services, SDK/toolchains, applications and all applicable product profiles,
plus separately qualified GPU/APU/NPU integration. RV0–RV4 are the CPU foundation;
whole-ecosystem support requires the cross-layer evidence in that policy.

## Candidate first target and decisions

Propose 64-bit RISC-V on QEMU `virt`, one hart (hardware execution context), with
an S-mode kernel entered through a separately pinned OpenSBI firmware. Firmware
handles machine-level services; it is not a Linux host. Sv39 is the candidate
virtual-memory scheme. These are reviewable starting choices, not a frozen ABI.
RV32, SMP, hypervisor extensions, vector/floating-point context, accelerators and
physical boards require separate gates. No vendor-wide compatibility promise.

Reason: an emulator-first RV64 port provides a bounded environment for validating
shared service/isolation semantics before board-specific firmware and drivers.
Alternative prediction: starting on a physical board would mix architecture
faults with recovery/device uncertainty; implementing machine-mode firmware first
would expand privileged scope before native kernel execution is established.
Flex: revisit the CPU/ISA/ABI, firmware, MMU and interrupt choices in RV0 based on
available reproducible tools. Do not silently change any accepted shared contract.

## Gates and acceptance

| Gate | Deliverable | Required evidence |
| --- | --- | --- |
| RV0 | Accepted architecture/boot ADR and reproducible target | Pin emulator version/machine properties, exact ISA extensions, compiler/binutils/ABI flags, firmware hash/license and image/load/handoff contract; resolve manifest nulls with evidence |
| RV1 | Entry, serial, boot-context validation and traps | Actual QEMU entry markers, stack/BSS proof, bounded DTB parsing/reservations; deliberate synchronous exception with expected cause/PC; malformed-input rejection |
| RV2 | Protected mappings, memory, interrupts/timer and scheduling | W^X/guards, no writable executable aliases, exhaustion/rollback, register preservation and progress under timer preemption |
| RV3 | U-mode process/syscall/handle/IPC parity | Two isolated tasks, illegal-access offender termination with survivor progress, checked user copies, stale/denied handles, capability transfer/waits/reclamation |
| RV4 | Native services/device integration and optional named hardware | Relevant M4+ gates, per-device lifecycle/DMA/recovery evidence; emulator success is not physical support |

## RV0 questions to settle before code

- Pin a specific QEMU binary/version and supported CPU properties rather than a
  moving default. Record one-hart topology, RAM size, console, interrupt controller
  and firmware configuration. Select exact ISA extension set and ABI explicitly;
  do not assume all RV64 processors have the same extensions.
- Record OpenSBI release/build/hash, provenance and license. Specify firmware
  entry privilege, hart ID/DTB handoff, image bounds/load address, reserved regions,
  initial interrupt/MMU state and supported SBI extensions. Probe required services
  and reject unsupported profiles; do not assume arbitrary firmware support.
- Choose toolchain, code model, relaxation policy, stack alignment and GP/TP use.
  Inspect generated instructions and ELF layout; prohibit accidental FP/vector
  code until complete architectural state management is implemented and tested.
- Specify trap layout, caller context, interrupt nesting policy, fault stack,
  delegation assumptions, return-state validation and exception diagnostics.
  Distinguish user faults, kernel defects and scoped copy-recovery faults.
- Define Sv39 mapping/ownership rules, page-table validation, access/dirty-bit
  handling, address canonicality, barriers/TLB invalidation and instruction-cache
  synchronization. No direct reuse of x86 ordering or page-table encodings.
- Select timer and interrupt delivery mechanisms for the pinned platform; specify
  acknowledge/completion, spurious interrupts, monotonic deadlines and bounded
  handlers. Early bring-up must not assume direct machine-mode register access.

## Shared contracts, security and evidence

Reuse portable allocation, object/rights and IPC semantics only after auditing
architecture assumptions. Keep raw pointers out of public ABI; zero pages before
cross-principal reuse, contain U-mode faults, validate copy ranges/return states,
and preserve ownership through teardown. Changes to frozen ADRs or tested paths
retain owner gates. No implicit DMA trust: later drivers need containment evidence
or an explicit restricted profile. Firmware is part of the trust boundary.

For each gate record source SHA, commands, tool/firmware versions, configuration,
image hash, sanitized serial logs, expected/observed markers and failures. Hosted
parser tests complement CPU execution; compilation alone is not boot evidence.
Add separate RISC-V CI compile and emulator jobs only when each actually works;
unavailable tools, unsupported profiles and timeout must not appear as PASS.
Preserve AMD64/ARM regression gates. Full POSIX, LDAP, Android and media runtime
support needs native qualification beyond RV3, not architecture-name matching.

## Getting started

Inspect config/riscv64.json and kernel/arch/riscv64/README.md. The null qualification
fields deliberately expose missing pins/evidence. First contribution: review and
freeze RV0 with exact tool/firmware metadata and failure contracts. Then add the
small RV1 executable path in a separate PR, retaining honest evidence. There is
no `make riscv64` target or boot command yet; do not run the AMD64 image in RV QEMU.
Use the conversational or manual workflow in [GETTING_STARTED.md](GETTING_STARTED.md).
No firmware source or binary is imported by this scaffold, and no device flashing
is authorized. Do not change the mobile integration pin to imply RISC-V support.

## Primary references

Reviewed 2026-10-24 for target/firmware planning; RV0 must pin revisions before
implementing architecture behavior:
- https://www.qemu.org/docs/master/system/riscv/virt.html
- https://github.com/riscv-software-src/opensbi

QEMU documents a synthetic virt platform and generated device tree. Its available
features are not automatically JeiOS features. OpenSBI provides the proposed
firmware layer, not a tested JeiOS integration in this change.
