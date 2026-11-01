#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot the normal kernel; validate profile and actual serial progress."""
import argparse
from emulator import boot
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("image")
parser.add_argument("--cpu", default="qemu64")
parser.add_argument("--memory", default="128M")
parser.add_argument("--expect-nx", choices=("present", "absent"))
args = parser.parse_args()
expected = [b"JeiOS: entered C kernel", b"M1: traps ready", b"M1: boot map validated", b"M1: protected mappings",
            b"M2: preemption/GPR/sleep/heap checks passed",
            b"M2: exited threads reaped; page accounting restored",
            b"M2: thread capacity exhausted at T="]
if args.expect_nx:
    expected.append(b"CPU: NX enforced" if args.expect_nx == "present"
                    else b"CPU: legacy profile, NX absent")
    # The ring-3 M3c self-tests (s4-s7) only run on the NX-present profile. Assert
    # the final one so deleting/guarding-out the M3c step-7 bootstrap fixture fails
    # the smoke run rather than passing silently.
    if args.expect_nx == "present":
        expected.append(b"M3c-s7: bootstrap fixture OK")
        expected.append(b"M4b-s1: endpoint creation quota/charge release OK")
        expected.append(b"M4b-s1: PROCESS record status/typed dispatch OK")
        expected.append(b"M4c-s1: spawn/LAUNCHER/startup-v1/CLOCK_TICKS OK; negative baselines OK; quota validation order OK; fail-Nth rollback OK (steps 1-5); spawn cycles=")
        expected.append(b"M4d-s1: PROC_KILL OK (READY, BLOCKED_IPC, WOKEN-reserved, EXIT first-reason, supervisor guard, self/after-death/unlinked/rights, next-syscall PROC_STATUS)")
        expected.append(b"M4e-s1: ring-3 C image OK (startup v1, rodata/data/bss, CLOCK_TICKS, READY, exit 0); image sha256=")
if args.expect_nx != "absent":
    expected.append(b"M4f-s1: supervisor scenario completed")
    expected.append(b"M4f: zero-leak identity baseline verified")
terminal = (b"M4f: zero-leak identity baseline verified"
            if args.expect_nx != "absent"
            else b"initialization complete; halted")
print(boot(args.image, cpu=args.cpu, memory=args.memory, terminal=terminal,
           expected=expected))
print("QEMU smoke: PASS")

