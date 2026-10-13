# Multitouch input throughout JeiOS

Status: owner requirement and proposed contracts, 2026-10-13. No input drivers,
compositor, gesture engine or touch UI is implemented. Full multitouch is a design
goal across qualified hardware, not a claim that every device works automatically.

## Scope and defaults

Treat direct touchscreens and indirect multitouch trackpads as first-class input
from device drivers through the input broker, compositor, native toolkit and
applications. Cover desktop, login/lock, settings, permission prompts, recovery,
on-screen keyboard, window management and eventual mobile/tablet profiles.
Provide keyboard, mouse and accessibility equivalents for essential actions;
never require a complex gesture as the only way to recover or authenticate.

Defaults should include consistent tap/select, drag, scroll, pinch zoom and
supported rotate/navigation gestures, with discoverable feedback. Direct touch
must target the screen location; trackpad motion uses indirect pointer semantics.
Gesture ownership, scroll direction, sensitivity, tap-to-click, palm rejection,
edge gestures, handedness and per-device/per-app overrides are configurable.
Offer previews and reset-to-default without changing security authority. Do not
assume a fixed finger count or synthesize pressure when hardware cannot report it.

## Input protocol and lifecycle

Start with documented HID-class devices and qualify USB and I2C HID transports
separately; Bluetooth and vendor-specific devices need their own drivers/tests.
Driver reports carry device/attachment generation, seat, monotonic timestamp,
frame sequence, contact ID, begin/update/end/cancel and supported coordinate,
contact-size, pressure and tool-type fields. Declare units, axis ranges, maximum
contacts and optional fields. Validate descriptors and report lengths before
allocation; bound contacts, queues and processing work against hostile devices.

Maintain contact identity until explicit end/cancel. Batch coherent contact frames
so applications cannot observe half a hardware update. On queue overflow, report
loss and cancel/resynchronize the affected sequence rather than leaving stuck
contacts. Hot-unplug, suspend, session switch and driver reset cancel all owned
contacts and invalidate the attachment generation. Reused contact IDs cannot
revive previous gestures. Drivers own buffers until validated broker handoff;
clients receive bounded event copies or scoped immutable grants, not device DMA.

The input broker owns per-seat device routing; compositor state owns surface focus
and capture; apps receive only authorized surface events. Serialize each gesture's
ownership transition and never hold kernel/driver locks while calling apps.
Define coalescing rules that retain begin/end/cancel events and multi-contact
coherence. Use bounded queues and backpressure; measure input-to-display latency
on real devices before assigning performance guarantees.

## Coordinates, gestures and UI integration

Map physical coordinates through calibration, display association, orientation,
scaling and surface transforms. Pin a transform for a gesture or cancel it when
mapping changes; do not jump a drag to another monitor. Support mixed DPI,
rotated displays, multiple touchscreens and multi-seat isolation. Trackpads report
relative motion/gestures separately from direct touch contact positions.

Arbitrate system versus app gestures explicitly: reserve a small documented set,
let apps negotiate supported gestures, and cancel raw delivery when recognition
transfers ownership. Never deliver the same action as both a compatibility click
and native touch activation. Legacy pointer-only apps receive a documented
single-pointer fallback; this cannot create native multitouch features inside
unmodified foreign apps. Publish the limitation instead of claiming parity.

Native widgets expose usable touch targets, long-press alternatives, drag handles,
focus/selection behavior and accessible gesture actions. Palm rejection, typing
suppression and stylus coexistence are qualified policies, not reasons to discard
all simultaneous keyboard/touch use. Stylus, hover and pressure remain optional
capabilities. Test assistive input, reduced motion, left-handed use and users who
cannot perform multi-finger gestures. Secure prompts retain accessible alternatives.

## Input security and qualification

Raw/global input observation and event injection require distinct capabilities.
Normal apps cannot observe another user's contacts, secure text entry or key PINs.
Mark synthetic/remote input and restrict secure-attention and authentication flows;
software injection is not proof of hardware authenticator user presence. Routing
changes and compositor recovery must not deliver a held gesture into a new secure
prompt as consent. Do not log touch traces or typed content by default.

M4 qualifies device lifecycle and input transport bounds. M7 implements broker,
compositor, native toolkit and secure UI contracts; E1 qualifies mobile profiles.
Preboot touch support needs its own firmware/driver path; a serial-only prototype
does not meet the complete touch recovery goal. Essential keyboard/serial fallback
remains available where hardware supports it.

Acceptance: descriptor/report fuzzing; maximum simultaneous contacts; two-screen
routing; DPI/rotation changes; palm/typing coexistence; hotplug/suspend; dropped
frames/overflow; gesture cancellation/arbitration; app/compositor crash; multi-seat
and cross-user isolation; injected-event denial at secure UI; accessibility and
pointer compatibility. Test a published device/transport/contact-count matrix.
Define the native event ABI and numeric resource limits in an ADR before coding.
