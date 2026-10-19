# ADR 0005: Apache License 2.0

Status: accepted by owner, 2026-10-13; supersedes the 2026-10-12 pending decision.

Use Apache License, Version 2.0 for original JeiOS project code. The unmodified
license text is in ../../LICENSE. Source files use the Apache-2.0 SPDX identifier.
This records the owner's explicit choice, not a license inferred from GitHub
visibility. Existing third-party work is never relicensed by this decision.

Contributors must have authority to submit their work under these terms and
preserve accurate notices. No copyright assignment or separate CLA is introduced.
Maintain per-component provenance, exact license/build versions and required
notices/source distribution. Preserve applicable NOTICE material when importing
components; do not fabricate attribution or claim ownership of upstream work.

Apache-2.0 provides copyright and contributor patent grants subject to its terms.
It does not confer unrelated codec patent, firmware or proprietary SDK rights.
Review dependencies and actual linking/distribution arrangements individually.
Separate processes are an architectural boundary, not an automatic legal ruling.

FFmpeg, HandBrake and Blackmagic RAW integration remains planned; each keeps
its applicable upstream terms. See ../ACCELERATORS.md. No external kernel source,
codec or SDK is imported by this change.

Primary source: https://www.apache.org/licenses/LICENSE-2.0

## Decision-record annotation (reconstructed — sweep 2026-10-19)

Per ADR 0000; reconstructed from this ADR's text only. Part 4: no transcript
retained.

**Decision + why (as recorded).** Apache License 2.0 for original JeiOS code
(SPDX `Apache-2.0`); an explicit owner choice (2026-10-13), not a license inferred
from repo visibility; third-party work is never relicensed; per-component
provenance + NOTICE preservation required; no CLA/assignment.

**Predictions if other choices were made (reconstructed).**
- *Infer the license from GitHub visibility (the superseded 2026-10-12 pending
  state):* the ADR exists precisely to reject that — an inferred license is not a
  grant, leaving contributions legally ambiguous.
- *A copyleft license (e.g. GPL):* not in the record as weighed; consequence
  **unknown — not in the record**. (The ADR does record Apache-2.0's specific
  property: copyright + contributor patent grants, but "not unrelated codec
  patent, firmware or proprietary SDK rights" — the reason accelerator
  integrations keep their own upstream terms, ADR/ACCELERATORS.md.)

**Flex options to consider in future (reconstructed).** **Firm** — a license
change is an owner decision + a new ADR, never incidental. Per-dependency
linking/distribution review is an ongoing obligation, not a revisit of this
choice.
