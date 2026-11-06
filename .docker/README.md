# JeiOS reproducible builder container

A single **pinned** toolchain image for building and testing JeiOS, usable both
locally and in CI, so `make check` behaves identically everywhere instead of
inheriting whatever versions a moving runner image ships. Part of the CI
foundation (issue #18).

## What it pins

The base is pinned **by digest** (`ubuntu@sha256:fe81560990fae86dc…`), and the
image install step **fails the build** if QEMU is not 8.2.x (ADR 0002 pins
`virt-8.2`) — the assertion the aarch64 CI job used to make at runtime becomes a
property of the image. Captured toolchain at build (see `/etc/jeios-toolchain`
inside the image): gcc 13.3, aarch64 cross-gcc 13.3, QEMU 8.2.2 (x86 + aarch64),
python3 3.12, plus the exact dpkg package versions.

**Reproducibility, stated honestly (fix-gate F02).** This is a *pinned base +
recorded package versions*, not a bit-for-bit clean-build guarantee: the base
image is digest-pinned, but the `apt-get install` packages come from moving
Ubuntu repositories and are **not** version-pinned, and the QEMU assertion
constrains only major.minor. `/etc/jeios-toolchain` records the exact versions an
image actually built with, which is the evidence a PR should cite. For a stronger
guarantee, pin each package against an immutable snapshot source. "Cached-image
parity" (reusing an already-built image) is not the same as a clean rebuild.

It pins the **software** toolchain and the emulator only. It adds **no**
real-hardware coverage: QEMU runs in TCG mode (no KVM in CI containers), exactly
as the existing CI does, and the standing "no physical-board qualification"
boundary is unchanged.

## Build it

From the repo root:

```sh
docker build -f .docker/Dockerfile -t jeios-builder .
```

## Run a task

The repo is mounted at run time (the image is source-free), so one image serves
any branch/working tree. The helper `ci-run.sh` wraps the lifecycle: it builds
(or reuses) the image, runs ONE task in a throwaway container, runs as your host
UID so `build/` is not left root-owned, uses `--network none`, and writes the
full log to a file while printing a short verdict.

By default the task runs **isolated**: `ci-run.sh` clones the tracked tree into a
container-private dir and builds there, so a poisoned/stale host `build/` cannot
make a test pass and the result is a clean rebuild. Pass `--in-tree` to instead
bind-mount your working tree read-write (fast local iteration; `build/` lands in
your tree, and uncommitted changes are included).

```sh
.docker/ci-run.sh check            # make check, isolated clean copy
.docker/ci-run.sh mutants          # the critical-mutant harness, isolated
.docker/ci-run.sh aarch64          # cross-build the ARM64 kernel
.docker/ci-run.sh soak 50          # 50x check+mutants (flake/soak hunting)
.docker/ci-run.sh --in-tree check  # bind-mount the worktree instead of copying
.docker/ci-run.sh shell            # interactive shell, repo at /src (in-tree)
.docker/ci-run.sh -- make test     # arbitrary command
```

Cleanup is guaranteed on the normal path and on SIGINT/SIGTERM (the trap removes
the container and exits non-zero rather than, say, continuing a soak loop). A
`kill -9` or a host crash cannot run a trap, so unconditional teardown after an
uncatchable kill is **not** claimed; `docker run --rm` plus the trap cover every
path that can be observed. An invalid or zero soak count is rejected, not treated
as success.

Or run the image directly without the helper (bind-mount / in-tree style):

```sh
docker run --rm --network none --user "$(id -u):$(id -g)" \
  -e HOME=/tmp -e ASAN_OPTIONS=detect_leaks=0 \
  -v "$PWD":/src -w /src jeios-builder make check
```

If your shell is not yet in the `docker` group (freshly added — needs a new
login), run the helper under `sg docker -c '…'` in the meantime.

## The "agent as the CI brain" pattern

`ci-run.sh` separates *running* the work from *interpreting* it. The host owns the
container lifecycle (create + guaranteed teardown); the big log lands in a file.
A peer coding agent can then be pointed at that **file** to interpret it —
"which mutants survived and with what seeds", "is this a hang or a crash",
"did a soak iteration regress" — so a large, noisy log is digested by an agent's
large context instead of flooding an orchestrator's. In this model **no agent is
granted the docker handle or a writable sandbox**: peers read the resulting log,
they do not drive containers. Granting a peer its own containerized sandbox
(so the peer itself spins up and tears down the box) is a deliberate, separately
approved escalation and is intentionally not wired here.

## Why a container and not Kubernetes

JeiOS CI is a short, stateless batch job (compile + QEMU-TCG smoke, minutes), not
a fleet of long-running services. A single pinned image gives reproducibility and
local parity; an orchestrator (K8s) would add a control plane and manifests to
maintain for no benefit at this scale.
