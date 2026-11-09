#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the bounded ARM MP3a-2 EL0/SVC/fault-death smoke proof."""

import subprocess
import sys


def main():
    command = [
        sys.executable,
        "tools/smoke_arm.py",
        "build/aarch64-el0test/Image",
        "--elf",
        "build/aarch64-el0test/jeios-arm.elf",
        "--log",
        "build/aarch64-el0test/serial.log",
        "--timeout",
        "60",
        "--el0-test",
        "--require",
    ]
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
