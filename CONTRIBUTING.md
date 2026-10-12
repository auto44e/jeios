# Contributing

This is an experimental new OS. Read AGENTS.md and the handoff, then choose one
bounded roadmap task. Explain behavior, ownership/lifetime, architecture-specific
preconditions, failure cases and tests. Keep implementation-status claims exact.
Use a branch and reviewable change after the initial scaffold.

Run `make check`; run `make smoke` for boot changes when tools are available.
Update HANDOFF with actual evidence, not expected CI. Never add private keys,
credentials, personal logs, generated binaries or imported source without review.

Project licensing is pending owner selection. External contributions and imported
code should wait until licensing/contributor terms are settled; report defects
and discuss architecture without assuming a distribution license.
