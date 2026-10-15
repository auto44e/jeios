# Ecosystem-wide zero-trust framework

Owner requirement: 2026-10-14. Status: accepted product direction; proposed
engineering contracts below, not implemented enforcement or a frozen API.
Applies to desktop, mobile, IoT, native apps, compatibility runtimes, services,
peripherals, accelerators, nearby sharing, sync and managed/federated deployments.

## Meaning and trust limits

No implicit authority from being local, first-party, on the same network, paired,
signed, domain-joined or logged in. Authenticate the relevant identity and
authorize the requested operation against an explicit resource, scope and policy.
Assume a compromised component and limit its reach through isolation, least
privilege, bounded work and revocation. Authentication is not authorization.

The kernel and selected privileged mechanisms remain a trusted computing base.
Current M1/M2 workers are trusted CPL0 code sharing one address space: no app
isolation, capability enforcement or ecosystem zero-trust runtime exists.
Compromised kernel/firmware, invasive physical attacks and all side channels are
not solved by this framework. Publish each device profile's actual guarantees;
unsupported required isolation must refuse activation, not silently downgrade.

## Enforcement contracts

| Boundary | Proposed enforcement | Required denial/failure evidence |
| --- | --- | --- |
| App to kernel | Private address space, checked copies, typed generation handles, per-operation rights | Cross-process reads/writes, forged/stale/wrong-type handles and overflow rejected; survivor progresses |
| App/service to broker | Kernel-derived caller identity, explicit resource grant and bounded request | Caller-supplied identity cannot impersonate; forwarding cannot launder authority |
| Service to service | Scoped channels, separate identities, attenuation and bounded delegation | No rights amplification; restart cannot inherit an old instance's grants |
| Driver/accelerator to memory | Minimal buffer/range grants, qualified DMA isolation and device lifecycle | Revoke/quiesce before reuse; timeout cannot free memory still accessed by DMA |
| Device to device | Explicit enrollment, authenticated encrypted channel, resource-level authorization | Pairing is not universal access; wrong peer, replay and expired grant denied |
| Administrator to user data | Distinct management and decryption authority, visible enrollment policy | Domain membership does not silently grant personal E2E keys |
| Foreign runtime to native service | Native identities and broker checks for every mapped permission | Android root/UID or a compatibility permission cannot bypass host policy |
| Package/update to execution | Provenance, scoped install rights and independently qualified boot/update trust | Signing alone does not authorize camera, files, network or administrator access |

## Policy and capability architecture

Keep identity, policy decisions and enforcement distinct. Kernel mechanisms
enforce local object rights; userspace brokers enforce resource semantics.
Do not put network calls, a cloud dependency or a general policy interpreter
on every syscall. A previously granted capability may authorize operations within
its checked scope; continuous evaluation means rechecking relevant conditions and
defined revocation events, not asking for MFA on every operation.

Before implementing an interface, specify subject identity (principal, process,
package, service instance and device where relevant), object identity/generation,
action, scope, issuer, delegation ceiling, expiry policy, policy version and
reason codes. Bind identities to trusted evidence, not caller assertions.
Never send a local numeric handle over a network as if it were a portable credential.
Remote grant formats and cryptographic protocols need separate reviewed ADRs.

Deny undeclared access by default. Separate discovery from access. Broker defaults
grant the smallest useful resource scope, with explicit consent or managed policy.
Delegation may narrow rights, scope or duration; it may not create authority.
Customization and expert UI modes do not change security checks. Provide readable
effective-policy explanations without exposing secrets. Fleet policy precedence,
owner overrides and emergency recovery need explicit rules and audit evidence.

## Revocation, offline use and recovery

Specify revocation for new operations, existing sessions, queued/in-flight work,
open files/sockets, delegated grants and shared buffers. Define the point at which
a decision becomes effective and whether existing work completes or is cancelled.
Closing a handle alone must not be claimed to revoke every duplicate or child grant.
Race-safe revocation needs generations/epochs or another reviewed lifecycle design.

Cross-device revocation cannot be instantaneous while a peer is disconnected.
Define bounded offline grants, expiry and synchronization conflict behavior before
release. Secure time/rollback resistance prerequisites must be explicit; if they
cannot be upheld, refuse the guarantee or identify the weaker development profile.
Expiry cannot erase plaintext already received or revoke knowledge already copied.

Local-only operation remains supported without a cloud account or mandatory
remote attestation. Define locally verifiable policy and recovery paths. Loss of a
policy service must not grant new privileges; continued authorized offline work
must follow an explicit bounded policy. Recovery is a separately authorized path,
not an undocumented universal bypass. Hardware MFA and attestation provide inputs
when qualified; neither proves a device is uncompromised.

## Ecosystem integration and conservative defaults

Apply the same authority model to files/raw disk ranges, network destinations,
microphone/camera/sensors, clipboard/screen capture, input injection, credentials,
sync collections and management commands. Native stock apps receive no blanket
exemption. Touch/secure prompts must preserve trusted consent routing.

GPU/NPU offloading inherits the initiating workload's authority and data scope.
Resource availability never authorizes broader memory access or remote transfer.
Remote device/cloud execution requires separately authorized destinations and
data disclosure. FFmpeg, HandBrake, BRAW parsers and imported runtimes remain
bounded userspace integration targets with per-component provenance review.

Use open documented protocols where suitable; exact standards and libraries
require a reviewed selection and compatibility matrix. No custom cryptography.
Audit decisions, grant changes and recovery with bounded local retention,
redacted fields and explicit export; no default surveillance or telemetry.
Policy evaluation and logs need quotas so denial floods cannot exhaust the OS.

## Delivery gates and acceptance

1. M3a: enforce address-space and syscall boundaries; contain adversarial user
   faults and protect architectural state. This remains the immediate task.
2. M3b: typed scoped handles, lifetime, attenuation and stale-right rejection.
   Specify revocation semantics without claiming immediate distributed revocation.
3. M3c: bounded IPC, authenticated local caller provenance, capability transfer,
   cancellation and service-instance separation.
4. M4-M6: brokers, drivers, storage and network enforcement; test confused-deputy
   forwarding, quotas, detach/restart and grant-revocation races.
5. M7-M9 and identity/domain gates: trustworthy consent, key lifecycle, enrollment,
   policy composition, signed update/recovery and independently tested offline rules.
6. Mobile MP3/MP6/SEC and Android gates: qualify equivalent boundaries on ARM64
   and each supported device/runtime; AMD64 success does not qualify mobile.
7. Ecosystem qualification: test compromised paired peers, replay, membership
   removal, stale policy, network partition, clock rollback, recovery and downgrade.

Each gate records exact implementation commit, platform/configuration, commands,
observed positive and negative results, remaining trusted components and limits.
No gate is complete because a design, API stub, signed binary or dashboard exists.

## Usability and owner-controlled security spectrum

Owner clarification: 2026-10-14T19:55:55-04:00. Security throughout the OS and
ecosystem must be seamless and intuitive for everyday users while exposing
granular, customizable controls to power users, developers and organizations.
Support a spectrum from deliberately permissive experimentation ("YOLO") through
secure everyday operation to tightly restricted, high-assurance deployments.
The spectrum is an accepted product goal; names and exact settings below are
proposals, not an implemented UI or approved security certification.

### Independent controls

Keep three dimensions distinct: interface detail, actual security policy, and
hardware/boot assurance. A simple interface can operate a strict device; an expert
interface can inspect a secure system without weakening it. Security profiles
intentionally change policy only through an authorized operation, never as a side
effect of showing more settings. Hardware guarantees cannot be created by a preset.

| Proposed profile | Intended behavior |
| --- | --- |
| Everyday (default) | Strong practical defaults, automatic routine protections, understandable consent at meaningful boundaries, easy permission review |
| Developer | Scoped debugging, local builds, test identities and per-workload exceptions; retain unrelated protections |
| Permissive / experimental ("YOLO") | Owner-authorized broad grants and selected disabled protections, including development boot/kernel policy where supported; explicit loss of assurance |
| Hardened | Tighter application/device admission, shorter grants and stronger authentication/recovery policy appropriate to the threat model |
| Restricted / organization-defined | Custom policy beyond stock presets, potentially allowlisted apps/peripherals, offline operation, restricted networking and dual-control administration |

These are starting configurations, not a single universal security score or a
claim that every setting forms a total ordering. Allow per-app, service, device,
user, workspace and resource overrides where the implementation can enforce them.
For example, keep personal keys and normal apps protected while opening a test
workspace for development. Where changing kernel/boot trust affects the whole
machine, explicitly report that isolation cannot preserve the previous assurance.

### Everyday experience and expert tools

Use plain-language descriptions of what an app wants, why and for how long.
Prefer scoped pickers and brokered actions over broad grants. Avoid repeated
prompts for already authorized routine work; prompt when authority meaningfully
expands or sensitive policy requires reauthentication. Do not make dismissing
security dialogs the normal workflow. Support accessible touch, keyboard and
assistive interfaces, with concise defaults and expandable technical details.

Experts need equivalent documented GUI, CLI and policy API control, searchable
settings, versioned policy export/import, dry-run validation, effective-policy
inspection and explanations of denied operations. Report the source of each
restriction and how an authorized operator can change it. Policy imports are
privileged changes and must be validated; an app cannot self-select a permissive
profile. Keep policy schema and numeric defaults in reviewed ADRs before freezing.

### Deliberate weakening and restoration

An authorized personal-device owner may deliberately relax supported protections.
Record the exact scope, affected guarantees, duration and restart/reboot needs.
Offer temporary exceptions and a reliable return path; persistent exceptions are
allowed when explicitly selected. Confirmation should be proportional to impact,
not a blanket extra prompt on every action. Show persistent, unobtrusive status
when assurance is reduced, with details available on demand.

Permissive policy can grant broad authority, but does not mean accepting malformed
kernel pointers, memory corruption or broken lifetime rules. Mechanism correctness
remains required. Disabling a boundary removes the corresponding security claim;
do not continue advertising zero-trust enforcement for that boundary. A permissive
peer never automatically weakens another device or inherits its credentials.

Returning to a hardened preset does not undo prior compromise, data disclosure,
key exposure or rollback. Where required, restoration must include trusted boot,
reinstallation, re-enrollment or credential rotation before assurance is restored.
Define these transitions per profile and device, including recovery from lockout.

### Managed ownership and qualification

On an unmanaged personal device, the owner controls supported profiles. On managed
devices/workspaces, identify who controls each policy and show enrollment terms,
locked settings and effective policy. An ordinary user cannot override an
organization's authorized policy floor. Personal versus corporate ownership,
unenrollment, emergency access and destructive reset implications need explicit
contracts; no hidden administrator authority or universal bypass.

Test both usability and enforcement: everyday tasks without prompt storms,
accessible explanations, GUI/CLI/API equivalence, scoped exceptions, policy
precedence, denied unauthorized changes, profile transitions, offline recovery,
cross-device mismatch and inability to claim unavailable hardware assurance.
No preset is "absolutely secure"; qualify actual controls and residual risks.

## Related designs

[Security](SECURITY.md), [M3 proposal](M3A.md), [Identity](IDENTITY.md),
[Domains](DOMAINS.md), [Encryption](ENCRYPTION.md), [Storage](STORAGE.md),
[Accelerators](ACCELERATORS.md), [Security keys](SECURITY_KEYS.md),
[Input](INPUT.md), [Mobile](MOBILE.md) and [Roadmap](ROADMAP.md).
