# Mobile scaffold handoff

2026-10-13. Intended repo: auto44e/jeios-mobile. Currently staged in the core
repository as mobile/jeios-mobile because the available GitHub connector cannot
create repositories. Export this directory as the new repository root without
importing the entire core history. Browser creation requires approved fallback.

Implemented: original docs, Apache-2.0 LICENSE, agent contract, candidate registry
and read-only target-plan inspector. Tested: inspector execution, Python syntax,
relative links and repository whitespace checks. No AOSP build, Android service,
device tree, emulator boot, physical flash, security or compatibility test exists.
No upstream source/binary was imported. No separate GitHub repository exists yet.

Owner goal: AOSP-adjacent mobile system, Pixel 9 or 10 candidate, near 1:1 ecosystem
functionality with reasonable device limits. Engineering proposal: Cuttlefish first,
Pixel 9 tokay first hardware, Pixel 10 frankel later; AOSP/Linux bring-up separate
from the fresh C kernel. Exact AOSP release, framework adaptation strategy and
product names are unresolved, not silently approved stable interfaces.

Next bounded task MB0: select maintained source, inspect actual product/release
files, record immutable revisions/toolchain and dependency provenance. Pass only
when another agent can reproduce source selection and all blockers are explicit.
Then MB1 unchanged upstream build/boot before adding JeiOS services. Core kernel
M3a proceeds independently; mobile scaffolding does not advance that gate.

Run from this seed root: `python3 tools/target_plan.py` and
`python3 -m py_compile tools/target_plan.py`. Exit zero means the registry was
inspected successfully, not that any target builds. Do not execute illustrative
lunch placeholders. Use BUILD.md for qualification and DESIGN.md for parity.
