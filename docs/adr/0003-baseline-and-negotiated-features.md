# ADR 0003: Early AMD64 baseline with optional modern features

Status: provisional interpretation of owner's compatibility requirement.

First target early AMD64-class PCs and a known QEMU platform; add AArch64 as its
own port. Do not reinterpret '64-bit' as support for all historical ISAs.
Compile baseline code without host-specific instruction assumptions. Advertise a
feature only after hardware/platform, OS context handling and policy permit it.

Legacy boot capability is separate from hardened support. Missing NX/IOMMU/etc.
cannot be hidden behind a fallback claiming the same protections. Advanced
vector/protection state affects scheduling, migration, hotplug and ABI.

Actual old-machine minimum RAM/board and hardened minimum feature set remain open.

## Decision-record annotation (reconstructed — sweep 2026-10-19)

Per ADR 0000; reconstructed from this ADR's text only. Part 4: no transcript
retained.

**Decision + why (as recorded).** Target early AMD64-class PCs + a known QEMU
platform first; AArch64 is its own port; do NOT reinterpret "64-bit" as support
for all historical ISAs; compile baseline code without host-specific instruction
assumptions; advertise a feature only after hardware/platform + OS context
handling + policy permit it. Status: provisional interpretation of the owner's
compatibility requirement.

**Predictions if other choices were made (reconstructed).**
- *Claim broad "64-bit" ISA support up front:* the Consequences warn this hides
  reality — "missing NX/IOMMU/etc. cannot be hidden behind a fallback claiming the
  same protections"; advertising a feature before context/policy support would be
  a false hardening claim (the exact discipline ADR 0007 D2 later enforces for NX).
- *Bake in modern vector/protection state early:* recorded as affecting
  "scheduling, migration, hotplug and ABI" — committing before those exist would
  constrain later design.

**Flex options to consider in future (reconstructed).** In-text open item: "Actual
old-machine minimum RAM/board and hardened minimum feature set remain open"
(`FLEX (planning review)`). The principle (capability advertised only when truly
supported) is firm.
