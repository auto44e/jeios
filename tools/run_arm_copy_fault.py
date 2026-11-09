#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run one bounded ARM MP3b copy-fault negative proof."""

import os
import subprocess
import sys


FAULTS = {
    "exact-elr",
    "far-bound",
    "far-lower",
    "prefilter-stray",
    "owner",
    "direction",
    "permission",
    "l0",
    "live-root",
    "restore-selected",
    "save-path",
    "live-root-core",
    "load-permission",
    "guard-2",
}


def main():
    name = os.environ.get("ARM_COPY_FAULT", "")
    if name not in FAULTS:
        print("ARM_COPY_FAULT must name one supported copy-fault stage",
              file=sys.stderr)
        return 2
    build = f"build/aarch64-copyfault-{name}"
    command = [
        sys.executable,
        "tools/smoke_arm.py",
        f"{build}/Image",
        "--elf",
        f"{build}/jeios-arm.elf",
        "--log",
        f"{build}/serial.log",
        "--timeout",
        os.environ.get("ARM_SMOKE_TIMEOUT", "60"),
        "--el0-test",
        "--copy-fault",
        name,
        "--require",
    ]
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
