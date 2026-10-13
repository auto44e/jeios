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
