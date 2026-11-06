#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Ephemeral-container task runner for JeiOS (issue #18, "curl-style" local CI).
#
# Builds (or reuses) the pinned builder image from .docker/Dockerfile, then runs
# ONE task inside a throwaway container that is ALWAYS removed afterwards (even on
# failure or Ctrl-C). The big, noisy build/test log is captured to a file so it
# can be handed to a peer agent for interpretation without flooding an
# orchestrator's context — the "agent as the CI brain" pattern: a human or a peer
# reads the short verdict, the file holds the full evidence.
#
# This is the lead-orchestrated model: the HOST owns the container lifecycle
# (create + guaranteed teardown); a peer, if used, only READS the resulting log
# file. No agent is granted the docker handle or a writable sandbox.
#
# Usage:
#   .docker/ci-run.sh check               # make check in a clean container
#   .docker/ci-run.sh mutants             # make mutants
#   .docker/ci-run.sh aarch64             # make aarch64 (cross-build only)
#   .docker/ci-run.sh soak [ITERS]        # repeat check+mutants ITERS times (default 20)
#   .docker/ci-run.sh shell               # interactive shell in the container
#   .docker/ci-run.sh -- <cmd...>         # run an arbitrary command in the container
#
# Env:
#   JEIOS_IMAGE   image tag (default: jeios-builder)
#   JEIOS_LOGDIR  where logs go (default: a fresh mktemp dir, path printed)
#   JEIOS_NOBUILD set to skip the image build/freshness check
#   DOCKER        docker invocation (default: "docker"; e.g. "sg docker -c" users
#                 should instead run this whole script under `sg docker -c`)
#
# Exit status is the task's status, so it composes in CI and in `&&` chains.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
image="${JEIOS_IMAGE:-jeios-builder}"
docker="${DOCKER:-docker}"

task="${1:-check}"; shift || true

logdir="${JEIOS_LOGDIR:-$(mktemp -d /tmp/jeios-ci.XXXXXX)}"
mkdir -p "$logdir"
log="$logdir/${task}.log"

cname="jeios-ci-$$-$(date +%s)"
cleanup() { "$docker" rm -f "$cname" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

build_image() {
    [ -n "${JEIOS_NOBUILD:-}" ] && return 0
    echo "[ci-run] building $image (cached layers reused)…" >&2
    "$docker" build -q -f "$repo/.docker/Dockerfile" -t "$image" "$repo" >/dev/null
}

# Run a command in a fresh, auto-removed container. The repo is mounted
# read-write so build/ lands in the tree; --user keeps output owned by the host
# user (no root-owned build/ droppings). No network by default: the build and
# the hosted/QEMU-TCG tests need none, so --network none shrinks the blast radius.
in_container() {
    "$docker" run --rm --name "$cname" \
        --network none \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp -e ASAN_OPTIONS=detect_leaks=0 \
        -v "$repo":/src -w /src \
        "$image" "$@"
}

build_image

case "$task" in
  check|mutants|aarch64|test|smoke)
    echo "[ci-run] task=$task  image=$image  log=$log" >&2
    if in_container make "$task" >"$log" 2>&1; then
        echo "[ci-run] $task OK  (full log: $log)"; tail -n 3 "$log"
    else
        rc=$?
        echo "[ci-run] $task FAILED rc=$rc  (full log: $log)" >&2
        tail -n 25 "$log" >&2
        exit $rc
    fi
    ;;

  soak)
    iters="${1:-20}"
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
    echo "[ci-run] interactive shell in $image (repo at /src)…" >&2
    in_container bash
    ;;

  --)
    echo "[ci-run] custom: $*  log=$log" >&2
    if in_container "$@" >"$log" 2>&1; then
        echo "[ci-run] custom OK  (full log: $log)"; tail -n 3 "$log"
    else
        rc=$?; echo "[ci-run] custom FAILED rc=$rc  (full log: $log)" >&2
        tail -n 25 "$log" >&2; exit $rc
    fi
    ;;

  *)
    echo "usage: $0 {check|mutants|aarch64|test|smoke|soak [ITERS]|shell|-- <cmd...>}" >&2
    exit 2
    ;;
esac
