# Ecosystem standards adoption

Owner decision: 2026-10-26. Adopt the following standards as scoped engineering
requirements across core, mobile, SDK, services and applications. This decision
adds requirements and a verification process; it does not certify existing code.
Existing frozen contracts and roadmap sequencing remain in force.

| Baseline | Adoption scope | Evidence required before a conformance claim |
| --- | --- | --- |
| ISO/IEC/IEEE 9945:2026 (POSIX Issue 8) | Planned POSIX application environment; implement incrementally behind native capability boundaries | Named interface/utility inventory, semantic and failure tests, explicit unsupported features; a subset is not full POSIX conformance |
| ISO/IEC 9899:2011 (C11) | Retain the core language baseline; document freestanding assumptions and compiler/assembly extensions | Compiler/flags and architecture matrix, undefined-behavior checks, extension inventory; separately qualify a future hosted C library |
| ISO/IEC 15408:2022 series (Common Criteria) | Security requirements and assurance structure for named profiles | Threat model, evaluation boundary, assumptions, functional/assurance requirements and evidence; formal evaluation is a separate future gate |
| ISO/IEC 25010:2023 | Product quality requirements | Measurable per-component targets for reliability, security, performance, maintainability and other applicable characteristics; tests and accepted gaps |
| ISO 9241-210:2019 | Human-centred design process | User/context research, accessible prototypes, usability tasks and observed results, iterative fixes across simple and advanced workflows |
| ISO/IEC 10646:2020 with published amendments 1:2023 and 2:2025 | International text interchange, alongside a separately pinned Unicode behavior/data version | Encoding and malformed-input tests, normalization/name policy, rendering/input coverage and security-sensitive identifier tests; character repertoire alone is not full Unicode behavior |
| ISO/IEC 27001:2022 including Amd 1:2024 | Organizational ISMS support through product capabilities | See ORG_SECURITY.md; customer-specific applicability and organizational evidence remain separate from device observations |

ISO C's newer 9899:2024 edition is a future toolchain/library assessment, not an
implicit C23 kernel migration. Do not silently update normative editions or import
standards text/source. Check applicable amendments and obtain authorized normative
text before claiming clause-level conformity. Public abstracts establish scope,
not completion of a requirements audit.

## Requirement and evidence register

For each implementation PR record: requirement ID; standard edition and exact
reference once verified; component/profile and accountable owner; applicability
rationale; behavior and failure contract; acceptance test; source/build revisions;
evidence location/date; deviations and unresolved gaps. Use stable local IDs
until normative references are reviewed; do not invent clause mappings.

Statuses: planned, implemented, tested for a named profile, formally evaluated
(where applicable). A failed, skipped, unavailable or stale check remains visible.
Certification is never inferred from alignment, a passing unit test or a badge.
Hardware-specific evidence follows ARCHITECTURE_SUPPORT.md; AMD64 results do not
qualify ARM64, RISC-V, GPU, APU or NPU profiles. Contributor coaching level never
changes the requirements. Review changes to frozen contracts with the owner.

First bounded task: create the reviewed requirement inventory for the next M4
service/audit boundary, identifying its owner, security assumptions and testable
quality targets. Keep MP1/M4/M5a priorities; add runtime implementations as separate
reviewable changes with actual execution evidence.

## Primary sources

Scope checked 2026-10-26; not a full normative-text audit:
- https://www.iso.org/standard/91911.html (POSIX)
- https://www.open-std.org/jtc1/sc22/wg14/ (C standards committee)
- https://www.iso.org/standard/72891.html (security evaluation)
- https://www.iso.org/standard/78176.html (product quality)
- https://www.iso.org/standard/77520.html (human-centred design)
- https://www.iso.org/standard/76835.html (UCS)
- https://www.iso.org/standard/83362.html and https://www.iso.org/standard/85287.html (UCS amendments)
- https://www.iso.org/standard/27001 and https://www.iso.org/standard/88435.html (ISMS and amendment)
