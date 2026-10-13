# Adjacent AOSP mobile project and ecosystem parity

Owner requirement, 2026-10-13: develop an AOSP-adjacent mobile member of JeiOS,
initially investigating Pixel 9/10, with near 1:1 user-facing functionality across
device types within hardware/security/resource limits.

The exportable [jeios-mobile seed](../mobile/jeios-mobile/README.md) contains its
own architecture, build qualification, target registry, inspector and handoff.
It is temporarily stored here; the separate GitHub repository is not created yet.
Original additions retain Apache-2.0. No Android source or vendor blob is included.

Proposed first bring-up uses AOSP/Linux, while auto44e/jeios keeps its fresh C
kernel. Shared protocols, identity/authority semantics and capability reporting
bridge implementations. Porting Android onto the new kernel is separate research,
not an implied result of mobile framework reuse. Mobile imports require component
provenance and license review, not blanket relicensing under Apache-2.0.

Cuttlefish is the first build/boot gate. Pixel 9 tokay is the first physical
candidate; Pixel 10 frankel follows independent qualification. Source revisions
and actual lunch products remain unresolved. The seed documents historical Google
source/vendor references without presenting them as a current secure release.

Parity includes identity/sync, family/fleet/domain workflows, sharing, MFA,
multitouch, accessibility, storage, customization, acceleration/media and recovery.
Unsupported capabilities report a reason; no silent weakened encryption or remote
execution to fabricate parity. Identical binaries/UI/driver access are not implied.

This is design/scaffold work only. Core M3a remains next. Track mobile MB0-MB4
separately and require observed build, boot and hardware evidence before claims.
