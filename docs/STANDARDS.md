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

## Retained POSIX and LDAP commitments

The following detailed commitments remain in force. Historical next-task wording
is superseded by the current HANDOFF and NEXT_PHASE sequence.

# Core POSIX and LDAP commitments

Owner decision: 2026-10-16T19:04:18-04:00. Status: accepted requirements;
implementation and conformance remain planned. These are core OS capabilities
across desktop and mobile, not optional third-party compatibility afterthoughts.
Core capability does not mean kernel-resident protocol code or mandatory enrollment.

## POSIX.1-2024 application environment

Target POSIX.1-2024 (Issue 8): Base Definitions, System Interfaces, and Shell and
Utilities. Build a requirement matrix separating mandatory requirements, optional
feature groups, XSI scope, implementation-defined choices, unsupported behavior,
dependencies and test evidence. Certification is a separate later decision;
partial support must never be advertised as full conformance or UNIX certification.

Provide native kernel/service primitives plus a userspace C runtime, shell and
utilities. Native JeiOS extensions coexist with POSIX. This is source portability,
not automatic Linux/macOS binary, Android app or foreign driver compatibility.
Review candidate libc/shell/utility provenance and licenses before importing code.

Contracts must cover descriptor ownership/inheritance/close, fork/exec/spawn/wait,
signals and interrupted operations, threads/synchronization, clocks, sockets,
paths/links/permissions, atomic namespace operations and durability. Map UID/GID
credentials onto explicit native authority; a compatibility UID of zero is not
unrestricted host privilege. Define bounded resources and supported failure modes.
Any security-policy restriction that changes required semantics must be disclosed
in the profile; it cannot be counted as a conformance pass.

Protected system deployments retain writable application data locations and
accurate access/read-only errors. Validate required semantics per storage backend;
Btrfs, OpenZFS, F2FS and ext4 do not gain identical guarantees by sharing a VFS.
Crypto relock follows ENCRYPTION.md: revoke access, end or reconstruct affected
processes as required, and never revive stale descriptors/credentials on unlock.
Do not promise transparent restoration of arbitrary POSIX process state.

Staging: M3 process/handle/IPC boundaries; M4/M5 service and storage prerequisites;
M6 tested initial interfaces, networking and portable tool builds; subsequent
complete-profile coverage and qualification. M6's subset is a checkpoint, not the
final compatibility commitment. Test positive and negative semantics on native
AMD64 and qualified ARM64, then reproducible real program builds and execution.
Host-only tests cannot establish native OS conformance.

First task: [core #15](https://github.com/auto44e/jeios/issues/15), profile/ADR and
conformance roadmap. Kernel M3a remains next; no stable ABI is frozen here.

## Built-in LDAPv3 directory integration

Ship an integrated directory provider and identity-broker contract. Enrollment is
optional: unmanaged local login remains fully functional without network access,
cloud accounts or a directory server. A local LDAP server is not required.
Generic LDAP support is a core commitment, superseding candidate-only wording.

Implement network parsing in an isolated userspace service after native isolation
exists. Specify request ownership, bounded BER/messages/searches/pagination/group
expansion, cancellation, deadlines, locking, connection and credential lifetimes,
provider restart and cache invalidation. Authenticate TLS peers and validate names
and certificates; reject downgrade/plaintext credential fallback. Bound referrals
and prohibit forwarding credentials to unapproved authorities. Redact diagnostics.

Use provider-qualified stable IDs with collision-safe local UID/GID mappings and
explicit schema/group policies. Lookup, authentication and authorization are
separate decisions. Group membership grants only mapped native rights and never
implicitly adds personal-data decryption or recovery recipients. Local and remote
accounts with the same displayed name must remain distinct.

Define cache expiry, bounded offline authentication, clock uncertainty, revocation,
reconnect reconciliation, leave/recovery and protected credential storage. Offline
operation cannot promise immediate remote revocation. Crypto relock must withdraw
protected cached credentials and session authority according to the declared scope.

LDAP is not complete AD DS domain join, Kerberos SSO, Windows GPO processing or
Entra/cloud enrollment. Those remain separately qualified integrations under
DOMAINS.md. No AD-controller implementation is implied. Provider/server versions,
schema and authentication mechanisms require independent compatibility evidence.

First task: [core #16](https://github.com/auto44e/jeios/issues/16), provider/identity
ADR and disposable-lab test plan. Integration requires M3-M6 services, networking,
storage and qualified TLS/entropy/key management; M9 supplies security qualification.
Test invalid certificates, hostile/truncated responses, name collisions, nested
group cycles, tenant isolation, outages, expired/revoked membership, restart and
lock/unlock. No live directory enrollment or account changes are part of planning.

## Shared mobile acceptance

Share implementations in the core repository. Mobile owns ARM64 execution and
power/suspend/offline/work-profile qualification, not a duplicate libc or directory
stack. Expose precise supported/limited/unavailable capabilities and identical
identity authority rules. Android compatibility is a separate consumer of native
services; Android UID/group conventions do not override host authorization.

## Specification references

- [POSIX.1-2024 / Issue 8](https://pubs.opengroup.org/onlinepubs/9799919799/).
- [LDAPv3 roadmap, RFC 4510](https://www.rfc-editor.org/rfc/rfc4510).
- [LDAP protocol, RFC 4511](https://www.rfc-editor.org/rfc/rfc4511).
- [LDAP authentication and security, RFC 4513](https://www.rfc-editor.org/rfc/rfc4513).

Pin precise applicable revisions and security updates during implementation review.
