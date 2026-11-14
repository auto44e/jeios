#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
case "${1:-}" in
  check|aarch64|integration|mutants)
    command -v docker >/dev/null || { echo "error: Install and start Docker, then retry." >&2; exit 1; }
    # The existing builder also compiles AMD64 hosted/assembly paths.
    # On Apple Silicon use Docker's AMD64 emulation; never compile kernel as Mach-O.
    export DOCKER_DEFAULT_PLATFORM=linux/amd64
    exec bash .docker/ci-run.sh -- make "$1"
    ;;
  *) echo "error: Unknown Xcode task: ${1:-<missing>}" >&2; exit 2 ;;
esac
