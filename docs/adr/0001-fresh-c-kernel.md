# ADR 0001: Fresh C kernel

Status: accepted for initial project by owner request, 2026-10-12.

The owner wants a new kernel and a C OS core. Use freestanding C11 and minimal
architecture assembly. Borrow documented ideas from Unix/BSD/Linux/NT without
combining their kernels. A shared architecture-neutral core has platform ports.

Consequences: hardware and ABI support must be implemented or explicitly ported;
existing driver ecosystems are not inherited. C memory safety requires rigorous
bounds/ownership discipline, hardening, fuzzing and isolated service boundaries.
Boot/traps/context switching need exact assembly despite the C core requirement.

## Decision-record annotation (reconstructed — sweep 2026-10-19)

Per ADR 0000, this pre-policy decision is brought to the 3-part format. The parts
below are **reconstructed from this ADR's own text only** (AGENTS.md history rule:
never fabricate after-the-fact reasoning); anything the record does not support is
marked "unknown — not in the record."

**Decision + why (as recorded).** A fresh C OS core on freestanding C11 + minimal
arch assembly, borrowing documented ideas from Unix/BSD/Linux/NT without combining
their kernels; an arch-neutral core with platform ports. Driver: owner request
(2026-10-12). No peer-debate transcript was retained for this decision — part 4
(attached correspondence) is "transcript not retained."

**Predictions if other choices were made (reconstructed).**
- *Fork/derive an existing kernel instead of a fresh core:* the Consequences
  already name the cost this avoids — you would inherit a driver ecosystem and ABI
  but also that kernel's license, architecture and security posture, contradicting
  the owner's "new kernel, C core" requirement.
- *A non-C core (e.g. Rust):* not in the record as considered; predicted
  consequence **unknown — not in the record**. (The Consequences do record the
  accepted cost of C: "memory safety requires rigorous bounds/ownership
  discipline, hardening, fuzzing and isolated service boundaries.")

**Flex options to consider in future (reconstructed).** The decision is **firm**
(an owner-level project premise; a change needs a new owner decision + ADR). The
arch-neutral-core / platform-port split is the one structural choice later
milestones exercise (the ARM64 MP track is its first test).
