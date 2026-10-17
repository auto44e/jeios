# Domain membership and centralized policy

Status: proposed requirements, 2026-10-13. No directory, domain client, policy
engine or Microsoft interoperability exists yet. This extends
[IDENTITY.md](IDENTITY.md); M3a remains the next implementation gate.

## Distinct deliverables in a mixed-platform ecosystem

1. A native, optionally self-hosted JeiOS directory/domain service: users, groups,
   devices, organizational containers, delegated administration, authentication
   and centralized device/user policy. Local-only operation remains supported.
2. An optional interoperability client for joining existing AD-compatible
   environments, including Microsoft AD DS and independently tested Samba AD. Qualify authentication, directory
   lookup, machine trust and selected policy mappings independently. Native
   domain support does not imply AD wire compatibility, and AD login does not
   imply full Windows Group Policy or application compatibility.

3. Cross-platform identity and management integration for Windows, macOS and
   Linux endpoints in the same organization. Evaluate FreeIPA and LDAP/Kerberos
   realms separately from AD-compatible providers. Provide native management
   agents or supported platform adapters where needed; no blanket GPO parity.

Microsoft Entra ID/cloud enrollment, AD DS membership, generic OIDC login and
SCIM provisioning are separate integrations. Do not label them interchangeable.
An AD-compatible domain controller or forest replacement is not in the initial
scope. Cross-domain trusts require a later explicit design, never implicit trust
based on matching names or a shared network.

## Native join and leave contract

A domain is a named administrative/security authority with authenticated service
identity, directory object identifiers and versioned policy. DNS/domain labels
help discovery but cannot authenticate an authority. Verify the authority through
an approved trust bootstrap, show management scope and recovery ownership, then
use a narrowly authorized enrollment transaction. Create a unique device identity;
never clone machine secrets from installation images.

Join stages: discover -> authenticate authority -> authorize scope -> provision
machine record/key -> validate initial policy -> durably commit membership.
Bound each stage with deadlines and retry IDs. A partial join must be visibly
pending/failed, with cleanup/reconciliation of any remote object. Never advertise
joined while local keys or required policy are missing. Protect and rotate machine
credentials; distinguish stale credentials, unreachable servers and revoked trust.
Rejoining the same hostname cannot take over an existing machine record without
explicit authority and proof.

Keep person login, machine trust, tenant enrollment and data-key membership
separate. Directory groups map to native scoped rights through reviewed policy;
a remote 'administrator' group does not automatically gain every native capability.
Use issuer-qualified stable IDs, not usernames, to avoid local/domain collisions.
Bind every session and grant to its authentication and policy generations.

Graceful leave revokes membership and tokens, retires the machine record, stops
managed services, removes scoped credentials and applies a declared data-retention
policy. Preserve unrelated local accounts and personal data. Whole-device-owned
and BYOD/work-scope enrollment have different leave permissions. An authorized
offline detach records remote cleanup pending; it cannot pretend server-side
revocation happened. Provide an enrollment-time local recovery plan for broken
trust/controller loss, with auditable use and ownership-specific rules. Recovery
cannot secretly confer domain authority or personal-data decryption keys.

## Group Policy-style native configuration

Use signed, schema-versioned policy objects with explicit device and user sections.
Policies can target a domain, organizational-unit-like hierarchy, device/user
groups and approved device facts. Delegated administrators may edit only their
scope and allowed settings. Support templates for workstation, kiosk, shared lab,
server and headless appliance; advanced overrides use the same API and validation.

Each setting schema specifies type, scope, merge operator, security implications,
required capabilities, application timing, rollback behavior and offline expiry.
Distinguish recommended defaults from mandatory constraints. Recommended values
remain locally customizable. Mandatory settings apply only within the authority
accepted during enrollment. Some settings require restart/logout; saved policy
must not falsely appear active. Do not infer 'most restrictive wins' generically:
values such as routing, recovery recipients and update endpoints have no safe
universal restriction ordering.

Proposed native evaluation, to be frozen and tested in an ADR:

- Validate schema, issuer, target, signature, generation and lifetime first.
- Gather base/device-profile defaults, authorized domain/container policies from
  broad to specific, explicit group assignments and permitted local preferences.
- Use a published total order for recommended-value overrides. For mandatory
  constraints, use the setting's defined composition; reject incompatible values
  or unresolvable same-priority assignments before applying side effects.
- A delegated child cannot relax a parent's mandatory constraint unless granted
  an explicit exception authority. Exceptions are scoped, expiring and audited.
- Never combine competing device-wide authorities from unrelated tenants. Work
  scopes can coexist only where their resources/policies are disjoint or an
  owner-authorized resolver has an explicit rule.

An effective-policy view explains every value: source, inherited chain, relevant
groups, winner, rejected alternatives, enforcement broker and pending restart.
Provide a simulation/diff before rollout and the same inspection through GUI, CLI
and automation. A kiosk's device constraints remain applicable to its sessions;
user preferences do not bypass them. Separate desired, validated, applied and
healthy policy generations, with precise errors rather than one 'compliant' flag.

Apply validated snapshots through narrow subsystem brokers using prepare/commit
and compensation where possible. Retain known-good recovery policy. Security
revocations take effect at defined broker boundaries, including existing sessions
and handles; a cosmetic settings update is not enforcement. Mandatory unsupported
settings fail qualification visibly rather than being ignored. No arbitrary
privileged startup scripts by default; optional automation is signed, scoped,
resource-bounded and separately authorized under INIT.md contracts.

## Offline operation, scaling and encryption

Cache authenticated last-known-good policy and bounded offline credentials. Each
policy class declares maximum staleness and expiry behavior: continue local work,
restrict managed resources or require reauthentication. No indefinite startup
wait for a controller. Counter rollback, clock uncertainty and controller failover
need explicit freshness rules; a restored backup must not resurrect revoked grants.
At reconnect, reconcile generation and revocations before granting fresh access.
Offline devices cannot receive instant policy updates or revocations.

Use replicated directory/control services, bounded nested-group expansion, cycle
detection, paginated queries, change feeds and jittered refresh. Define authoritative
membership consistency separately from eventually consistent health reporting.
Do not expose raw directory queries to every app or leak group membership through
unscoped discovery. Native scale targets require the fleet tests in IDENTITY.md.

Protect management channels and credentials; sign policy so transport intermediaries
cannot change it. Domain administrators necessarily see directory and policy data
they manage. Personal sync content remains E2E-protected to its selected recipients;
domain join alone does not add an escrow recipient. Organization-managed recovery
is explicit. A fully privileged managed OS remains an endpoint trust boundary.
An AD interoperability mode must report its actual negotiated security: it cannot
be called post-quantum merely because native JeiOS sync uses a PQ-capable protocol.
Do not silently enable legacy authentication to make a join succeed.

## Microsoft AD interoperability qualification

Investigate the documented dependency chain: authenticated domain discovery and
DNS, time synchronization, Kerberos, directory operations, machine-account trust,
and selected GPO retrieval/interpretation including required SMB/SYSVOL behavior.
This is a protocol/adaptor workstream, not a Linux library that drops unchanged
into a fresh kernel. Licensing/provenance gates apply to any candidate component.
Publish tested server versions, functional levels, credential/encryption types,
trust arrangements, offline behavior and supported policy categories.

Begin with an explicit native mapping allowlist: login restrictions, selected
password/session policy, update policy or a similarly bounded set. Report unknown
Windows-only policy extensions as unsupported; never mark the device compliant
because a GPO was downloaded. Registry paths, Windows security identifiers and
native rights need documented translations. Imported scripts, registry settings
or Windows management binaries are not presumed executable on JeiOS.

Microsoft documents local/site/domain/organizational-unit processing plus
inheritance and filtering rules. An AD adapter claiming compatible evaluation must
test the applicable precedence, enforcement, security filtering and loopback
semantics against Windows reference behavior. The native model above is not a
claim to reproduce that ordering. Keep unsupported combinations explicit and
avoid changing native authority merely to emulate an unsafe mapping.

## Acceptance and handoff

Depends on M3-M6 isolation, IPC, persistent state and networking; M9 qualifies
credentials, signing, recovery and policy enforcement. Split native directory,
join lifecycle, policy engine and AD adapter into separate reviewable gates.

Test rogue discovery responses, name collisions, duplicate join, interrupted join,
key rotation/revocation, controller outage, offline leave, stale generations,
directory/group cycles, bounded expansion, user/device policy conflicts, delegated
scope escape, rollback after partial broker failure, mandatory unknown settings,
restored backups, clock changes, cross-tenant leakage and personal-key separation.
For AD, use a disposable lab and compare each supported mapping with its reference
behavior; no organization enrollment or live directory changes are authorized by
this design task. Next kernel work remains M3a, not enterprise service code.

## Primary references

Reviewed 2026-10-13; pin relevant protocol versions and current security requirements
when implementing, rather than importing broad compatibility claims:

- [Microsoft Group Policy processing](https://learn.microsoft.com/en-us/windows-server/identity/ad-ds/manage/group-policy/group-policy-processing).
- [Microsoft Group Policy scope](https://learn.microsoft.com/en-us/windows-server/identity/ad-ds/manage/group-policy/group-policy-scope).
- [MS-GPOL processing order](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gpol/41d4bdd2-352b-46bc-a8a8-5eba011bab52).
- [Microsoft domain join workflow](https://techcommunity.microsoft.com/blog/askds/domain-join-and-basic-troubleshooting/4405860/).

## Mixed-platform compatibility matrix and directionality

Owner clarification: interoperability must include macOS and Linux, not just
Microsoft implementations. Treat directory provider, endpoint OS, login/SSO,
resource authorization and policy enforcement as separate matrix axes.

| Direction / environment | Planned goal | Independent qualification |
| --- | --- | --- |
| JeiOS client -> Microsoft AD DS or Samba AD | Domain identity, machine trust, SSO and selected policy mappings | Test each provider/version and supported protocol behavior independently |
| JeiOS client -> FreeIPA or LDAP/Kerberos realm | Directory identities and scoped authentication/resource access | FreeIPA is not an AD clone; schema, trust and ID mapping need separate adapters |
| Windows/macOS/Linux clients -> shared existing directory | Coexist with JeiOS under common organizational identities | Test platform login, group mapping, SSO, password/key rotation and offline behavior |
| Windows/macOS/Linux clients -> native JeiOS services | Access supported identity, sync and fleet services through standards or explicit agents | Native services do not become AD-compatible controllers merely by exposing a directory |
| JeiOS fleet console -> mixed endpoint fleet | Common policy intent, inventory, deployment and health reporting | Per-platform enforcement adapters, enrollment requirements and explicit unsupported settings |

Platform-specific work includes Windows policy/management interfaces, macOS
supported SSO and device-management/configuration interfaces, and Linux directory
clients and distribution-specific enforcement. Existing SSSD/realmd or Samba
components are integration candidates for Linux, not dependencies assumed to run
inside the JeiOS kernel. Apple management eligibility and enrollment constraints
must be checked before claiming that an operation is available. Keep platform
agents narrowly scoped, signed and independently updated.

A portable policy expresses intent such as screen-lock timeout or allowed login
groups. Each adapter reports supported, semantically equivalent, partially mapped,
unsupported or failed, with actual enforcement evidence. A server must not count
an unsupported control as compliant. A Windows registry/GPO setting cannot simply
be replayed on macOS/Linux. Directory login alone does not apply cross-platform
management policy. Conversely, managing inventory does not require adopting one
shared user password or granting the console every user's data keys.

Map immutable issuer-qualified identities to platform SID/UID/GID and native
principals without collisions or automatic administrator promotion. Test group
nesting, ACL translation and service identities as well as interactive login.
Cross-realm trust has explicit direction and scope; transitive privilege is never
assumed. Preserve personal/work separation and E2E recipient boundaries across
adapters. Devices that cannot meet a PQ protocol minimum must report incompatibility
or use a separately approved, visibly weaker interoperability profile.

Acceptance expands to a versioned provider x endpoint x feature matrix, with
mixed-platform login/SSO, revocation, offline use, shared-resource authorization,
policy enforcement and enrollment/leave tests. Exact Windows/macOS releases and
Linux distributions remain open until selected and exercised in a disposable lab.
This adds architectural scope, not implemented support or live enrollment.

Additional primary references reviewed 2026-10-13:

- [Apple AD integration](https://support.apple.com/guide/deployment/integrate-mac-computers-with-active-directory-depd1a7cad1f/web).
- [Samba overview](https://devel.samba.org/samba/what_is_samba.html).
- [FreeIPA trust architecture](https://www.freeipa.org/page/Trusts).

## Core LDAP commitment — 2026-10-16

[STANDARDS.md](STANDARDS.md) promotes built-in LDAPv3 integration from an
interoperability candidate to a core requirement. Enrollment remains optional;
local accounts remain independent. LDAP directory access, authentication, native
rights mapping, AD DS membership, Kerberos SSO and cloud identity retain separate
contracts. No runtime integration is implemented. See core issue #16.
