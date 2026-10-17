# Contributing

This is an experimental new OS. Read AGENTS.md and the handoff, then choose one
bounded roadmap task. Explain behavior, ownership/lifetime, architecture-specific
preconditions, failure cases and tests. Keep implementation-status claims exact.
Use a branch and reviewable change after the initial scaffold.

Run `make check`; run `make smoke` for boot changes when tools are available.
Update HANDOFF with actual evidence, not expected CI. Never add private keys,
credentials, personal logs, generated binaries or imported source without review.

Original project contributions use Apache-2.0; see LICENSE and ADR 0005. Submit
only work you have authority to contribute under those terms. Preserve accurate
authorship and third-party notices; document provenance, dependency versions and
license compatibility before imports. Use SPDX-License-Identifier: Apache-2.0
for original source files. Third-party code keeps its own license and must not
receive an inaccurate Apache identifier. No separate CLA is introduced here.


Follow [the ecosystem coding policy](docs/CODING_STANDARD.md). In each code PR,
identify risk profile, applicable rules, changed invariants, verification evidence
and deviations. Human and AI contributions have the same obligations.
