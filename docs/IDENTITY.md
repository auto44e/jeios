# Identity, users, device trust, sync and fleet management

Status: owner requirements and proposed architecture, 2026-10-13. No account,
login, identity broker, sync engine or fleet controller is implemented. These
contracts extend the design without advancing the next kernel gate, M3a. All
cryptographic choices remain subject to [ENCRYPTION.md](ENCRYPTION.md).

## One model, progressively richer configuration

A local account must work indefinitely without a network or vendor account.
Optional enrollment adds relationships to that account; it must not replace local
process isolation with trust in a cloud session. The same typed management APIs
serve simple settings, advanced CLI/automation and a future fleet console. Reveal
complexity per area, preserve good defaults and show effective policy and its
source. Moving to an advanced UI never changes privileges.

| Profile | Default behavior | Explicit additions |
| --- | --- | --- |
| One user, one device | Local login, private home, local recovery setup, no discovery or sync | Additional authentication factors, encrypted backup |
| One person, several devices | One identity with individually approved device keys and local unlock on each | Select which files/settings/credentials sync and which devices receive them |
| Family | Separate identities and private homes, shared collections only by invitation | Guardian roles, shared device rules, recovery delegation and selected shared data |
| Shared machine | Isolated sessions, key domains and user storage; ephemeral guest option | Per-user services, quotas, concurrent seats when qualified |
| Organization/fleet | Explicit enrollment and visible management scope | Tenant policy, group provisioning, staged deployment, health monitoring and delegated administration |
| Headless appliance/IoT | Distinct workload/device identity, bounded local logs and recovery | Fleet enrollment without inventing a human shared password |

A single identity is not one reusable password/key copied everywhere. Each device
has an independent authentication key and each local account-device association
has separate key material. PINs/password verifiers and biometric templates remain
local by default; biometrics may unlock a device-bound credential after platform
qualification. Cross-device credential sync is a separately reviewed opt-in vault,
not a side effect of syncing settings. Shared accounts are not the default for
families, staff or administrators.

## Objects and authority boundaries

Stable opaque identifiers distinguish person, local principal, device, session,
tenant, group, membership, collection and enrollment. Names/email addresses are
mutable labels, not authority or canonical identities. Mapping an external account
uses its verified issuer and subject; matching an email must not merge accounts.
A single device may host several users, and a person may belong to multiple
organizations without mixing their data or policies.

Separate at least these roles: device owner, current user, family organizer,
guardian, tenant administrator, deployment operator, auditor and data custodian.
Rights are scoped to objects and operations, time-bound where appropriate, and
attenuated on delegation. Neither group membership nor device enrollment alone
grants filesystem-wide access. The kernel enforces local principal/handle rights;
identity and policy brokers translate approved relationships into scoped grants.
An identity service compromise must not mint arbitrary kernel authority.

A compromised privileged OS can inspect ordinary active sessions. Personal/work
separation on a managed device does not promise cryptographic secrecy from an
administrator authorized to replace its trusted OS. Show this boundary during
enrollment. Stronger confidentiality requires separately qualified execution and
trust arrangements; E2E transport alone does not solve endpoint control.

## Local login and sessions

The secure login UI authenticates to a narrow session broker, not to arbitrary
applications. Rate-limit attempts, use vetted password derivation and provision
recovery before enabling a lockout policy that could strand data. Lock/login,
privilege elevation and disk unlock are distinct operations with distinct state.
Create a fresh session identity and scoped handles after authentication; reject
stale tokens following logout or principal deletion. Define reauthentication for
adding devices, exporting keys and changing recovery policy.

User switching isolates clipboard, displays, audio capture, notifications,
background services and mounted private storage. Locking hides sensitive previews
and follows an explicit background-sync/key-residency policy; it cannot be claimed
to erase secrets still needed by active services. Guest sessions default to no
persistent sync credentials and are destroyed at session end subject to storage
and crash-recovery guarantees. Accessibility must work at login without granting
untrusted plugins access to passwords or the secure input path.

Personal offline login remains possible after connectivity loss. Organization
profiles define bounded offline credentials and revocation freshness; show the
resulting availability/security tradeoff before enrollment. Server revocation is
not instantaneous on an offline device. No forced vendor check-in for an unmanaged
local account. Cached network credentials cannot remain unbounded bearer tokens.

## Pairing and enrollment mechanisms

Every mechanism feeds a common state machine: discovered/invited -> peer verified
-> scope approved -> key proof -> durable membership committed -> active. Failure
before commit leaves no active member; retries use an idempotent operation ID.
Authenticate peer keys and bind intended action, roles, identities, device IDs,
nonce, protocol and scope into the transcript. Adding a device for the same person
is different from sharing with another person or enrolling a managed device.
Display which operation is being approved on the trusted endpoint.

| Mechanism | Intended use | Required protection |
| --- | --- | --- |
| QR scan | Nearby personal devices or invitations | Bind an authenticated key/handshake and action; expire one-time invitations |
| Human-compared phrase/code | Camera-free pairing | Reviewed short-authentication-string or PAKE design as appropriate, strict attempt limits; a short code is not a raw encryption key |
| Cable or NFC-assisted bootstrap | Local/headless onboarding | Treat the transport as untrusted; authenticate device and scope, require owner presence where supported |
| Existing trusted-device approval | Adding another personal device | Fresh challenge, local reauthentication, new-device key proof and visible device list |
| Remote invitation | Family/team sharing across networks | Single-use bounded invitation, authenticated recipient and inviter, explicit membership scope |
| Provisioning package / enterprise enrollment | Large-scale or offline rollout | Signed tenant policy, narrowly scoped one-use enrollment material, device key proof, expiry and revocation |
| Recovery enrollment | All normal devices unavailable | Separately provisioned owner recovery mechanism; do not silently substitute email/password reset for decryption authorization |

An invitation link is sensitive bootstrap material, not sufficient authority to
read all existing data. Avoid secrets in logs, analytics, referrers or reusable
URLs. No automatic trust based on proximity, matching SSIDs or device names.
No reusable fleet-wide private key in a golden OS image. Cloned images generate
fresh device keys using qualified entropy on first enrollment. Hardware-backed
attestation is optional and platform-specific, and must not be a vendor lock-in
requirement for personal accounts.

## E2E sync and group membership

The synchronization service stores and relays ciphertext for personal data.
Only authorized endpoints and deliberately authorized recovery recipients obtain
content keys. Use collection-level membership: private, selected-person/family,
and organization data have different recipient sets and retention policies.
Encrypt sensitive filenames, settings and manifests as well as file contents where
feasible; servers still learn some routing, size, timing and membership metadata.
Document that leakage and minimize retention. Local-only and self-hosted service
modes are first-class; internet sync is optional, not needed for nearby transfers.

Each collection has versioned membership and key epochs. Authenticate changes;
reject stale/replayed grants, unauthorized additions and server key substitution.
Clients maintain membership history/checkpoints and require explicit verification
for unexpected keys. Design split-view/equivocation detection and offline join
resolution before production: a server signature alone is not proof every device
saw the same membership. Use a reviewed group key protocol; study MLS semantics,
but do not claim base MLS automatically satisfies the PQ requirement. Pin a reviewed
PQ-capable full protocol and library, including authentication and recovery.

On removal, stop distributing future keys to the device/member and advance epochs
for future data. Removal cannot erase already decrypted copies, old keys or
screenshots. Adding a member grants only the selected history; rotating a key must
not accidentally expose historical content. Recovery grants are also recipients:
show who can decrypt, rotate when they change, and test lost/stolen device paths.
Password reset restores authentication only unless a previously authorized recovery
mechanism can also unlock encryption keys. Without keys/recovery, some data must
remain unrecoverable rather than having a hidden administrative backdoor.

Sync an allowlisted set of portable settings by default after opt-in. Never copy
raw machine permissions, device enrollment, executable trust decisions, hardware
paths or secrets through ordinary preference sync. File conflicts preserve both
versions until resolved; do not use last-writer-wins for authority or key state.
Signed logical generations, bounded tombstones and explicit conflict rules handle
clock skew, offline edits and interrupted transfers. Expired offline replicas may
need a full resync; they cannot silently resurrect deleted users/data. Reuse the
bounded transfer/durability contracts in ENCRYPTION.md. Sync is not a backup.

## Families, organizations and data visibility

Family membership does not expose private files or encryption keys. Shared albums,
folders and calendars have explicit membership. Guardian authority is a distinct,
visible policy over specified child accounts/devices, with documented controls,
recovery scope and transition/removal rules. Do not silently model every family
member as an enterprise employee or make every organizer a data custodian.

For managed work collections, an organization may require declared recovery or
archival recipients. Such recipients are cryptographic endpoints, so do not label
the data inaccessible to the organization. Personal collections have no tenant
escrow by default. BYOD enrollment defaults to a work scope; enterprise-owned
full-device control is a separate consent/provisioning mode. Tenant uninstall or
selective wipe must not remove unrelated personal data. Whole-device wipe requires
a separate right and policy. Policy conflicts between tenants fail visibly, never
resolve by unioning administrator permissions across tenants.

## Fleet deployment and monitoring

Separate identity/provisioning, device policy, deployment, telemetry and data sync
services with independently scoped credentials. Evaluate OIDC for federated human
authentication and SCIM for provisioning users/groups through isolated adapters.
These are interoperability candidates, not device-management or E2E-encryption
protocols, and no compatibility is implemented. Preserve native local accounts
when federation is unavailable under the configured offline policy.

Controller state is desired configuration; device reports are observed state with
generation, timestamps and freshness. Use signed, target-bound, expiring commands
with operation IDs, acknowledgements and replay protection. Model queued, accepted,
running, succeeded, failed, expired and outcome-unknown distinctly. Remote wipe or
reboot cannot be claimed complete just because queued. Validate policy locally;
a relay compromise cannot turn an inventory request into arbitrary code execution.

Deploy signed images/packages in canary rings with health gates, maintenance
windows, bandwidth/CPU budgets, interruption-safe resume and pause/rollback rules.
Avoid a simultaneous fleet reboot on reconnect: use jitter, bounded queues and
backpressure. Partition control-plane storage by tenant/device, permit stateless
front ends and replicated durable command state, and define consistency for policy
updates and membership separately from eventually consistent health summaries.
No numeric fleet-scale claim until measured with a published workload, device
count, failure rate, reconnect storm and controller-loss tests.

Default monitoring includes only explicitly enrolled inventory, version, update
and coarse health fields. No personal file contents, browsing history, screenshots,
keystrokes or location by default. Publish field-level schema, retention, access
roles and collection reason. Encrypt reports to designated management collectors;
those collectors necessarily see the reports they process. This is distinct from
personal E2E content that controllers cannot decrypt. Audit privileged changes
with actor/target/policy generation, redact secrets and resist tampering through
independent append-only receipt/storage design; a local log alone is insufficient.

High-impact fleet operations support staged approval and optional two-person
control. A central outage must not disable unrelated local work. Tenant removal,
owner transfer and disposal revoke credentials, clear authorized key material and
record any pending remote action; do not transfer a previous owner's account keys.
Firmware/storage erasure limits remain explicit. Remote revocation and remote wipe
cannot guarantee deletion from offline or attacker-controlled endpoints.

## Implementation contracts and qualification

Broker-owned identity records are immutable versioned snapshots. Serialize a
membership transition per collection/tenant authority, compare expected generation
before commit, and make restart/retry idempotent. Never hold kernel locks across
network IPC. Bound principals, memberships, messages, pending approvals, device
records, sync storage and event history with quotas and backpressure. A key handle
is non-exportable to ordinary callers; copy-out and diagnostics cannot serialize
key material. Persist revocation/epoch changes before acknowledging durable success;
restore from backup must not reuse nonces or resurrect revoked authority.

M3/M4 prove local principals, isolated sessions, handles and brokers. M5 defines
durable identity/key records and crash recovery. M6 prototypes local pairing and
sync with explicit non-production status. M7 provides equivalent UI/CLI workflows.
M9 qualifies authentication, recovery, E2E/PQ protocols and managed policy. Later
ecosystem work adds family sharing and measured fleet scale after those gates.

Required tests: cross-user/tenant denial; stale sessions; external issuer collision;
MFA/recovery changes; offline login expiry; spoofed/replayed invites; interrupted
join; device cloning; group removal concurrent with offline edits; server key
substitution; lost-all-devices recovery; forbidden historical-key disclosure;
selective versus full wipe; policy conflicts; sensitive telemetry redaction;
controller loss; command replay/expiry; corrupted durable state; reconnect storms;
malicious peers and resource exhaustion. Test local-only setup without a network.

Open ADRs: principal/session ABI; credential/key vault and recovery protocol;
PQ group protocol and membership consistency; enrollment profile; federation
adapters; fleet policy precedence and telemetry schemas; numeric resource limits.
No algorithm/library import precedes license/provenance and security review.

## Primary references

Reviewed 2026-10-13 as protocol design inputs, not implementation claims:

- [OpenID Connect Core](https://openid.net/specs/openid-connect-core-1_0-errata2.html): federated authentication and claims.
- [SCIM RFC 7644](https://www.rfc-editor.org/info/rfc7644/): identity provisioning protocol; review updates/errata when selecting an adapter.
- [MLS RFC 9420](https://www.rfc-editor.org/info/rfc9420/): group key management reference; PQ qualification is separate.

## Domain membership and centralized policy

[DOMAINS.md](DOMAINS.md) makes native domain joining, directory identities,
organizational hierarchy and Group Policy-style management explicit. Joining
Microsoft AD DS is a separately qualified interoperability target; OIDC login,
cloud enrollment and native policy do not imply AD/GPO compatibility.

Domain interoperability includes mixed Windows/macOS/Linux endpoints, Microsoft
and Samba AD providers, and separately qualified FreeIPA/LDAP/Kerberos adapters.
See DOMAINS.md for directionality and per-platform policy enforcement limits.

## Hardware authenticators

[SECURITY_KEYS.md](SECURITY_KEYS.md) makes hardware-key enrollment, MFA,
reauthentication and recovery first-class native services. User presence alone
is not user verification; account authentication alone does not recover data keys.
