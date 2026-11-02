# Built-in hardware security-key support

[Included native software portfolio](NATIVE_PRODUCTS.md): latest-stable source policy, C coreutils/GNU+uutils tracking, SSH/TLS, compilers, Make, curl, FIDO, zlib and QEMU. Planned integration; no runtime support added.

Status: owner requirement and design, 2026-10-13. No authenticator transport,
credential broker or MFA login flow is implemented. This extends IDENTITY.md,
DOMAINS.md and ENCRYPTION.md; it does not advance the current M3a kernel gate.

## Standards and system integration

Provide an OS credential broker with FIDO2/CTAP support and a native API; browser
integration implements WebAuthn origin/relying-party rules. Qualify USB HID first,
then NFC and applicable Bluetooth transports independently. Protocol version,
extensions and transport availability must be negotiated and tested, not inferred
from a connector shape or vendor name. Legacy U2F support is a separate optional
compatibility profile. PIV/smart-card/PKCS#11 integration is a separate later
enterprise adapter, not implied by support for a multi-function key's FIDO mode.

Use security keys for local login/unlock, privileged operations, new-device
approval, account/recovery changes and enterprise authentication where the
corresponding relying party supports them. Domain login mappings need their own
protocol support; a FIDO assertion is not automatically a Kerberos credential.
Expose consistent enrollment, naming, last-used state, removal and testing through
GUI/CLI APIs. Do not silently synchronize hardware-bound private credentials to
other devices; enroll each relevant relying party/device through explicit flows.

## Authentication and authority

Each ceremony uses a fresh challenge bound to the account, relying party, operation
and requesting session. The broker authenticates native application identity;
untrusted callers cannot choose another app's relying-party identity. Browsers
must enforce origin/RP binding and trusted UI context. Native local authentication
requires a specified and reviewed RP identity scheme; do not invent an arbitrary
website origin to bypass those constraints.

Touching a key proves user presence, not necessarily user verification. Enforce
PIN/biometric verification when policy requires it, or an independently validated
second factor for MFA. Passwordless and MFA describe different properties; a
presence-only key is not automatically multifactor. Verify assertion signatures,
challenge, credential/account binding and required flags, including replay handling.
Counter anomalies inform credential-specific policy; do not assume every key has
a globally monotonic nonzero counter. Treat removable authenticators as untrusted
peripherals even though they protect credentials.

No ambient raw authenticator access for apps. Broker requests have owner/session,
operation ID, deadline and cancellation state. Bound concurrent prompts, messages,
CBOR nesting/length and device queues. Serialize per-device conversations; reject
late responses after logout, cancellation, unplug or attachment-generation change.
Never let the next user's ceremony consume the previous user's result. PIN entry
uses the secure input path, is not logged, and respects authenticator retry limits.
Clear transient secrets; keys/tokens cannot escape through diagnostic APIs.

## Good defaults and recovery

Local-only setup must remain possible without buying a key. Make registration
straightforward, recommend a backup key or deliberately provisioned recovery path,
and verify it before enforcing a key-only policy. Adding/removing factors requires
reauthentication or the explicit recovery protocol. Loss, theft, PIN lockout,
reset and replacement have distinct UI and revocation steps; reset may destroy
credentials and must not masquerade as harmless troubleshooting.

Personal policy does not demand identifying attestation by default. Enterprise
allowlists/attestation requirements are explicit, scoped and show privacy/support
consequences. Removal/revocation affects the correct account/device/tenant only;
an offline endpoint cannot instantly learn server revocation. Cache local trust
under a documented offline policy and preserve an auditable recovery route.
Key removal does not automatically log out every session unless that behavior was
explicitly selected and the active-ceremony/key relationship is established.

Authentication is not disk decryption or E2E recovery. Using a key to unlock
wrapped data keys needs a separately reviewed mechanism, supported authenticator
extensions, KDF/key separation, backups and lockout handling. Signatures alone
are not secret key derivation. Preboot unlock needs its own trusted minimal driver
and broker path; post-login browser support is not proof it works before disk
unlock. Never add a recovery backdoor to make security-key loss transparent.

Current hardware-key support must not be labeled post-quantum merely because
JeiOS has a PQ transport plan. Report actual credential algorithms and minimum
policy; unavailable PQ authentication requires explicit incompatibility or an
approved visibly weaker profile. No invented firmware capabilities or silent
cryptographic downgrade. Key firmware, host OS compromise and secure UI limits
remain part of the threat model.

## Gates and tests

M4: qualified transports and lifecycle; M5/M6: durable credential records and
broker protocols; M7: accessible enrollment/login UI; M9: reviewed MFA, entropy,
recovery and secure boot dependencies. Preboot unlock and enterprise adapters
receive independent acceptance gates. Select a reviewed library only after
Apache-2.0 compatibility/provenance checks; original project licensing does not
relicense dependencies.

Test real keys from multiple vendors plus malicious/emulated devices: malformed
responses, wrong RP/account/origin/challenge, replay, missing UP/UV, cancellation,
timeout, unplug, concurrent users, PIN retry limits, lost-key recovery, backup-key
registration, revoked keys, offline login, unsupported algorithms/transports,
attestation privacy and secure-input injection. Publish a versioned feature/device
matrix. No compatibility or certification claim precedes observed evidence.

## Primary references

Reviewed 2026-10-13; pin protocol versions and review errata at implementation:

- [FIDO specifications](https://fidoalliance.org/specifications/): CTAP and authenticator transports.
- [W3C WebAuthn](https://www.w3.org/TR/webauthn-3/): RP/origin binding, user presence and verification.
