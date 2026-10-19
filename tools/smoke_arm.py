#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
MP1 step 1 (ADR 0002): boot the ARM64 Image on the pinned QEMU virt-8.2 /
cortex-a72 target and assert the ordered boot markers.

Discipline (mirrors the x86 smoke harness + the ADR 0002 D6 evidence rule):
  - capture serial to a FILE, never stdout (stdout races the timeout kill and
    looks empty even when the UART produced output);
  - require the exact ordered markers and the terminal line; a halted emulator
    with no markers is a FAILURE, not success;
  - reject any "FATAL" line;
  - -no-reboot so a triple-fault/reset can't masquerade as a hang;
  - record the image SHA-256, tool versions, and the exact QEMU command;
  - SKIP (exit 0, clearly labelled) if the aarch64 toolchain / qemu-system-aarch64
    is unavailable — never report PASS when nothing ran.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

QEMU = "qemu-system-aarch64"
MACHINE = "virt-8.2"
CPU = "cortex-a72"

# Ordered step-1 markers. "bss cleared" (default) vs "bss clear verified"
# (patterned-BSS build) — accept either, but if the verified form is present it
# must be the verified one; see --bss-verified.
BASE_MARKERS = [
    b"JeiOS/aarch64: entry EL1",
    b"JeiOS/aarch64: stack ready",
    b"JeiOS/aarch64: tcr_el1 initialized (tbi0=0)",
    None,  # bss line, filled below
    b"JeiOS/aarch64: dtb pointer recorded",
    b"JeiOS/aarch64: vectors installed",
]
# Default build terminal marker (no vector test).
TERM_DEFAULT = b"JeiOS/aarch64: initialization complete; halted"
# Vector-test build: the planted BRK must vector to the EL1h sync slot and the
# handler must assert the exact syndrome. These ordered markers replace the
# default terminal and prove the deliberate exception end-to-end.
VECTOR_TEST_MARKERS = [
    b"JeiOS/aarch64: triggering deliberate BRK",
    b"JeiOS/aarch64: EXCEPTION vector=0x0000000000000004 ec=0x000000000000003c",
    # Assert the RAW syndrome line independently (not just the in-kernel PASS), so
    # a bug in the kernel's own comparator cannot produce a false PASS: exact
    # ESR/ELR-shape/SPSR observed on the wire.
    b"esr=0x00000000f200a55a",
    b"spsr=0x00000000800003c5",
    b"JeiOS/aarch64: vector self-test PASS",
    b"JeiOS/aarch64: exception handled; halted",
]


def skip(msg):
    print(f"QEMU aarch64 smoke: SKIP ({msg})")
    sys.exit(0)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--memory", default="256M")
    ap.add_argument("--timeout", type=int, default=15)
    ap.add_argument("--bss-verified", action="store_true",
                    help="expect the patterned-BSS 'bss clear verified' marker")
    ap.add_argument("--vector-test", action="store_true",
                    help="expect the deliberate-BRK vector self-test markers")
    args = ap.parse_args()

    if shutil.which(QEMU) is None:
        skip(f"{QEMU} not installed")
    if not os.path.exists(args.image):
        skip(f"image {args.image} not built")

    markers = list(BASE_MARKERS)
    markers[3] = (b"JeiOS/aarch64: bss clear verified" if args.bss_verified
                  else b"JeiOS/aarch64: bss cleared")
    if args.vector_test:
        markers += VECTOR_TEST_MARKERS
    else:
        markers.append(TERM_DEFAULT)

    # Evidence: image hash + tool versions.
    with open(args.image, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    try:
        qver = subprocess.check_output([QEMU, "--version"], text=True).splitlines()[0]
    except Exception:
        qver = "(unknown)"
    print(f"image: {args.image}  sha256={sha}")
    print(f"qemu:  {qver}")

    with tempfile.NamedTemporaryFile(prefix="jeios-arm-serial-", suffix=".log",
                                     delete=False) as tf:
        serial_path = tf.name

    cmd = [QEMU, "-M", MACHINE, "-cpu", CPU, "-smp", "1", "-m", args.memory,
           "-display", "none", "-serial", f"file:{serial_path}",
           "-no-reboot", "-kernel", args.image]
    print("cmd:   " + " ".join(cmd))

    try:
        subprocess.run(cmd, timeout=args.timeout, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        pass  # expected: the kernel halts in WFE; the timeout stops QEMU.

    with open(serial_path, "rb") as f:
        out = f.read()
    os.unlink(serial_path)

    sys.stdout.write(out.decode("ascii", "replace"))
    sys.stdout.write("\n")

    if b"FATAL" in out:
        print("QEMU aarch64 smoke: FAIL (FATAL marker present)")
        sys.exit(1)

    pos = 0
    for m in markers:
        idx = out.find(m, pos)
        if idx < 0:
            print(f"QEMU aarch64 smoke: FAIL (missing/out-of-order marker: "
                  f"{m.decode()})")
            sys.exit(1)
        pos = idx + len(m)

    print("QEMU aarch64 smoke: PASS")


if __name__ == "__main__":
    main()
