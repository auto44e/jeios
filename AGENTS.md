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
3. Work on one roadmap gate at a time. M3 is complete; follow the dated current
   HANDOFF and docs/NEXT_PHASE.md for MP1 completion, M4 and bounded M5a planning.
   A desktop mockup does not advance kernel readiness.
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

## Multi-agent collaboration model

Owner direction 2026-10-17. Work on this repository may be carried out by more
than one AI coding agent operating as peers (for example a lead agent plus one or
more external coding agents), not only as author-plus-reviewer. The following
rules govern that mode and are themselves subject to the guiding star below.

- **Guiding star.** The one non-negotiable is the greater OS vision: the frozen
  ADRs (notably ADR 0007 for M3a), ROADMAP gates and the honesty/critical-profile
  discipline in this file, SECURITY.md and CODING_STANDARD.md. No agent decision,
  majority or otherwise, may weaken a security invariant, overclaim evidence, or
  contradict a frozen contract.
- **Authoring is shared, not centralized.** Distribute real authorship across
  agents to balance their use and reduce single-author blind spots. A useful
  default for a component with tests is to have the implementation, its test, and
  any fake/model written by *different* agents, so no one author's mistake covers
  both a behavior and its check.
- **Decisions are democratic for open questions.** Where a design choice is NOT
  settled by a frozen contract, agents decide among themselves by majority with
  recorded reasoning; a participant may be outvoted and then follows the majority.
  Record who proposed and who concurred so provenance stays honest.
- **Trust, but push back.** Agents extend good-faith trust to each other's
  judgment AND are expected to challenge it; this applies in every direction,
  including a reviewer pushing back on the lead and the lead pushing back on a
  reviewer. Unchallenged agreement is not evidence.
- **Hard overrides that beat any vote.** (1) A verified fact — a failed compile,
  a failing test, observed CI — settles the matter regardless of opinion. (2) Any
  change that touches a frozen contract/ADR or previously tested code (e.g. the
  M1/M2 allocator) is escalated to the owner, not decided by agent majority.
  (3) The honesty rules (local vs remote CI, implemented/tested/planned/blocked,
  no invented reviewers, AI-assisted review is NOT independent human
  qualification) are never votable.
- **Review status is honest about its nature.** Multi-agent review is valuable
  signal and must be recorded as AI-assisted review. It does not by itself satisfy
  the independent human review that the critical profile requires before a
  hardened/release qualification claim; that remains a human gate.

## Single-agent contributions (verbose decision/logic/workflow in the PR)

Owner direction 2026-10-19. When a contribution is carried out by a **single
agent** (no peer agents to externalize the reasoning into a debate transcript),
the PR itself MUST carry that reasoning. The obvious code diff is necessary but
NOT sufficient: a single agent's decision-making, logic, and workflow otherwise
live only in that agent's transient context and are lost to reviewers and future
sessions. So a single-agent PR description MUST be **as verbose as possible** about:

- **Decision-making** — every non-obvious choice made while implementing, the
  alternatives weighed, and why this one won (the ADR 0000 three parts — decision
  + why, predictions if other choices were made, flex to revisit — applied at PR
  granularity, not only at ADR granularity). If a choice touches a frozen
  contract/ADR/tested path it is escalated to the owner, exactly as in the
  multi-agent model.
- **Logic** — the actual algorithm/invariant reasoning: why the code is correct,
  which invariants it preserves, the edge cases considered and how each is handled,
  and the failure modes. Not a restatement of the diff — the reasoning the diff
  does not show.
- **Workflow** — the path taken to the result: what was investigated, what was
  tried and rejected, the order of operations, and the verification actually run
  (exact `make check`/`smoke`/`integration`/CI-profile/ARM results, honest about
  local vs remote CI). A dead end that was ruled out is useful signal; record it.

This is the single-agent analogue of the multi-agent debate transcript + the
`docs/adr/debates/` attachment rule (ADR 0000 part 4): where peers would have
produced a transcript, the lone agent produces a verbose narrative in the PR. The
honesty rules above apply unchanged (implemented/tested/planned/blocked;
AI-assisted review is not human qualification; no invented reviewers). Verbosity
never licenses overclaiming — more words, all of them accurate. CONTRIBUTING.md
restates this as a PR requirement; the two are one norm.
