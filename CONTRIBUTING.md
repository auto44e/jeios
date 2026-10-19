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

**Single-agent PRs: make the reasoning verbose.** If you contribute using a single
AI agent (no peer agents), the PR description MUST externalize the agent's
decision-making, logic, and workflow — not just the code diff — as fully as
possible: every non-obvious choice and why (with alternatives and what would
reopen it), the correctness/invariant/edge-case/failure-mode reasoning the diff
does not show, and the path taken including dead ends tried and the exact
verification run (honest about local vs remote CI). This is the single-agent
counterpart to the multi-agent debate transcript; see the "Single-agent
contributions" section of AGENTS.md. Verbosity must never become overclaiming —
keep every implemented/tested/planned/blocked claim exact.

## Assistance level and PR evidence

External contributors, especially those using agents: read the
[contributor guide](docs/CONTRIBUTOR_GUIDE.md). Choose as much or as little coaching as useful and
change that level at any point. Easy mode guides setup and explains development
and decisions in simple terms; hard mode retains every engineering requirement.
Run applicable local CI commands and broader change-specific tests, and include
result data using the PR template. Disclose blocked checks and environment
limitations; local and remote results are separate evidence.
