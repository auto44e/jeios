# Architecture and ecosystem parity

Status: proposed, 2026-10-13. The owner wants an adjacent mobile OS borrowing AOSP
framework capabilities and near 1:1 functionality across device types within
hardware, power and security constraints. No mobile runtime has been implemented.

## Platform relationship

The first practical track is an AOSP-based mobile system with an Android/Linux
kernel, Binder IPC, ART, framework services, HALs and platform security machinery.
Retain upstream isolation, SELinux enforcement, application sandboxing, keystore
and verified-update mechanisms while designing JeiOS integration. This is an
engineering proposal for bring-up, not a decision to replace the JeiOS C kernel.

Running Android atop the fresh kernel is separate research: Linux ABI assumptions,
Binder, bionic, runtime, graphics, HAL/driver and security dependencies need real
implementations or adapters. A cosmetic launcher cannot establish that portability.
Do not promise binary or driver compatibility across the two kernels.

Use narrow Android services/adapters for identity, sync, sharing and management.
Versioned protocols and data schemas are the portable boundary; native desktop
handles are never transmitted as meaningful mobile authority. Authenticate the
caller and translate each operation into scoped Android permissions/UID policy.
Prefer isolated app/service processes before framework-wide privileged changes.
Do not turn a universal privileged daemon into the ecosystem API.

## Capability parity contract

'1:1' means the same user goals, data, rights and predictable workflows where the
hardware permits, not identical UI geometry, kernel implementation or unrestricted
background execution. Every capability reports supported, limited, unavailable
or not-yet-implemented with reason and protocol version. Never silently weaken a
security requirement to claim parity. Remote execution needs a separately granted
capability and clear data destination; it is not implicit local support.

| Capability | Shared intent | Mobile adaptation / acceptance |
| --- | --- | --- |
| Local login and identity | No mandatory vendor account; isolated users | Android user/profile and lockscreen mapping; test background/key availability |
| E2E/PQ sync and nearby sharing | Same recipients, revocation, consent, conflicts and recovery | Battery-aware jobs, locked-device policy, radio/transport limits; vetted protocol |
| Family and enterprise | Separate private/work authority, clear policy sources | Qualify Android management/user APIs and distinguish work-profile/full-device control |
| Domain interoperability | Windows/macOS/Linux/JeiOS ecosystem identity and scoped access | Directory/SSO adapters; do not imply Windows GPO parity |
| Multitouch and accessibility | Touchscreen/trackpad gestures and equivalent accessible actions | Android input/framework adapters; external keyboards and displays qualified |
| Hardware MFA | User presence/verification, backup keys, recovery | USB/NFC/platform APIs; no assumption all modes work preboot |
| Files and storage | Safe sharing, format matrix, scoped raw access where supported | Android filesystem/kernel availability; raw system-volume writes remain privileged and exclusive |
| GPU/NPU/media | Open standards first, conservative scheduling and CPU fallbacks | Vendor HAL/runtime availability, thermal budgets and codec/SDK licensing |
| Customization and init | Intuitive defaults, explainable service/config state | Preserve Android init/service dependencies initially; translate user-facing policy, not replace PID1 blindly |
| Updates and privacy | Signed recoverable updates, local diagnostics, honest security status | Device-specific AVB, rollback indexes, OTA signing and firmware patch coverage |

Each row is planned. A per-release matrix must report observed behavior, not repeat
these goals as completed features. Core references are the design documents in
https://github.com/auto44e/jeios/tree/main/docs ; review INPUT.md and SECURITY_KEYS.md
from their PR if not yet merged. Carry later changes across both handoffs.

## Shared protocol requirements

Define version negotiation, explicit error codes, bounded payloads, cancellation,
idempotency, capabilities and data ownership before service code. Use distinct
identity/device/session IDs. Never sync reusable hardware private keys or ordinary
password verifiers as preferences. Untrusted relay/directory substitution, replay,
offline revocation, epoch changes and recovery are acceptance tests. Android's
existing encryption does not automatically provide JeiOS PQ authentication or
E2E group sync. A successful hardware keystore call is not proof of every device's
hardware protection level.

Maintain local-only operation, optional self-hosting and explicit family/work
recipients. Platform push services must not become a mandatory vendor account;
select transport/power tradeoffs deliberately. Cross-device compliance is tested
against a protocol simulator before native JeiOS services exist, then against
real implementations when available. Label simulator-only evidence accordingly.

## Security, licensing and maintenance

Preserve upstream notices and per-component terms. Original additions use
Apache-2.0; upstream Linux and proprietary vendor components are not relicensed.
Record provenance in a machine-readable component inventory before imports.
AOSP does not include a promise of Google Play/GMS rights or app certification.
Do not copy GrapheneOS/CalyxOS branding or claim equivalent hardening.

Verified boot, modem/baseband, camera/GPU firmware, rollback rules and unlockability
are device/SKU-specific. Maintain a patched branch and vendor update process;
a historical Android build is only a reproduction experiment, not a safe current
production baseline. No relocking until custom-key boot/update/recovery has been
independently qualified. Keep lab images isolated from personal accounts/data.
Document emergency calling, cellular, suspend and recovery limitations before any
physical image could be considered daily-use. No phone is flashed by this seed.
