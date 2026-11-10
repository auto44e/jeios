#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run one isolated MP3c ARM IPC fault image through the smoke checker."""

import os
import subprocess
import sys


def main():
    fault = os.environ.get("ARM_IPC_FAULT", "")
    if not fault:
        print("ARM_IPC_FAULT is required", file=sys.stderr)
        return 2
    build = "build/aarch64-ipcfault-" + fault
    timeout = os.environ.get("ARM_SMOKE_TIMEOUT", "60")
    command = [
        sys.executable,
        "tools/smoke_arm.py",
        build + "/Image",
        "--elf",
        build + "/jeios-arm.elf",
        "--log",
        build + "/serial.log",
        "--timeout",
        timeout,
        "--el0-test",
        "--require",
        "--ipc-fault",
        fault,
    ]
    expected_fatal = os.environ.get("ARM_IPC_EXPECTED_FATAL")
    if expected_fatal is None:
        return subprocess.run(command, check=False).returncode

    result = subprocess.run(command, check=False, capture_output=True)
    output = result.stdout + result.stderr
    marker = ("JeiOS/aarch64: FATAL " + expected_fatal).encode("ascii")
    if result.returncode != 0 and marker in output.splitlines():
        # The IPC smoke checker intentionally exits nonzero after observing the
        # expected kernel halt.  Mutation preflight needs that named halt to be
        # treated as the clean baseline; any missing/wrong fatal remains nonzero.
        sys.stdout.buffer.write(output)
        return 0
    sys.stdout.buffer.write(output)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
