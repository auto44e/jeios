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
