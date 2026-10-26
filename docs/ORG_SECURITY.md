# Organizational security and ISO 27001 support

Owner decision: 2026-10-26. Planned ecosystem functionality, not implemented by
this document. Build capabilities that help organizations operate and demonstrate
their information security management system (ISMS). ISO/IEC 27001 certification
concerns an organization's scoped management system; installing JeiOS does not
certify an organization or make the OS itself ISO 27001 certified.

## Product feature backlog and acceptance

The IDs below are project requirements, not asserted ISO Annex A control mappings.
All are planned. Implementation ownership is unassigned until a task is accepted.

| ID | Capability | Acceptance and failure evidence | Dependencies |
| --- | --- | --- | --- |
| ORG-01 | Asset and software inventory: device identity, ownership, OS/firmware/package versions, support status and SBOM references | Detect stale/offline records and changed inventory; bound collection; no fabricated attestation | M4 device/services, M5 storage, M6 transport |
| ORG-02 | Directory-backed identity, hardware MFA/passkeys, scoped roles, joiner/mover/leaver and periodic access-review exports | Denied access, role change, revocation, expired credentials, bounded offline policy and audited emergency access | M3 rights, M5/M6 identity/network, existing LDAP plan |
| ORG-03 | Versioned fleet policy: enrollment, configuration baseline, effective-policy inspection, drift detection, exception expiry | Authenticated enrollment, unauthorized/expired policy rejection, conflict rules, preview/rollback, explicit unmanaged state | M4 services, M5 configuration, M6 networking |
| ORG-04 | Protected audit trail and SIEM export | Access-controlled events with actor/action/result, sequence and policy revision; tamper/loss/gap detection, queue exhaustion, clock uncertainty, offline delivery and replay tests | M4 event/service contract, M5 durable storage, M6 authenticated export |
| ORG-05 | Vulnerability, patch and software provenance management | Signed artifacts, inventory-to-advisory trace, rollout status, failed update recovery and stale-version warnings | M5 storage, M6 transport, M9 update chain |
| ORG-06 | Encryption and key lifecycle, lock/recovery and data protection policy | Document key ownership, rotation/revocation/recovery, hardware prerequisites and lock semantics; no unsupported full-crypto-lock claim | M5 storage, M9 crypto/boot and existing zero-trust plan |
| ORG-07 | Backup, restore and continuity operations | Encrypted backup, authorized restore, restore drills, measured recovery objectives, corruption and power-loss tests | M5 durable storage, M6 transport, M9 keys |
| ORG-08 | Incident response and scoped containment | Authorized isolation, evidence collection, timed emergency access, action audit and recovery; test offline devices and disrupted management connections | M4 services, M6 network, M9 hardened profiles |
| ORG-09 | Evidence and exception reporting | Export versioned machine-readable records and readable reports with scope, owner, timestamps, source revisions, observations, gaps and review/expiry dates | ORG-01–08 evidence contracts; delivery may be incremental |

Audit is not guaranteed tamper-proof merely because logs are signed. Specify the
trust boundary, key protection, external anchoring/collector assumptions, time
quality and what compromise can destroy. Establish bounded queues and explicit
loss/backpressure policy; no silent loss or unbounded blocking in kernel paths.
Store no passwords, private keys or unnecessary personal content in events.

Evidence exports must distinguish observed state from administrator assertions,
identify freshness and missing devices, and support configurable retention,
redaction, access control and deletion policy. A dashboard may show coverage and
gaps; it must not output an automatic universal "ISO compliant" verdict.

## Organizational responsibilities

The customer determines scope, interested parties, risk assessment/treatment,
Statement of Applicability, control owners and accepted exceptions. It also owns
training, personnel/physical controls, supplier management, internal audits,
management reviews and continual improvement. The 2024 amendment's contextual
considerations belong to that management process, not an invented kernel feature.
JeiOS can supply evidence and operational controls; it cannot substitute for these
organizational decisions or a certification body's assessment.

Provide configurable cross-references to the customer's Statement of Applicability
and chosen control framework after normative review. Do not assume every control
applies to every customer or publish a complete mapping from public summaries.

## Administration and privacy

Use optional explicit organizational enrollment with disclosed device/workspace
management scope. Preserve personal/work separation and least privilege; remote
wipe or containment needs scoped authority, clear impact and audit. Offer GUI,
CLI and API parity with the same enforcement. Simple onboarding explains choices
in ordinary language; expert controls expose details without bypassing policy.

Specify service ownership/lifetime, authorization, locking/caller context, bounds,
error states, rollback and hardware prerequisites in each implementation contract.
Test malicious input, stale identities, revoked rights, offline operation, full
storage, interrupted updates and recovery. Do not ship placeholder success paths.

## Delivery gates

1. ORG-G0: review ORG-04 event and ORG-09 evidence schemas alongside M4 contracts;
   define retention/privacy, authorized writers/readers, failure semantics and
   negative tests. Pass: accepted contract and traceable requirement inventory.
2. ORG-G1: implement one bounded local event/evidence path with durable-storage
   dependencies satisfied; prove access denial, loss reporting and restart behavior.
3. ORG-G2: add enrolled identity/policy and authenticated collection; prove
   revocation, stale/offline handling and cross-device isolation.
4. ORG-G3: integrate update, crypto, restore and response workflows when underlying
   M5/M6/M9 mechanisms qualify; perform an end-to-end organizational pilot.

These gates do not reorder MP1/M4/M5a or certify a release. Apply them to desktop,
mobile, server and IoT with honest per-profile availability; use the shared
[standards register](STANDARDS.md) and [architecture policy](ARCHITECTURE_SUPPORT.md).
