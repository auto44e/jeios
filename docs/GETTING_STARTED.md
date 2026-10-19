# Getting started with JeiOS development

You can contribute by having a natural conversation with a coding agent, by
writing code directly, or by moving between the two. You do not need to know C,
shell commands or Git syntax to begin the fully guided path. The agent translates
your plain-English goals into code, tests and a reviewable contribution; you guide
the intended behavior and discuss important choices.

JeiOS is an experimental fresh C11 operating system, not a Linux distribution.
AMD64 M3 currently demonstrates isolated processes and message passing; ARM64 is
in early bring-up. It is not a daily-use OS. Start in an emulator, a program that
acts like a separate computer, rather than booting it on your personal machine.
Current progress and tasks live in [HANDOFF.md](HANDOFF.md) and
[NEXT_PHASE.md](NEXT_PHASE.md).

## Path A: contribute through conversation

Open a coding agent with access to your development workspace and the repository.
A chat without file/terminal access can explain or draft changes, but cannot
truthfully claim to have set up your computer, edited files or run tests.
You can start with this request:

> Help me make my first JeiOS contribution in fully guided mode. I want to work
> in plain English. Read the repository instructions, check my environment, help
> set up the tools, and suggest a small useful task. Explain OS and coding concepts
> simply as we need them. Write and test the code with me, explain consequential
> decisions, and prepare a PR with the actual results. Keep all project standards
> and owner approval gates in place.

Describe a concrete behavior: “This error is confusing; help make it clearer,”
“Can we test what happens when this input is cut short?” or “Explain this bug and
help me fix it.” These are examples, not claims that a particular bug exists.
The agent should inspect the project, identify a real bounded task, explain its
acceptance criteria, implement the agreed change and show evidence.

A useful conversational loop:

1. Describe the desired behavior or problem in your own words.
2. The agent explains what it found, proposes a small change and surfaces important
   tradeoffs. Ask “what does that mean?” or change the goal naturally.
3. The agent edits and tests, explaining what passed, failed or could not run.
4. Review a plain-language summary plus the actual diff/result data. Ask for
   revisions, then have the agent prepare a branch and pull request.
5. Work through review feedback; approval and merging follow project policy.

Say “explain more,” “handle the commands for me,” “let me write this part,” or
“switch to concise expert mode” at any time. You can mix guided kernel concepts
with expert Git usage in the same session. No syntax quiz or mandatory manual
coding exercise is required. Generated code still needs the same tests and review;
an agent must never turn a conversational request into a false completion claim.
See [CONTRIBUTOR_GUIDE.md](CONTRIBUTOR_GUIDE.md) for the assistance scale and evidence.

## Path B: set up and work directly

The current AMD64 reference CI uses Ubuntu 24.04 on x86-64. Use a suitable Linux
development environment; on another host, arrange a compatible VM or separately
validate your environment before claiming equivalent results. ARM64 is a separate
cross-build/target path, not an automatic consequence of a successful x86 build.
The commands below are for Ubuntu/Debian-style package management, not every OS.

### 1. Get the tools

Review the packages first; these commands change the development host and need
administrator privileges. In guided mode the agent should inspect installed tools
and perform authorized setup rather than requiring you to understand each flag.

```sh
sudo apt-get update
sudo apt-get install git gcc binutils make python3 grub-pc-bin grub-common xorriso qemu-system-x86
```

Git tracks changes; GCC/binutils compile and link the kernel; make runs build
recipes; Python runs test tools; GRUB/xorriso construct the emulator boot image;
QEMU runs it. Installation alone is not proof the build or boot works.

### 2. Get a working copy and branch

For external contributions, create a GitHub fork (your copy of the project) using
GitHub's Fork action. Clone the fork URL GitHub gives you, or use this read-only
upstream checkout to explore and build:

```sh
git clone https://github.com/auto44e/jeios.git
cd jeios
git status --short
git switch -c contribute/my-first-change
```

A branch keeps a proposed change separate from main. Do not overwrite an existing
checkout or discard uncommitted work. If you cloned upstream without write access,
add your verified fork URL as a separate remote named `fork` before publishing;
never put an access token into a URL or a PR.

Read [AGENTS.md](../AGENTS.md), [CONTRIBUTING.md](../CONTRIBUTING.md),
[AGENT_START.md](AGENT_START.md), current HANDOFF and the relevant subsystem docs.
AGENTS.md is the instruction file coding agents must follow. An ADR records an
accepted architecture decision; an issue tracks work; a PR proposes changes for
review. Choose an open task with bounded scope and check for concurrent work.

### 3. Establish the baseline

```sh
git rev-parse HEAD
gcc --version
ld --version
python3 --version
qemu-system-x86_64 --version
make check
make smoke
make integration
```

`make check` builds and runs hosted/ELF checks. `make smoke` boots the normal image
in QEMU; the test intentionally reaches its terminal markers and halts, so do not
expect a desktop or shell. `make integration` runs deliberately failing kernel
scenarios and checks the expected failures. A timeout is not success.

Keep actual output and exit codes. Stop and investigate a failure before using it
as a baseline. If tools are missing, record what is blocked. A documented managed
environment LeakSanitizer restriction permits the narrow override explained in
[DEVELOPMENT.md](DEVELOPMENT.md); it does not permit claiming leak coverage.

### 4. Make one small change and test it

Use the repository's implementation rather than guessing from design aspirations.
Explain the expected behavior and failure case before editing. A documentation
clarification or regression test can be a good first contribution when it addresses
an actual need. Do not alter frozen contracts or tested paths without the required
owner decision. Run applicable CI profiles and focused tests from
[CONTRIBUTOR_GUIDE.md](CONTRIBUTOR_GUIDE.md); the assistance level never changes them.

```sh
git diff --check
git diff
```

The diff shows exactly what will change. Check for unrelated edits, secrets and
build outputs. Update the appropriate handoff/status documents with the tested
revision, commands and honest results. Documentation-only changes need not pretend
they exercised a new runtime path; explain inapplicable tests.

### 5. Submit for review

Stage only the files you intend to contribute, inspect the staged diff, and commit
with a short description. The agent can do this in guided mode. After the `fork`
remote points to your own repository, a typical publication sequence is:

```sh
git diff --cached
git commit -m "Describe the bounded change"
git push -u fork contribute/my-first-change
```

These assume you have staged your selected files; a commit with no staged changes
will not create the intended contribution. Open a PR from that branch into
`auto44e/jeios:main`. Complete the PR template with environment, exact revision,
commands, expected/observed results, sanitized logs and gaps. Compare remote CI
results to local results without treating either as a substitute for the other.
Respond to feedback and rerun affected tests after code changes. Do not self-merge
unless the owner has explicitly authorized it.

## Mobile contributors

Use [jeios-mobile](https://github.com/auto44e/jeios-mobile) for target metadata,
integration planning and parity qualification; shared kernel/ARM code belongs in
this repository. In a mobile checkout run:

```sh
python3 tools/validate.py
python3 tools/target_plan.py
python3 -m unittest discover -s tests -v
```

These validate planning, not phone execution. The mobile pinned core revision may
lag upstream; read its HANDOFF/config before claiming support. For actual ARM
kernel work, read mobile ARM64.md and ADR 0002, inspect `mk/aarch64.mk`, set up the
specified cross-toolchain/QEMU profile, then run the relevant ARM build/boot tests.
This first-contribution guide does not authorize phone unlock, flashing or disk
formatting. No AOSP lunch target is implied by the fresh kernel.
