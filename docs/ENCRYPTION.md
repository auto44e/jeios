# Encryption, protected memory and nearby sharing

Status: requirements, 2026-10-13. No cryptography, secure entropy, encrypted
storage, memory-encryption driver or sharing protocol is implemented. These
requirements span M3-M9 and platform qualification; they do not claim present
protection. Use audited implementations after license/provenance approval.

## Separate the guarantees

| Threat / boundary | Required mechanism | Limit |
| --- | --- | --- |
| Stolen powered-off storage | Encrypted content, key protection and authenticated boot policy | Unlocked endpoints expose data; passwords need a vetted memory-hard KDF |
| Passive recording or hostile network peer | Authenticated end-to-end protocol with post-quantum protection | Traffic timing/size and compromised endpoints remain visible |
| Physical DRAM/bus observation | Qualified hardware memory encryption | Not equivalent to isolation from the running kernel; platform-specific coverage |
| Malicious app or peripheral | Address spaces, scoped handles, IOMMU and narrow buffer grants | Encryption alone does not enforce access rights |
| Host/hypervisor inspection of a workload | Qualified confidential-computing boundary | Requires platform/monitor support; not blanket protection against physical attacks or side channels |

Ordinary computation needs plaintext in the executing CPU's trusted context.
Encrypting an idle buffer reduces exposure only if keys and plaintext copies are
outside the attacker's reach. An arbitrary reader of all kernel/application RAM
can usually obtain both. Do not advertise universal encrypted computation or
post-quantum RAM protection. Hardware DRAM encryption uses its own symmetric
algorithms and key sizes; a PQ network handshake does not upgrade that hardware.

Expose independent capability indicators for storage encryption, transport suite,
physical-memory coverage, DMA containment, boot trust and confidential execution.
Older AMD64 hardware may retain strong software storage/transport encryption
while lacking RAM encryption. Report that absence; refuse a workload explicitly
requiring unavailable protection. Never silently downgrade a negotiated minimum.

## Cryptographic policy and key lifecycle

NIST FIPS 203 defines ML-KEM; FIPS 204/205 define ML-DSA/SLH-DSA signatures.
A KEM establishes shared secrets; it does not directly encrypt files. Target a
reviewed hybrid classical plus ML-KEM protocol with authenticated suite negotiation
and transcript binding. Authentication needs its own PQ migration strategy:
classical-only identity signatures do not provide full PQ authentication. Select
and pin a reviewed protocol version and library at implementation time, with
interop vectors; do not invent a combiner or wire protocol from these primitives.

Use a vetted symmetric construction with a conservative 256-bit key policy where
supported. Payload AEAD, disk-sector encryption, password derivation and hardware
memory encryption have different contracts. Disk confidentiality does not imply
integrity or rollback resistance; unkeyed filesystem checksums are not MACs.
Document nonce uniqueness, key separation, usage limits and failure behavior per
construction. No ad hoc fallback or algorithm negotiation based only on speed.

Key creation waits for a qualified CSPRNG; early-boot timers/CPUID are not enough.
Separate device identity, volume, user, session and transfer keys. Define wrapping,
rotation, revocation, recovery and deletion before persistent formats stabilize.
Hardware sealing is optional per platform and binds a documented recovery/update
policy; a broken update must not permanently strand owner data. Recovery secrets
are owner-held, never a mandatory vendor escrow or a committed repository file.

Secret buffers have one accountable owner, bounded lifetime, explicit zeroization,
no ordinary dumps, no unencrypted swap and no unreviewed debug serialization.
Zeroization must resist compiler removal; it cannot erase historical snapshots or
all CPU/device copies. Minimize copies and deny sharing unless explicitly granted.
Sleep/hibernation must define which keys remain resident and how resume validates
state. Suspend-to-RAM is not equivalent to powered-off protection. Crash dumps
are disabled for secrets or encrypted under a separately controlled recovery key.

DMA buffers and GPU/accelerator memory are separate exposure domains. Use IOMMU
containment and minimal bounce buffers where devices cannot access encrypted RAM;
clear them after completion, cancellation and reset. Zero-copy is allowed only
when its grant and confidentiality contract hold. Hardware memory encryption must
be qualified against exact firmware, processor model, shared-page transitions,
reset behavior and current vendor advisories, not a feature-bit check alone.

## Nearby device sharing (working feature name, no Apple interoperability claim)

Default off; when invoked, discovery is time-limited and local. No mandatory
account, cloud relay, contacts upload, permanent broadcast device ID or automatic
receiver acceptance. Rotate discovery identifiers; reveal names/previews only as
policy permits after authentication. Nearby radio presence is not proof of trust.
LAN/Wi-Fi and future USB transports share the security contract; Bluetooth discovery
does not require the bulk transfer to use Bluetooth. Radio/address metadata may
still be observable, so do not promise anonymity.

Pair using an authenticated QR exchange or a reviewed short-authentication-string
protocol with strict attempt limits. A display name is never peer identity.
Bind both endpoints, protocol version, suite, ephemeral keys and roles into the
handshake transcript. Require explicit recipient approval before accepting files.
Previously paired peers remain revocable; pairing does not imply unrestricted
file access or permanent discoverability. User trust UI stays outside the sender's
control. Downgrade and identity-change warnings cannot be bypassed by a theme.

Use fresh session keys and reviewed forward-secrecy/rekey semantics, with key
erasure after transfer. Define PQ forward-secrecy claims for the selected full
protocol, not merely the presence of ML-KEM. Bound offers, filenames, item count,
metadata and total bytes before allocation. Receive into a quota-limited staging
area via scoped handles; reject traversal, symlink escapes and special files.
Do not auto-run applications or overwrite an existing destination silently.

Authenticate chunk position, transfer/file identity, lengths and final manifest.
Resume tokens must be peer-bound, expire, and avoid key/nonce reuse after restart.
Cancellation revokes grants and erases transient keys; disclose if data already
arrived at the recipient. Commit only after integrity verification and explicit
durability results. A successful network ACK alone does not mean durable storage.
Scan/preview handlers run sandboxed. Files retain untrusted-origin metadata where
the destination format supports it. Compression must not mix secrets with
attacker-controlled content in a way that introduces an avoidable oracle.

## Delivery and acceptance gates

M3/M4: isolated processes, checked buffers, revocation and scoped brokers.
M5: persistent formats, crash-safe writes and key-lifecycle design before format
freeze. M6: networking and transfer prototype with explicit experimental status.
M9: qualified entropy, reviewed crypto, authenticated boot/update policy, encrypted
storage and audited sharing. P2 qualifies confidential workloads independently.

Test known-answer/interop vectors; malformed keys/messages; replay/downgrade/MITM;
revoked peers; exhausted quotas; transfer interruption/resume; nonce reuse across
crashes; malicious filenames; locked-device behavior; debug/dump/swap leakage;
key rotation/recovery; power loss and hardware absent/degraded states. Treat parsing
and cryptographic failures as generic peer failures without secret-bearing logs.
No production claim until independent review and named platform evidence exist.

## Primary references

- [NIST finalized PQ standards](https://csrc.nist.gov/News/2024/postquantum-cryptography-fips-approved).
- [AMD SME/SEV overview](https://www.amd.com/en/developer/sev.html).
- [AMD physical-memory fault injection advisory](https://www.amd.com/en/resources/product-security/bulletin/amd-sb-3048.html): example of why platform qualification has explicit limits.
- [Linux fscrypt threat model](https://docs.kernel.org/filesystems/fscrypt.html): reference for encrypted-at-rest boundaries, not a code import.

References reviewed 2026-10-13; standards errata and platform advisories must be
rechecked when an implementation and supported hardware are selected.

## Identity, group sync and managed endpoints

[IDENTITY.md](IDENTITY.md) extends pairing to local and remote device enrollment,
family/organization membership, collection key epochs and recovery. Authentication
reset does not automatically recover encrypted data. Personal sync relays do not
receive content keys; organization recovery recipients and management telemetry
collectors are explicitly named endpoints for their respective data. A privileged
managed OS can inspect ordinary active sessions, so E2E is not a promise against
every device administrator. Revocation cannot erase previously delivered plaintext.

## Cryptographic relock on ordinary device lock

Owner requirement: 2026-10-14T20:08:17-04:00. A normal lock action should be able
to restore pre-first-unlock-like protection for selected user data without requiring
a power cycle. Applies across device types and to manual lock, idle timeout,
lid-close, remote authorized lock and other configured session-lock triggers.
Exact triggers/defaults and platform feasibility require ADRs and tests.

Data remains encrypted at rest while unlocked; the change is withdrawal of usable
keys and plaintext access, not re-encrypting the entire disk each time. Distinguish
screen/UI lock, cryptographic relock of a declared protection domain, and a real
reboot. Relock cannot reset every privileged component, undo compromise or provide
universal forensic equivalence to powered-off or freshly booted hardware.

### Proposed lock policies

- Ordinary session lock: hides interaction; explicitly identify any keys/data still
  accessible to background services. Do not label this cryptographic relock.
- Cryptographic relock: ordinary lock also evicts the selected domain's usable
  keys and clears or seals its plaintext state, restoring a defined credential-gated
  state. Offer this as an integrated everyday lock option, not a special reboot.
- Strict credential relock: same protection with fresh PIN/passphrase or a separately
  qualified strong credential required; disable biometric-only and trusted-device
  auto-unlock paths for this profile. A convenience biometric option, if qualified,
  has a separately stated key-release policy and must not imply identical guarantees.

Default selection must balance usable workflows with demonstrated protection.
Keep configuration understandable: explain background functionality lost and data
still available. Power users and organizations can scope profiles per user/workspace
where real key/process isolation permits. A session holding another user's data
must not retain it across that user's relock merely because its own session is open.

### Transition contract before implementation

Define states unlocked -> relocking -> cryptographically locked -> unlocking.
Immediately secure the UI and block new domain grants. In bounded time, revoke
access, stop/quiesce affected workloads and device submissions, durably commit
approved writes, then remove keys from every usable key slot/cache and clear
plaintext across apps, brokers, filesystem/page caches, shared mappings, clipboard,
previews, swap/dumps, IPC and GPU/NPU/device memory. Invalidate old handles and
pending completions so races cannot reopen the domain.

Suspending a process with readable plaintext in RAM is insufficient. Terminate it
or checkpoint it using an independently reviewed encrypted format whose decrypting
key is unavailable until the next authorized unlock. Resumption and application
state restoration are goals requiring tested implementations, not assumed free
side effects. Noncooperative apps must not indefinitely prevent lock; define bounded
termination, unsaved-work policy and clear recovery behavior without silently
discarding data or leaving the domain accessible.

Only report successful cryptographic relock after the declared postconditions hold.
If writes, DMA quiescence, memory sanitization or key eviction fail, keep the UI
locked, report protection as incomplete and follow an explicit tested fallback.
Where shutdown/reboot is needed, say so; never certify success based on a screen
animation. The exact durability/fallback policy must be resolved before release.

Key hierarchy must separate boot-available services from credential-gated user
domains. No resident master/wrapping key, recovery agent or cached authentication
token may silently reconstruct a supposedly evicted domain key. Credential attempts
require reviewed rate limiting and hardware protection where qualified; short PIN
security cannot be assumed on platforms lacking those protections.

### Background work and practical limits

Alarms, emergency functions, calls and minimal notifications may need distinct
boot-available services/data. Specify their narrow data classes, keys and metadata
exposure. Sensitive notification previews, sync, search, media playback, downloads
and remote workloads pause unless a user-authorized exception explicitly retains
the required data in a separate domain. Exceptions reduce coverage; display them.
An app cannot silently copy protected content into a weaker class for convenience.

Remote peers, already delivered plaintext, peripheral firmware and historical memory
copies cannot be erased by local relock. Powered RAM remanence, firmware, secure
hardware, storage/controller caches and compromised kernel behavior require an
honest platform-specific threat model. An unlocked hostile kernel may already
have retained secrets. Encryption at rest alone does not solve this.

### Dependencies and acceptance

Design key classes, protected domains, app lifecycle and lock ABI before storage,
identity and session formats freeze. Implement only after M3 isolation/handles/IPC,
M4 device quiescence and M5 storage/lifecycle foundations; M9 cryptographic and
platform qualification is required for a security claim. Mobile qualifies equivalent
behavior independently, including Android compatibility apps and suspend/resume.

Test every trigger, concurrent reads/writes and key requests, noncooperative apps,
shared mappings, in-flight DMA, process death, multiple users, background exceptions,
power loss during relock, hibernation, stale authentication tokens, wrong credentials,
offline unlock and failure recovery. Verify no usable domain key or protected
plaintext remains accessible through the specified software/hardware interfaces.
No whole-device BFU-equivalence claim without a separately defined threat model
and independent evidence. All behavior in this section remains planned.

Reference models reviewed 2026-10-15 UTC:
- Apple Data Protection classes:
  https://support.apple.com/en-ie/guide/security/secb010e978a/web
- Android credential-encrypted versus device-encrypted storage:
  https://source.android.com/docs/security/features/encryption/file-based

