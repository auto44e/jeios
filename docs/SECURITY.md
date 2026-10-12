# Security and privacy design requirements

**This remains an experimental kernel, not a hardened OS.** M1/M2 provides
read-only kernel text/rodata, NX data on qualified CPUs, guarded kernel stacks,
validated boot input and bounded allocation/rollback. Kernel faults stop the
machine with diagnostics. All threads run at CPL0 in one address space.
There is no userspace authority boundary, secure boot, encryption, randomness
service or networking. Early bootstrap maps are temporarily RWX until replaced.
The following threat model remains a requirement for future implementation.

## Assets, adversaries, and boundaries

Protect user content/credentials, application isolation, device ownership, update
integrity, service availability, and privacy of local diagnostics. Consider
hostile applications, malformed inputs, buggy drivers, malicious peripherals,
stolen devices, compromised package sources, and accidental misconfiguration.
No promise is made against all compromised firmware/hardware, invasive physical
attacks, every side channel, or unrestricted authorized privileged code.

| Boundary | Planned enforcement | Failure behavior |
| --- | --- | --- |
| App -> kernel | Checked syscall data, typed generation handles, rights and quotas | Denied/invalid without side effects |
| App -> broker | Scoped capability transfer, request bounds, broker identity | Reject/revoke with explicit status |
| Driver -> memory | Process boundary plus qualified IOMMU/IRQ routing | Disable/unavailable if required containment cannot hold |
| Service -> service | Explicit channels, rights attenuation, generation binding | Old requests cannot reach restarted instance |
| Installed image -> boot | Owner trust roots, signed images, recovery/rollback policy | Refuse unauthorized image under locked policy |
| Theme/widget -> system | Data themes; isolated code; trustworthy consent UI | Revert broken customization, retain authorized apps where possible |
| Diagnostic -> export | Local preview, redaction, user-initiated transfer | No automatic external report |

## Required defaults

No mandatory cloud login. No advertising ID or default telemetry. Network access
is policy-controlled and independently revocable. Files, microphone/camera,
location/sensors, clipboard, screen capture and raw devices are mediated. Log
retention is bounded and sensitive fields are redacted by default. Stock apps
use public broker interfaces rather than broad ambient privilege.

Installed executable identity must bind package provenance and sandbox identity.
Grant changes do not apply invisibly to a different package or restarted service.
For network revocation define existing socket teardown, DNS/proxy mediation,
child processes, shared services, IPv6, and VPN reconnect behavior. Encrypting DNS
or routing through a VPN is not a promise of anonymity.

## C hardening work

Add validated user-copy and trap containment, W^X where possible, guarded stacks,
stack protection, hardened allocation, zero-before-share, checked arithmetic,
resource accounting, and reliable entropy. PAC/BTI/CET/MTE are additive mitigations
with architecture prerequisites. Tagging detects some faults; it cannot replace
rights checks. Optional CPU instructions must not leak into baseline execution.
Fuzz untrusted parsers and fault-inject allocation/timeout/partial initialization.
Never recover arbitrary kernel corruption by simply returning an error.

## Security profiles are not difficulty presets

Keep compatibility tier, boot trust state, policy guarantees, and UI complexity
separate. An expert UI does not authorize privileged actions. Legacy hardware
may run but cannot claim guarantees such as NX-enforced data non-execution or
IOMMU-enforced device containment. Development trust permits deliberately changed
code and must show which boot/module guarantees are lost. User freedom and honest
security status coexist; neither requires pretending unsigned kernel code is safe.

## Boot, updates, and encryption

A future owner-configurable trust root is required; no hidden universal vendor
key. Verified boot, anti-rollback counters, A/B images, signed manifests, and
recovery must be coordinated. A known-good image can still be vulnerable; signed
recovery policy must distinguish authorized rollback from downgrade attacks.
Private signing keys never enter the repo. The GRUB prototype has no trust-chain enforcement.

Use vetted cryptography. Design key derivation/storage/unlock, memory lifetime,
physical access, recovery and backup before adding an 'encrypted' UI label. Mobile
bootloader relocking and secure key storage depend on the specific device; broad
Android support is not inherited from GrapheneOS or CalyxOS ideas.

## Inspiration and provenance

GrapheneOS motivates attack-surface reduction, memory hardening, scoped access,
and carefully limited security exceptions. CalyxOS motivates approachable privacy
controls and application network policy. These are reference designs, not code
imports, endorsements, or equivalent implementations. See REFERENCES.md.

## Vulnerability handling

Before production use, establish a tested private disclosure channel, triage
owners, patch distribution, supported-version policy, and a recovery process.
Do not use a public issue to post sensitive exploit details or credentials. No
private disclosure address is configured by this scaffold; do not invent one.
