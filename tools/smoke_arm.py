#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
MP1 (ADR 0002, issue #52): boot the ARM64 Image on the pinned QEMU virt-8.2 /
cortex-a72 target and assert the ordered boot markers, including the bounded
DTB copy/parse/validate markers.

Discipline (mirrors the x86 smoke harness + the ADR 0002 D6 evidence rule):
  - capture serial to a FILE, never stdout (stdout races the timeout kill and
    looks empty even when the UART produced output); --log keeps that file;
  - require the exact ordered markers and the terminal line; a halted emulator
    with no markers is a FAILURE, not success;
  - reject any "FATAL" line (in --dtb-bad mode: only the one expected FATAL);
  - -no-reboot so a triple-fault/reset can't masquerade as a hang;
  - the machine string and -m are PINNED to exactly what
    tools/gen_dtb_fixture.py dumped, and the boot's printed CRC32 of the copied
    live DTB extent must equal QEMU_VIRT82_DTB_CRC32 from the committed fixture
    — proving the DTB that booted is byte-identical to the one the hosted
    parser tests use;
  - record the image SHA-256, tool versions, and the exact QEMU command;
  - SKIP (exit 0, clearly labelled) if the aarch64 toolchain /
    qemu-system-aarch64 / image is unavailable — never report PASS when nothing
    ran. With --require (remote CI) every such SKIP is a FAIL (exit 1).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

QEMU = "qemu-system-aarch64"
TOOLCHAIN_CC = "aarch64-linux-gnu-gcc"
# Pinned to match tools/gen_dtb_fixture.py exactly (the DTB depends on every one
# of these: dtb-randomness=off removes rng-seed/kaslr-seed; -m sets /memory).
MACHINE = "virt-8.2,gic-version=2,virtualization=off,secure=off,dtb-randomness=off"
CPU = "cortex-a72"
MEMORY = "256M"
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                       "tests", "fixtures", "qemu_virt82_dtb.h")
JE_FDT_MAX_COPY = 64 * 1024

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
VALIDATED = b"JeiOS/aarch64: boot context validated pa_bits="
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

COPIED_RE = re.compile(
    rb"JeiOS/aarch64: dtb copied len=0x([0-9a-f]{16}) totalsize=0x([0-9a-f]{16})"
    rb" crc32=0x([0-9a-f]{16}) crc_len=0x([0-9a-f]{16})\r?\n")


def hx(v):
    return b"0x%016x" % v


class Fixture:
    """Values parsed from the committed generated fixture header."""

    def __init__(self, path):
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()

        def define(name):
            m = re.search(r"#define\s+%s\s+(0x[0-9a-fA-F]+|\d+)u?\b" % name, src)
            if not m:
                raise ValueError(f"{name} not found in {path}")
            return int(m.group(1), 0)

        self.crc32 = define("QEMU_VIRT82_DTB_CRC32")
        self.length = define("QEMU_VIRT82_DTB_LEN")
        self.totalsize = define("QEMU_VIRT82_DTB_TOTALSIZE")
        body = src.split("{", 1)[1]
        hdr = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)[:40])
        if len(hdr) != 40:
            raise ValueError(f"fixture {path}: fewer than 40 header bytes")
        self.off_dt_struct = int.from_bytes(hdr[8:12], "big")


def tool_version(cmd):
    try:
        return subprocess.check_output([cmd, "--version"], text=True,
                                       stderr=subprocess.STDOUT).splitlines()[0]
    except Exception:
        return "(unknown)"


def fail(msg):
    print(f"QEMU aarch64 smoke: FAIL ({msg})")
    sys.exit(1)


def check_ordered(out, markers):
    pos = 0
    for m in markers:
        idx = out.find(m, pos)
        if idx < 0:
            fail(f"missing/out-of-order marker: {m.decode()}")
        pos = idx + len(m)


def check_dtb_copied(out, fx):
    """The copied line must exist once and its CRC must match the fixture."""
    found = COPIED_RE.findall(out)
    if len(found) != 1:
        fail(f"expected exactly one well-formed 'dtb copied' line, found {len(found)}")
    length, total, crc, crc_len = (int(x, 16) for x in found[0])
    want_len = min(fx.totalsize, JE_FDT_MAX_COPY)
    if total != fx.totalsize or length != want_len:
        fail(f"dtb copied len=0x{length:x} totalsize=0x{total:x}; fixture expects "
             f"len=0x{want_len:x} totalsize=0x{fx.totalsize:x}")
    if crc_len != fx.length:
        fail(f"dtb crc_len=0x{crc_len:x} != fixture live extent 0x{fx.length:x}")
    if crc != fx.crc32:
        fail(f"booted DTB crc32=0x{crc:08x} != fixture QEMU_VIRT82_DTB_CRC32="
             f"0x{fx.crc32:08x} (booted DTB differs from the hosted-test fixture)")
    print(f"dtb:   crc32=0x{crc:08x} over 0x{crc_len:x} bytes == fixture")


def dtb_good_markers():
    # Exact for the pinned machine/-m: fixture has an empty memreserve map, no
    # /reserved-memory, no initrd, one /memory reg [0x40000000, +256 MiB).
    return [
        b"JeiOS/aarch64: dtb copied len=",
        b"JeiOS/aarch64: dtb parsed ram=1 rsv=0 ignored=0 dynamic=0 initrd=0\r\n",
        b"JeiOS/aarch64:   ram[0] base=" + hx(0x40000000) + b" size=" + hx(0x10000000),
        VALIDATED,
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--timeout", type=int, default=15)
    ap.add_argument("--bss-verified", action="store_true",
                    help="expect the patterned-BSS 'bss clear verified' marker")
    ap.add_argument("--vector-test", action="store_true",
                    help="expect the deliberate-BRK vector self-test markers")
    ap.add_argument("--dtb-bad", action="store_true",
                    help="expect the JE_ARM_DTB_BAD parser rejection (BAD_TOKEN)")
    ap.add_argument("--require", action="store_true",
                    help="missing QEMU/toolchain/image is a FAIL, not a SKIP")
    ap.add_argument("--log", metavar="PATH",
                    help="write the raw serial log here (kept after the run)")
    args = ap.parse_args()

    def skip(msg):
        if args.require:
            fail(f"--require: {msg}")
        print(f"QEMU aarch64 smoke: SKIP ({msg})")
        sys.exit(0)

    if args.dtb_bad and args.vector_test:
        fail("--dtb-bad and --vector-test are mutually exclusive (the DTB "
             "rejection halts before the BRK)")
    if shutil.which(QEMU) is None:
        skip(f"{QEMU} not installed")
    if shutil.which(TOOLCHAIN_CC) is None:
        skip(f"{TOOLCHAIN_CC} not installed")
    if not os.path.exists(args.image):
        skip(f"image {args.image} not built")
    try:
        fx = Fixture(FIXTURE)
    except (OSError, ValueError) as e:
        fail(f"cannot read DTB fixture: {e}")

    markers = list(BASE_MARKERS)
    markers[3] = (b"JeiOS/aarch64: bss clear verified" if args.bss_verified
                  else b"JeiOS/aarch64: bss cleared")
    bad_line = (b"JeiOS/aarch64: FATAL dtb rejected status=BAD_TOKEN off="
                + hx(fx.off_dt_struct) + b"\r\n")
    if args.dtb_bad:
        markers += [
            b"JeiOS/aarch64: dtb copied len=",
            b"JeiOS/aarch64: dtb-bad test: wrote token 0x0000000a at off="
            + hx(fx.off_dt_struct),
            bad_line,
        ]
    else:
        markers += dtb_good_markers()
        markers += VECTOR_TEST_MARKERS if args.vector_test else [TERM_DEFAULT]

    # Evidence: image hash + tool versions.
    with open(args.image, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    print(f"image: {args.image}  sha256={sha}")
    print(f"qemu:  {tool_version(QEMU)}")
    print(f"cc:    {tool_version(TOOLCHAIN_CC)}")
    print(f"fixture: {os.path.normpath(FIXTURE)} crc32=0x{fx.crc32:08x} "
          f"len={fx.length} off_dt_struct=0x{fx.off_dt_struct:x}")

    if args.log:
        serial_path = args.log
        os.makedirs(os.path.dirname(os.path.abspath(serial_path)), exist_ok=True)
        if os.path.exists(serial_path):
            os.unlink(serial_path)  # never assert on a previous run's output
    else:
        with tempfile.NamedTemporaryFile(prefix="jeios-arm-serial-", suffix=".log",
                                         delete=False) as tf:
            serial_path = tf.name

    cmd = [QEMU, "-M", MACHINE, "-cpu", CPU, "-smp", "1", "-m", MEMORY,
           "-display", "none", "-serial", f"file:{serial_path}",
           "-no-reboot", "-kernel", args.image]
    print("cmd:   " + " ".join(cmd))

    try:
        subprocess.run(cmd, timeout=args.timeout, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        pass  # expected: the kernel halts in WFE; the timeout stops QEMU.

    try:
        with open(serial_path, "rb") as f:
            out = f.read()
    except OSError as e:
        fail(f"no serial log produced: {e}")
    if args.log:
        print(f"log:   {serial_path}")
    else:
        os.unlink(serial_path)

    sys.stdout.write(out.decode("ascii", "replace"))
    sys.stdout.write("\n")

    if args.dtb_bad:
        # Exactly one FATAL, and it must be the expected parser rejection.
        fatal_lines = [ln for ln in out.split(b"\n") if b"FATAL" in ln]
        if fatal_lines != [bad_line.rstrip(b"\n")]:
            fail(f"--dtb-bad: expected exactly the BAD_TOKEN FATAL line, got "
                 f"{[ln.decode('ascii', 'replace') for ln in fatal_lines]}")
        for forbidden in (b"JeiOS/aarch64: dtb parsed", VALIDATED, TERM_DEFAULT):
            if forbidden in out:
                fail(f"--dtb-bad: forbidden marker present: {forbidden.decode()}")
    elif b"FATAL" in out:
        fail("FATAL marker present")

    check_ordered(out, markers)
    # In every mode the copy precedes the parse; its CRC is over the copy taken
    # BEFORE the dtb-bad injection, so it must match the fixture in all modes.
    check_dtb_copied(out, fx)

    print("QEMU aarch64 smoke: PASS")


if __name__ == "__main__":
    main()
