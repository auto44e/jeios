# Contributor assistance and local verification

Owner direction: 2026-10-19. These additional onboarding and evidence expectations
apply to contributors other than the project owner, especially contributors using
agents. Existing project-wide standards, review and evidence rules still apply to
everyone; this addition does not impose an onboarding mode on the owner or exempt
owner contributions from existing rules.

Start with [GETTING_STARTED.md](GETTING_STARTED.md) for a conversational or
hands-on first contribution. At the easiest level, contributors can code through
plain-English conversation: the agent translates intent into implementation and
tests, operates available tools, and explains consequential choices. Prior C,
terminal or Git syntax knowledge is not required. This is a contribution workflow,
not a new programming language or an exemption from review.

## One standard, a flexible assistance scale

Difficulty controls how much guidance an agent provides, not the quality of the
contribution or its permissions. Every level follows AGENTS.md, accepted ADRs,
coding standards, provenance rules, tests, evidence and owner approval gates.
“Hard” does not grant permission to skip documentation, and “easy” does not lower
acceptance criteria. This contributor setting is separate from OS user/security
profiles and is not a code or runtime configuration.

| Example point on the scale | Agent behavior |
| --- | --- |
| Easy / fully guided | Walk through setup and verify it; explain coding and OS development at a high level; introduce terms in plain language; guide task selection, edits, tests, decisions, PR creation and review responses. |
| Supported / collaborative | Provide a short plan, explain unfamiliar or consequential concepts, perform routine setup/work, and discuss meaningful choices with concise tradeoffs. |
| Hard / expert | Keep routine coaching minimal; execute the agreed task and report precise changes, decisions, evidence and blockers while following all guidance and standards. |

These are examples, not three locked modes. Offer a lightweight preference choice
at onboarding; if unspecified, start with brief plain-language guidance and adapt.
A contributor may ask for more or less detail at any time, including mid-task or
for one concept only. Do not restart work, discard evidence or change scope when
they adjust the scale. Do not infer ability from identity, job title or language.
If relevant, carry the chosen assistance level and current checkpoint into a
handoff without collecting unnecessary personal information.

An agent should do authorized routine work rather than merely hand the beginner a
list of commands. Explain what commands change and how success is checked. Keep
meaningful contributor choices visible: summarize options, recommendation and
consequences in simple terms, distinguish reversible implementation choices from
owner-gated contract changes, and record the chosen outcome. A contributor's agent
cannot approve a project-owner gate on the owner's behalf. Provide concise
technical decision summaries and evidence, not private internal deliberation.

## Fully guided contribution walkthrough

1. Explain the project: a kernel controls memory, CPU time and hardware; userspace
   services run behind that boundary. A compiler builds programs, an emulator
   runs a virtual machine, and CI repeats checks automatically. Describe the
   current milestone and its limits before choosing a small real contribution.
2. Inspect the contributor's OS/architecture, installed tools, workspace and
   available resources. Explain the supported host/target distinction. Read
   README, AGENTS, HANDOFF, the roadmap and relevant code before changing anything.
3. Set up an isolated checkout and branch, preserve existing work, and install or
   configure the required tools where authorized. Use the repository's documented
   dependency/target versions; do not assume a generic container or different OS
   reproduces the supported platform. Ask only when a missing choice, privilege
   or consequential host change requires it. If setup is blocked, explain exactly
   what is missing and continue useful work that does not depend on it.
4. Run a baseline build/test and explain its result. Separate a host/tool failure
   from a code defect. Use disposable emulator images; never treat onboarding as
   authorization to format a disk, flash a phone or provision secrets.
5. Guide the change: explain the relevant concept (for example, a capability is a
   checked permission to use an object), ownership and failure behavior. Discuss
   important choices before they become costly. Make small reviewable changes.
6. Run applicable local CI steps and additional tests below. Explain what each
   test establishes and what it cannot establish. Reproduce failures, fix the
   cause, and retain useful failing/passing evidence without hiding gaps.
7. Review the diff, update status/handoff docs, prepare a PR with result data,
   explain review feedback, and help revise it. Explain that PR submission does
   not mean approval, merging, hardware qualification or completion of a gate.

## Broader local testing and CI-style runs

External contributors must reproduce the relevant repository CI jobs locally
where their environment supports them, plus tests for the changed behavior and
its failure boundaries. A single happy-path test or remote green badge alone is
insufficient for a runtime change. Read the actual workflow at the tested commit;
local CI-style execution means running its applicable commands, not claiming a
GitHub-hosted runner ran locally. Do not replace working gates to save time.

Choose breadth by changed mechanisms, architectures and risk, not assistance
level. State the matrix before claiming coverage:

| Change | Local verification expected |
| --- | --- |
| Core documentation only | `make check`, changed links/whitespace and status/evidence consistency; emulator checks may be not applicable with a reason. |
| Core runtime, ABI, memory, process or IPC | `make check`, `make smoke`, `make integration`; current CI CPU/memory profiles; focused denial, malformed input, exhaustion, rollback, lifetime and survivor tests relevant to the change. |
| ARM boot/architecture | Applicable core regressions plus `make aarch64`, `make smoke-aarch64`, vector and BSS profiles when relevant; malformed boot input/fault evidence for changed paths. Qualify ARM separately from AMD64. |
| Mobile planning/config/tooling | `python3 tools/validate.py`, `python3 tools/target_plan.py`, `python3 -m unittest discover -s tests -v`; changed-link checks and negative tests for changed validation. These do not prove native execution. |
| Parsers/storage/services/build infrastructure | Relevant base gates plus deterministic hostile/corrupt inputs, failure injection, timeout/restart/recovery or known-bad gate checks appropriate to the change. Use disposable fixtures. |

For current AMD64 profile commands, see [DEVELOPMENT.md](DEVELOPMENT.md) and
[ci.yml](../.github/workflows/ci.yml). For ARM targets inspect mk/aarch64.mk and
mobile ADR 0002; `ARM_VECTOR_TEST=1` and `ARM_BSS_CHECK=1` select separate builds.
Not every documented ARM command is currently a remote CI job. Recheck workflow
contents rather than relying on a historical CI summary.

Broaden tests when a concrete failure mode, regression risk or required gate calls
for it. Additional compiler versions, static analysis, fuzzing or longer stress
runs are valuable where qualified and relevant; label experiments separately from
supported profiles. Do not demand meaningless full hardware matrices for a typo,
repeat green tests without a reason, or invent coverage percentages.

If tools, permissions, architecture or resources block a required run, make a
reasonable documented setup attempt and report BLOCKED, the cause and a concrete
follow-up. Never label an unrun check PASS. A draft PR can request help; completion
and merge still depend on the applicable gate/reviewer disposition. Local results
do not waive remote checks, and remote results do not fabricate local coverage.

## Reproducible runs in a container (recommended, optional)

The repository ships a pinned builder image and a task runner under `.docker/`
(see [.docker/README.md](../.docker/README.md)). It gives you the same toolchain
CI uses — gcc, the aarch64 cross-toolchain and QEMU 8.2 — without installing them
on your host, and runs each task on a clean copy of the tree so a stale local
`build/` cannot mask a result. It is the easiest way to produce the local
CI-style evidence a PR needs.

```sh
.docker/ci-run.sh check          # make check, isolated clean copy
.docker/ci-run.sh mutants        # the critical-mutant assurance harness
.docker/ci-run.sh soak 50        # repeat check+mutants to hunt flakes
.docker/ci-run.sh --in-tree shell # a shell in the image, your worktree mounted
```

First-time Docker setup needs a **log out / log back in (or a reboot)** after
`sudo usermod -aG docker "$USER"` before `docker` works without `sudo` — a group
change does not apply to shells that were already open. Verify with
`id | tr ' ' '\n' | grep docker` and `docker info`. If Docker is unavailable,
run the `make` targets directly on your host instead and say so in the PR; the
container is a convenience, not a required gate.

Honest scope: the container pins the **software** toolchain and the emulator, and
QEMU runs in TCG mode (no KVM). It is reproducible-enough for consistent toolchain
and local/CI parity, but it is **not** real-hardware coverage and not a bit-for-bit
clean-build guarantee (the base image is digest-pinned; the apt packages are
recorded, not version-pinned). Physical-board qualification remains a separate
gate. Record the image's recorded toolchain (`docker run --rm jeios-builder cat
/etc/jeios-toolchain`) alongside your results when a run matters.

### Using AI coding agents (any number, even one)

If you work with an AI coding assistant, it can carry much of this loop, and the
container makes that safe. A useful pattern even with a single agent:

- Let the agent **run the task in a container and read the (large) log** rather
  than pasting thousands of lines into the conversation: it runs
  `.docker/ci-run.sh mutants`, then reads the captured log file and reports the
  short verdict (survivors, seeds, a hang vs a crash). The container is the blast
  radius; the agent interprets, you decide.
- For heavier or parallel work (long soak/fuzz sweeps, cross-arch builds) an agent
  may drive its **own disposable container** and tear it down afterwards. Keep it
  contained: a throwaway `docker run --rm` box, `--network none` unless the task
  needs the network, run as your UID, **never** mount the Docker socket or
  credentials into it, and build on a container-private copy — never let an agent
  write your working tree or push on your behalf.
- Whatever an agent reports is **execution evidence, not qualification**: re-run
  the real gate yourself (or have a second agent of a different lineage review the
  diff) before you rely on it. An AI-assisted review is not the independent human
  review the critical profile requires. This mirrors the project's multi-agent
  norms in [AGENTS.md](../AGENTS.md); they apply to a single agent too.

This whole subsection is a recommendation to make local verification easier and
safer, not a new required gate. The verification matrix above is what a PR must
satisfy, by whatever means.

## Result data required in PRs

Use the PR template and include a compact evidence table, not only “tests pass.”
For each run, record:

- Tested source commit (or base plus exact working-tree patch), repository,
  date/time with timezone, command, relevant flags/environment overrides and seed.
- Host OS/architecture, compiler/linker/emulator versions, target CPU/memory/profile
  and relevant dependency versions. Redact hostnames/usernames where unnecessary.
- PASS / FAIL / BLOCKED / NOT RUN / NOT APPLICABLE, exit status, duration when
  measured, expected/observed markers or meaningful counts, and log/artifact link.
- Relevant failure reproduction and regression result; identify which test checks
  which changed invariant, and remaining unsupported cases or unresolved failures.
- For images/fixtures: hash and reproducible generation command where relevant.
  Attach sanitized bounded logs through PR/CI artifacts or approved project
  storage; do not commit build binaries, secrets or private host logs to source.

Use one row per distinct command/profile; report sampled coverage as sampled.
Do not substitute an old commit's result for the PR head: rerun affected checks
when executable inputs change, and identify docs-only differences explicitly.
Record unavailable measurements as unmeasured. Preserve useful failure details
alongside the corrected result, with sensitive data removed.

The documented LeakSanitizer environment exception must name the restriction,
exact override and resulting coverage gap; ASan/UBSan success without leak scanning
is not a full leak-check pass. Keep local, remote CI, emulator and physical-device
evidence distinct. Agent-authored/reviewed evidence is not independent human
qualification. Single-agent PRs still include the project's required detailed
technical rationale, alternatives, invariants and verification summary.

## Decision record and rollout

Decision: make assistance adjustable while preserving a single engineering bar,
and require broader risk-based local verification with reviewable result data.
Reason: contributors should be able to learn and contribute without losing the
reproducibility and failure evidence needed for OS work.

Alternative prediction: fixed beginner/expert modes would not fit mixed expertise;
a weaker beginner test bar would make acceptance inconsistent. Requiring every
possible test for every edit would add cost without proportional evidence.
Flex: adjust coaching continuously and refine test matrices as targets/tools mature;
changes to required gates still follow project review. This document and the PR
template add guidance, not automated enforcement. CI/enforcement work remains
tracked by #18/#19; no new runner, workflow or certification is claimed.
