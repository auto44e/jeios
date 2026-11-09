#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the bounded ARM MP3a-1 user-root smoke proof for mutant testing."""

import subprocess
import sys


def main():
    command = [
        sys.executable,
        "tools/smoke_arm.py",
        "build/aarch64-usertest/Image",
        "--elf",
        "build/aarch64-usertest/jeios-arm.elf",
        "--log",
        "build/aarch64-usertest/serial.log",
        "--timeout",
        "60",
        "--user-test",
        "--require",
    ]
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
