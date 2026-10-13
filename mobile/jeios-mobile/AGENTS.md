# JeiOS Mobile agent contract

Read README.md and all docs before editing. This is an AOSP-adjacent mobile
project, distinct from the fresh C kernel in auto44e/jeios. Do not copy the core
repo's C-only implementation rule onto upstream Android's Java/C++/Rust components.
Keep original shared low-level libraries portable where practical; no automatic
foreign-source import or license conversion follows from framework inspiration.

Record implemented/tested/planned/blocked status. No target becomes buildable
until source revision, product definition, release config, kernel/vendor/firmware
pairing and provenance are verified. Freeze manifests before reproducible builds.
Do not invent lunch commands, permissive SELinux fixes, AVB bypasses, keys or device
support. Do not flash/unlock/relock/wipe a phone as part of a scaffolding task.

Keep small reviewable branches. Document changes, test evidence, unresolved risks
and next bounded gate in docs/HANDOFF.md. Never commit vendor blobs, signing keys,
credentials, user identifiers or build outputs. Track upstream patch/update debt.
Before importing components record exact source, revision, license, redistribution
terms and modifications. No production/security/CTS claim without observed tests.
