# ADR 0001: Fresh C kernel

Status: accepted for initial project by owner request, 2026-10-12.

The owner wants a new kernel and a C OS core. Use freestanding C11 and minimal
architecture assembly. Borrow documented ideas from Unix/BSD/Linux/NT without
combining their kernels. A shared architecture-neutral core has platform ports.

Consequences: hardware and ABI support must be implemented or explicitly ported;
existing driver ecosystems are not inherited. C memory safety requires rigorous
bounds/ownership discipline, hardening, fuzzing and isolated service boundaries.
Boot/traps/context switching need exact assembly despite the C core requirement.
