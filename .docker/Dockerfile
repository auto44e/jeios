# SPDX-License-Identifier: Apache-2.0
#
# JeiOS reproducible builder image (issue #18, CI foundation).
#
# One pinned toolchain for all three CI jobs AND for local parity, so a
# contributor's `make check` behaves identically to CI instead of inheriting
# whatever versions a moving `ubuntu-24.04` runner happens to ship. The base is
# pinned BY DIGEST (not just the :24.04 tag) so the image is reproducible even if
# the tag is re-pointed upstream.
#
# This pins the SOFTWARE toolchain and the emulator only. It does NOT add
# real-hardware coverage: QEMU here runs in TCG mode (no KVM in most CI
# containers), exactly as the existing CI does, and the standing
# "no physical-board qualification" boundary (AGENTS.md rule 12, STANDARDS.md)
# is unchanged.
#
# Build (from the repo root):
#   docker build -f .docker/Dockerfile -t jeios-builder .
# Run a check (repo mounted read-write so build/ lands in your tree):
#   docker run --rm -v "$PWD":/src -w /src jeios-builder make check
# See .docker/README.md for the full local workflow and the ci-run.sh wrapper.

# ubuntu:24.04 (Noble) — matches the ubuntu-24.04 runner family CI used before.
FROM ubuntu@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

# Fail the IMAGE BUILD if the toolchain drifts, rather than discovering it at
# test time. The existing aarch64 CI job asserts QEMU 8.2.x at runtime; here that
# assertion moves to build time and becomes a property of the image.
ARG EXPECTED_QEMU_MAJORMINOR=8.2

ENV DEBIAN_FRONTEND=noninteractive

# Union of the three CI jobs' toolchains:
#   x86 kernel + ISO:   gcc binutils make python3 grub-pc-bin grub-common xorriso qemu-system-x86
#   aarch64 cross:      gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-arm
# gcov ships with gcc; ASan/UBSan ship with gcc's runtime. No clang/cppcheck/
# libFuzzer (gcc rejects -fsanitize=fuzzer) — the mutant+bounded-fuzz approach in
# tools/ is what the project actually uses. libc6-dev provides the hosted C
# headers (stdio.h/stdint.h) the hosted test suite compiles against; the
# freestanding kernel build itself does not use them. git is required by
# tools/mutants.py (it snapshots the tree with `git archive` for isolation).
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        gcc binutils libc6-dev make python3 git \
        grub-pc-bin grub-common xorriso qemu-system-x86 \
        gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-arm \
    && rm -rf /var/lib/apt/lists/* \
    && qv="$(qemu-system-aarch64 --version | awk 'NR==1{print $4}')" \
    && case "$qv" in \
         ${EXPECTED_QEMU_MAJORMINOR}.*) : ;; \
         *) echo "FATAL: QEMU $qv is not ${EXPECTED_QEMU_MAJORMINOR}.x (ADR 0002 pins virt-8.2); rebuild against a base image that ships ${EXPECTED_QEMU_MAJORMINOR}.x" >&2; exit 1 ;; \
       esac \
    && echo "builder image toolchain:" \
    && gcc --version | head -1 \
    && aarch64-linux-gnu-gcc --version | head -1 \
    && qemu-system-x86_64 --version | head -1 \
    && qemu-system-aarch64 --version | head -1

# The repo is mounted at run time (not COPYed), so the image stays source-free
# and one image serves every branch/working tree.
WORKDIR /src

# Record the toolchain versions INTO the image so `docker run jeios-builder
# cat /etc/jeios-toolchain` reproduces the exact evidence a PR should cite.
# NOTE on reproducibility (fix-gate F02): the BASE is pinned by digest, but the
# apt packages below come from moving Ubuntu repositories and are NOT version-
# pinned, and the QEMU assertion only constrains major.minor. This image gives a
# PINNED BASE + RECORDED package versions, which is reproducible-enough for a
# consistent toolchain and local/CI parity — it is NOT a bit-for-bit clean-build
# guarantee. The recorded dpkg versions below are the evidence a PR cites; to get
# a stronger guarantee, pin each package against an immutable snapshot source.
RUN { echo "# JeiOS builder image toolchain (captured at image build)"; \
      echo "base=ubuntu@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852"; \
      echo "host_arch=$(dpkg --print-architecture)"; \
      echo "gcc=$(gcc -dumpfullversion)"; \
      echo "aarch64-gcc=$(aarch64-linux-gnu-gcc -dumpfullversion)"; \
      echo "qemu-x86=$(qemu-system-x86_64 --version | awk 'NR==1{print $4}')"; \
      echo "qemu-aarch64=$(qemu-system-aarch64 --version | awk 'NR==1{print $4}')"; \
      echo "python3=$(python3 --version | awk '{print $2}')"; \
      echo "# exact dpkg package versions (reproducibility evidence):"; \
      dpkg-query -W -f='pkg ${Package} ${Version}\n' \
        gcc binutils libc6-dev make python3 git \
        grub-pc-bin grub-common xorriso qemu-system-x86 \
        gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-arm; \
    } > /etc/jeios-toolchain

CMD ["make", "check"]
