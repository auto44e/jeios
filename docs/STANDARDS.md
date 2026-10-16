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
