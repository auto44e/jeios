#!/usr/bin/env python3
"""Boot the normal kernel; validate profile and actual serial progress."""
import argparse
from emulator import boot
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("image")
parser.add_argument("--cpu", default="qemu64")
parser.add_argument("--memory", default="128M")
parser.add_argument("--expect-nx", choices=("present", "absent"))
args = parser.parse_args()
expected = [b"JeiOS: entered C kernel", b"M1: traps ready", b"M1: boot map validated", b"M1: protected mappings"]
if args.expect_nx:
    expected.append(b"CPU: NX enforced" if args.expect_nx == "present"
                    else b"CPU: legacy profile, NX absent")
print(boot(args.image, cpu=args.cpu, memory=args.memory, expected=expected))
print("QEMU smoke: PASS")
