# JeiOS contributor and AI agent instructions

Read `README.md`, `docs/HANDOFF.md`, `docs/ROADMAP.md`, and the relevant sections of
`docs/DESIGN.md` before editing. Also read `docs/AGENT_START.md` for the consolidated
owner requirements and task sequence. This file governs the whole repository.

## Required discipline

1. Treat this as a fresh C kernel, not a Linux fork. C11 is the current core
   language; assembly is reserved for boot, traps, context switching, and exact
   architecture operations. Do not introduce another core language by accident.
2. Read the implementation before claiming a design feature exists. Update the
   implementation-status table and handoff in the same change as new behavior.
3. Work on one roadmap gate at a time. Continue with M3a userspace boundaries after reviewing the completed M1/M2
   implementation ledger. A desktop mockup does not advance kernel readiness.
4. Document contracts: ownership, lifetime, locking, caller context, bounds,
   privilege, error states, rollback, hardware prerequisites, and tests. Explain
   the reason for subtle register/assembly operations and cite architecture docs.
5. No silent privilege changes or weakened security through difficulty/theming
   settings. Hardware absence must produce an explicit degraded profile or a
   refusal when a requested guarantee cannot be delivered.
6. Do not run `-march=native` on baseline kernel objects. Feature-specific code
   must be isolated and gated by hardware, OS context support, and policy.
7. Do not equate a module boundary with fault isolation. Ring-0 code can corrupt
   the entire kernel. Dynamic loading is not implemented or authorized by M0.
8. Do not import Linux, BSD, ReactOS, Apple, Android, firmware, or vendor source
   until provenance and license compatibility have been documented. Original JeiOS
   code uses Apache-2.0 (owner decision 2026-10-13); this does not relicense
   dependencies or authorize imports. Architectural inspiration is allowed.
9. Run `make check`. For boot-affecting changes run `make smoke` where dependencies
   exist. Record inability to run tests plainly. Never report expected CI as
   observed CI. Add meaningful boundary/failure tests for new core mechanisms.
10. Update `docs/HANDOFF.md` with exact commands, evidence, open issues, and next
    steps. Include changed invariants and unresolved hazards. Leave a small,
    reviewable commit; do not overwrite another contributor's work.
11. Do not add secrets, signing keys, personal data, binaries, build outputs, or
    credentials. Public diagnostics must redact sensitive runtime information.
12. Do not promise all hardware, all drivers, or all application ABIs. A target is
    supported only after reproducible execution tests and a named support tier.

## Before committing

Review `git diff --check`, tests, assembly/linker placement, undefined symbols,
and documentation status. In a connected GitHub workspace avoid force pushes.
Do not request reviewer assignments, send messages, or publish releases unless
asked. Main was initially empty; future substantive changes should use a branch
and a reviewable PR unless the owner directs otherwise.

## Handoff format

State: implemented / tested / planned / blocked. List files and commands. Identify
next bounded task and its pass/fail criteria. Record what is NOT enforced yet;
security claims must map to actual checks at a trust boundary.


## Status refresh and historical records

Owner preference recorded 2026-10-14: retain detailed timestamped history alongside
a clearly labeled current status. Every refresher must record an ISO-8601
observation time with timezone, repository/branch and exact inspected commit,
implemented/tested/planned/blocked states, PR/issue state, evidence and next task.
Separate an event's timestamp from the time it was verified. If an old record has
only a date, preserve that precision; never invent a time. Keep superseded plans
and previous failures as labeled history with links to their replacements.

Current status takes precedence over historical branch names or open/closed
wording. Preserve previous test results at their original commit and environment;
do not present them as rerun or current-head CI. State unverified items explicitly.
Use enough detail for an agent to resume without reconstructing the conversation.

## Ecosystem coding policy

Read [docs/CODING_STANDARD.md](docs/CODING_STANDARD.md) for NASA/JPL-inspired
risk profiles and JCS-01 through JCS-10. Apply applicable rules to new/materially
changed code, identify evidence and deviations, and never claim unaudited existing
code conforms. Automation rollout is core #19 with CI #18.
