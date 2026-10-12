#!/usr/bin/env python3
"""Boot GRUB in QEMU and require serial evidence, never treat a timeout as success."""
import argparse
import subprocess
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("image")
parser.add_argument("--cpu", default="qemu64")
parser.add_argument("--expect-nx", choices=("present", "absent"))
args = parser.parse_args()
cmd = ["qemu-system-x86_64", "-machine", "pc", "-cpu", args.cpu, "-m", "128M",
       "-cdrom", args.image, "-display", "none", "-serial", "stdio", "-monitor", "none",
       "-no-reboot", "-no-shutdown", "-nic", "none"]
try:
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=12)
    output = r.stdout
except subprocess.TimeoutExpired as exc:
    output = exc.stdout or b""
print(output.decode(errors="replace"))
assert b"JeiOS M0: entered C kernel" in output, "C entry not observed"
assert b"JeiOS M0: initialization complete; halted" in output, "successful init not observed"
assert b"JeiOS fatal:" not in output, "kernel reported fatal error"
if args.expect_nx:
    marker = (b"CPU: NX present (not enabled in M0)" if args.expect_nx == "present"
              else b"CPU: legacy profile, NX absent")
    assert marker in output, "expected CPU profile was not reported"
print("QEMU smoke: PASS (serial boot markers; not an isolation or hardware certification)")
