# Hardware profiles and feature policy

Status: M1/M2 implements an AMD64 BIOS/PC foundation, qualified in QEMU only.
It enables NX where present, read-only supervisor code/data permissions, stack
guards, a legacy PIC/PIT timer and kernel threads. Physical allocation is limited
to RAM below 1 GiB. No ARM, GPU or newer extended CPU context is implemented.

## Meaning of compatibility

'64-bit' is an address/instruction property, not a universal machine interface.
The first baseline is early AMD64 PCs (K8-era is the design reference); AArch64
Armv8-A is the proposed second ISA. Alpha, SPARC64, MIPS64, POWER and IA-64 require
independent ports and are not covered. Initial smoke target is QEMU `pc/qemu64`,
128 MiB, legacy BIOS/GRUB, CD image, and COM1. Minimum physical RAM and actual
K8-era board support remain unverified. UEFI-only machines need a boot-path gate.

| Profile | CPU/boot target | Guarantee and fallback | Implementation |
| --- | --- | --- | --- |
| M1/M2 emulator | AMD64, BIOS GRUB, COM1, PIC/PIT | Memory protection and preemptive kernel workload, then halt; no process isolation | NX/no-NX and 16/128/1536 MiB profiles; no real-PC qualification |
| Legacy PC | Early AMD64; optional NX/IOMMU/modern state | Baseline instruction paths; weaker protections disclosed | Planned; real board matrix required |
| Hardened PC | NX, validated isolation prerequisites, maintained firmware | W^X, process separation, device containment when qualified | Planned; exact mandatory feature set pending |
| AArch64 baseline | Armv8-A on named board/emulator | Ordinary MMU process isolation; advanced features optional | No implementation |
| AArch64 enhanced | Selected PAC/BTI/MTE/POE/SVE/SME | Each mechanism enabled independently after support gates | No implementation |
| Confidential compute | RME/CCA or qualified x86 confidential VM platform | Specific guest/host boundary, not blanket OS trust | Research scope |
| Mobile / IoT | Named 64-bit MMU platform | Device-specific support and shared service contracts | Future; no phone certification |

## Capability descriptor

For each feature define stable internal ID, raw report, firmware/platform readiness,
OS readiness, policy permission, required context state, allowed CPU mask,
security effect, fallback, test evidence, and errata overrides. Keep **advertised**
separate from **enabled** and **required**. The original `je_features_select` is only a
portable mask helper; it does not populate CPUID records or enable features.

Never execute an optional instruction to see whether it works before traps and
containment exist. Enabling a feature may require control registers, page-table
formats, memory allocation, firmware negotiation, or context save/restore.
Do not advertise AVX/AMX, SVE/SME, protection keys, or tagging to applications
unless their ABI and context management are complete. CPU hotplug must revalidate
process requirements; affinity restrictions must be visible to the scheduler.

## Optional feature matrix

| Mechanism | Intended value | Prerequisites | Safe absence behavior |
| --- | --- | --- | --- |
| x86 NX | Non-executable data pages | EFER.NXE, valid page tables | Legacy profile; cannot claim data non-execution |
| SMEP / SMAP | Guard kernel access to user mappings | Trap/copy discipline and proper enablement | Software validation, visibly weaker protection |
| XSAVE / AVX / AVX-512 / AMX | Application optimized execution | State sizing, control configuration, per-thread management | Baseline code; deny incompatible binary requirements |
| CET / PAC / BTI | Control-flow mitigation | Compiler, runtime and context integration | Disclose absence; not memory-safety equivalence |
| MTE | Detect selected tagged-memory misuse | Tags, allocator, fault handling, ABI and context | Hardened software allocator where available |
| POE / POE2 | Permissions within an address space | Architecture-qualified registers/tables and switch discipline | Process isolation; refuse required compartment semantics |
| RME / CCA | Confidential workload domains | CPU, firmware, monitor, virtualization | Ordinary VM with clearly different guarantee or refusal |
| IOMMU / IRQ remapping | Device DMA/interrupt containment | Groups, ownership, mapped grants, invalidation | Trusted-driver legacy mode or disable device |
| VT-x / AMD-V / Arm virtualization | Isolated VM compatibility domains | Hypervisor implementation and state management | No VM service; application compatibility separately scoped |
| NUMA/topology/energy hints | Better scheduling and locality | Reliable discovery, affinity and power backend | Conservative portable scheduling |
| GPU acceleration | Rendering/compute/media | Device-specific driver, firmware, memory/fence/reset contracts | Software rendering or framebuffer only |
| NPU | Supported inference operators | Vendor/runtime/compiler and safe buffer lifecycle | CPU/GPU runtime where actually supported |

POE2 and newer architectural descriptions do not establish availability in a
particular shipping chip. Feature status must include hardware execution evidence.
MTE, permission overlays, Realms and TrustZone are different mechanisms; do not
call them all 'thread sandboxing' or use their names interchangeably.

## Required test matrix once the mechanism exists

1. Baseline emulator with minimum feature set, then a named early AMD64 machine.
2. Enhanced emulator/real CPU with NX and other implemented protections enabled.
3. Feature advertised but OS support deliberately absent: selection must refuse.
4. Policy denies feature: no instruction or hardware enablement may occur.
5. Heterogeneous CPU set and hotplug: no incompatible migration or stale ABI.
6. Missing/failed IOMMU: strong containment requests refuse; no false green UI.
7. Missing UART/display: boot does not spin forever waiting for diagnostics.
8. Real GPU reset/removal and buffer reuse tests before acceleration certification.

Support records must name commit, CPU, motherboard/SoC, firmware, device IDs,
configuration, test commands, results, and known failures. Emulator evidence and
real hardware evidence must remain separate.
