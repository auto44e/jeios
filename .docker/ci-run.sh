#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Ephemeral-container task runner for JeiOS (issue #18, "curl-style" local CI).
#
# Builds (or reuses) the pinned builder image from .docker/Dockerfile, then runs
# ONE task inside a throwaway container that is removed on the normal path and on
# SIGINT/SIGTERM (a SIGKILL or host death cannot be trapped — see the honesty note
# below). The big, noisy build/test log is captured to a file so it can be handed
# to a peer agent for interpretation without flooding an orchestrator's context.
#
# This is the lead-orchestrated model: the HOST owns the container lifecycle;
# a peer, if used, only READS the resulting log file. No agent is granted the
# docker handle or a writable sandbox here.
#
# BUILD ISOLATION (fix-gate F02). By default the task runs on a CLEAN COPY of the
# tracked tree inside the container, with build outputs on a container-private
# volume — so a poisoned or stale host build/ CANNOT make a container test pass,
# and the result reflects a clean rebuild, not cached host/image binaries. Pass
# --in-tree to instead bind-mount your working tree read-write (fast local
# iteration; build/ lands in your tree) when you explicitly want that.
#
# Usage:
#   .docker/ci-run.sh check               # make check on a clean copy
#   .docker/ci-run.sh mutants             # the critical-mutant harness
#   .docker/ci-run.sh aarch64             # cross-build the ARM64 kernel
#   .docker/ci-run.sh soak [ITERS]        # repeat check+mutants ITERS times (default 20)
#   .docker/ci-run.sh shell               # interactive shell (implies --in-tree)
#   .docker/ci-run.sh [--in-tree] -- <cmd...>   # arbitrary command
#   .docker/ci-run.sh --in-tree check     # bind-mount the worktree instead of copying
#
# Env:
#   JEIOS_IMAGE   image tag (default: jeios-builder)
#   JEIOS_LOGDIR  where logs go (default: a fresh mktemp dir, path printed)
#   JEIOS_NOBUILD set to skip the image build/freshness check
#   DOCKER        docker binary (default: "docker")
#
# Exit status is the task's status, so it composes in CI and in `&&` chains.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
image="${JEIOS_IMAGE:-jeios-builder}"
docker="${DOCKER:-docker}"

in_tree=0
if [ "${1:-}" = "--in-tree" ]; then in_tree=1; shift; fi

task="${1:-check}"; shift || true

logdir="${JEIOS_LOGDIR:-$(mktemp -d /tmp/jeios-ci.XXXXXX)}"
mkdir -p "$logdir"
log="$logdir/${task}.log"

cname="jeios-ci-$$-$(date +%s)"
# Normal-path and catchable-signal cleanup only. A SIGKILL (kill -9) or a host
# crash cannot run a trap; --rm plus this handler cover every path we CAN observe,
# but we do not claim unconditional teardown after an uncatchable kill. On a
# trapped signal we clean up and then EXIT non-zero (do not fall through into,
# e.g., the next soak iteration).
cleanup() { "$docker" rm -f "$cname" >/dev/null 2>&1 || true; }
on_signal() { echo "[ci-run] interrupted — cleaning up container $cname" >&2; cleanup; exit 130; }
trap cleanup EXIT
trap on_signal INT TERM

build_image() {
    [ -n "${JEIOS_NOBUILD:-}" ] && return 0
    echo "[ci-run] building $image (cached layers reused)…" >&2
    "$docker" build -q -f "$repo/.docker/Dockerfile" -t "$image" "$repo" >/dev/null
}

# Run a command in a fresh container named "$cname" (so the trap can remove it).
# Isolated mode: copy the tracked tree into a container-private path and build
# there, outputs on an anonymous volume — nothing is shared with the host tree.
# In-tree mode: bind-mount the worktree read-write (opt-in).
in_container() {
    if [ "$in_tree" -eq 1 ]; then
        "$docker" run --rm --name "$cname" \
            --network none --user "$(id -u):$(id -g)" \
            -e HOME=/tmp -e ASAN_OPTIONS=detect_leaks=0 \
            -v "$repo":/src -w /src \
            "$image" "$@"
    else
        # Mount the worktree READ-ONLY at /host, copy tracked files to a private
        # writable dir under the run user's HOME (/tmp), build there. Nothing is
        # shared with the host tree. git is told /host is safe (ro bind mount).
        "$docker" run --rm --name "$cname" \
            --network none --user "$(id -u):$(id -g)" \
            -e HOME=/tmp -e ASAN_OPTIONS=detect_leaks=0 \
            -v "$repo":/host:ro -w /tmp \
            "$image" bash -c '
                set -e
                git config --global --add safe.directory /host 2>/dev/null || true
                # A clone gives /tmp/work a real .git so make mutants (which
                # snapshots via git) works, and guarantees a clean tree decoupled
                # from the host build/. For uncommitted local changes use --in-tree.
                if git clone -q --local /host /tmp/work 2>/dev/null; then :; \
                else mkdir -p /tmp/work && cp -a /host/. /tmp/work/; fi
                git config --global --add safe.directory /tmp/work 2>/dev/null || true
                cd /tmp/work
                exec "$@"
            ' _ "$@"
    fi
}

# Validate a soak iteration count: a bounded positive integer. Zero or garbage
# must NOT silently "succeed".
valid_count() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;      # non-numeric
        0|0*) [ "$1" -gt 0 ] 2>/dev/null || return 1 ;;
    esac
    [ "$1" -ge 1 ] && [ "$1" -le 100000 ]
}

build_image

case "$task" in
  check|mutants|aarch64|test|smoke)
    echo "[ci-run] task=$task  image=$image  isolated=$((1-in_tree))  log=$log" >&2
    if in_container make "$task" >"$log" 2>&1; then
        echo "[ci-run] $task OK  (full log: $log)"; tail -n 3 "$log"
    else
        rc=$?
        echo "[ci-run] $task FAILED rc=$rc  (full log: $log)" >&2
        tail -n 25 "$log" >&2
        exit "$rc"
    fi
    ;;

  soak)
    iters="${1:-20}"
    if ! valid_count "$iters"; then
        echo "[ci-run] soak: ITERS must be an integer in 1..100000, got '$iters'" >&2
        exit 2
    fi
    echo "[ci-run] soak: $iters iterations of check+mutants  log=$log" >&2
    : >"$log"
    fails=0
    for i in $(seq 1 "$iters"); do
        echo "=== soak iteration $i/$iters $(date -u +%FT%TZ) ===" >>"$log"
        if ! in_container bash -c 'make check && make mutants' >>"$log" 2>&1; then
            echo "ITER $i FAILED" >>"$log"; fails=$((fails+1))
        fi
    done
    echo "[ci-run] soak done: $((iters-fails))/$iters clean  (full log: $log)"
    [ "$fails" -eq 0 ] || { echo "[ci-run] $fails iteration(s) failed" >&2; exit 1; }
    ;;

  shell)
    # Interactive shell is inherently in-tree (you want to edit your worktree).
    in_tree=1
    echo "[ci-run] interactive shell in $image (repo at /src)…" >&2
    "$docker" run --rm -it --name "$cname" \
        --network none --user "$(id -u):$(id -g)" \
        -e HOME=/tmp -e ASAN_OPTIONS=detect_leaks=0 \
        -v "$repo":/src -w /src "$image" bash
    ;;

  --)
    echo "[ci-run] custom: $*  isolated=$((1-in_tree))  log=$log" >&2
    if in_container "$@" >"$log" 2>&1; then
        echo "[ci-run] custom OK  (full log: $log)"; tail -n 3 "$log"
    else
        rc=$?; echo "[ci-run] custom FAILED rc=$rc  (full log: $log)" >&2
        tail -n 25 "$log" >&2; exit "$rc"
    fi
    ;;

  *)
    echo "usage: $0 [--in-tree] {check|mutants|aarch64|test|smoke|soak [ITERS]|shell|-- <cmd...>}" >&2
    exit 2
    ;;
esac
