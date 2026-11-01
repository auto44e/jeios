#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and execute independent fault-injection kernels (never contaminate normal build)."""
import subprocess
from pathlib import Path
from emulator import boot

for mode, vector, error in [(1, 6, 0), (2, 13, 0x38), (3, 8, 0), (4, 14, 3), (5, 14, 0), (6, 14, 17), (7, 8, 0),
                            (8, 14, 0), (9, 14, 3)]:
    result = subprocess.run(["make", "iso", f"TEST_MODE={mode}"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    assert result.returncode == 0, result.stdout
    expected = [b"M1: traps ready", f"PANIC vector=0x{vector:016x} error=0x{error:016x}".encode()]
    if mode >= 4:
        expected.append(b"M1: protected mappings and stack guards ready")
    if mode in (4, 5, 6):
        symbols = subprocess.check_output(["nm", f"build/{mode}/jeios.elf"], text=True)
        symbol_name = {4: "__kernel_start", 5: "je_boot_guard", 6: "boot"}[mode]
        address = next(int(line.split()[0], 16) for line in symbols.splitlines()
                       if len(line.split()) == 3 and line.split()[2] == symbol_name)
        expected.append(f"cr2=0x{address:016x}".encode())
    if mode in (8, 9):
        # M3a #4 step 2 fault probes fire after the real-page-table build; the
        # faulting address is the arena base in both cases (mode 8: not-present
        # read under the kernel root; mode 9: write to the RX user CODE leaf
        # under the process root, CR0.WP making it fault at CPL0).
        expected.insert(1, b"M3a-s2: process built; audit ok; root distinct")
        expected.append(f"cr2=0x{0x40000000:016x}".encode())
    output = boot(Path(f"build/{mode}/jeios.iso"), terminal=b"JeiOS fatal: exception; halted",
                  expected=expected)
    print(f"fault mode {mode}: PASS\n{output}")

# Mode 10 is NOT a fatal fault-injection: it proves D7 denial is FAULT-LOCAL.
# A user task executes SSE at CPL3 -> #UD from ring 3 -> only that process is
# terminated; the kernel survives and reaches the normal halt. So its terminal
# marker is the ordinary completion line, and a "JeiOS fatal:" would be a FAILURE.
result = subprocess.run(["make", "iso", "TEST_MODE=10"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
assert result.returncode == 0, result.stdout
output = boot(Path("build/10/jeios.iso"),
              terminal=b"JeiOS: initialization complete; halted",
              expected=[b"M3a-s3: hostile-FP user thread created; entering ring 3",
                        b"M3a-s3: hostile-FP task #UD terminated (fault-local); kernel alive"])
assert "JeiOS fatal:" not in output, f"mode 10 kernel died instead of terminating the task:\n{output}"
print(f"fault mode 10 (fault-local D7 #UD): PASS\n{output}")

# Mode 11 is the DEV-ONLY exception-fixup recovery proof (compiled with
# -DJE_DEV_COPY_INJECT, which the default kernel never gets). A user task
# raw-copies a deliberately unmapped page; the fixup must RECOVER the copy-site
# #PF and the kernel must survive into the NX supervisor success path.
result = subprocess.run(["make", "iso", "TEST_MODE=11"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
assert result.returncode == 0, result.stdout
output = boot(Path("build/11/jeios.iso"),
              terminal=b"M4f: zero-leak identity baseline verified",
              expected=[b"M3a-s5b: exception-fixup recovered load+store copy-site #PF (dev inject); kernel alive"])
assert "JeiOS fatal:" not in output, f"mode 11 kernel died instead of recovering the copy fault:\n{output}"
print(f"fault mode 11 (exception-fixup recovery): PASS\n{output}")

# Mode 12 is the frozen D12 supervisor-loss profile.  The supervisor deliberately
# faults after child readiness; slot 0 must refuse recovery, kill/reap the
# remaining user population, and prove the identity baseline before halting.
result = subprocess.run(["make", "iso", "TEST_MODE=12"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
assert result.returncode == 0, result.stdout
output = boot(Path("build/12/jeios.iso"),
              terminal=b"M4f: zero-leak identity baseline verified",
              expected=[b"M4f: supervisor phase=0x0000000000000001",
                        b"M4f: supervisor lost (last_phase=",
                        b"reason=0x0000000000000000",
                        b"M4f: zero-leak identity baseline verified"])
assert "JeiOS fatal:" not in output, f"mode 12 died during clean D12 teardown:\n{output}"
assert "M4f-s1: supervisor scenario completed" not in output, \
    "mode 12 unexpectedly completed the success scenario"
print(f"supervisor-loss mode 12 (D12 refusal): PASS\n{output}")

# Mode 13 is the PR-G D11 broker scenario. The supervisor owns all policy and
# the kernel monitor accepts success only from its image/process/cap state.
result = subprocess.run(["make", "iso", "TEST_MODE=13"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
assert result.returncode == 0, result.stdout
output = boot(Path("build/13/jeios.iso"),
              terminal=b"M4g: zero-leak identity baseline verified",
              expected=[b"M4g: supervisor phase=0x0000000000000001",
                        b"M4g: supervisor phase=0x0000000000000002",
                        b"M4g-s1: broker scenario completed",
                        b"M4g: zero-leak identity baseline verified",
                        b"M4g: client-reported EXHAUSTED; kernel delivery count A=2 corroborated"])
assert "JeiOS fatal:" not in output, f"mode 13 broker scenario failed:\n{output}"
assert "M4g: supervisor lost" not in output, \
    "mode 13 unexpectedly took the mode-12 supervisor-loss path"
print(f"broker mode 13 (D11): PASS\n{output}")

# Mode 14 is the D9 privilege-sensitive device scenario. It creates COM2 as a
# second QEMU 16550 so the ring-3 driver can generate a real IRQ3 through UART
# loopback. The scenario then observes the width/crossing and forbidden-port
# #GP deaths, admin revoke, self-close, stale authority, bounded IRQ delivery,
# and deny-by-default bitmap after teardown.
result = subprocess.run(["make", "iso", "TEST_MODE=14"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
assert result.returncode == 0, result.stdout
output = boot(Path("build/14/jeios.iso"),
              terminal=b"M4h: zero-leak identity baseline verified",
              expected=[b"M4h: IRQ3 deliveries=", b"M4h: TSS page U=0 in user CR3",
                        b"M4h: ring-3 #GP isolation",
                        b"M4h-s1: D9 device scenario completed"],
              timeout=30, serial2="null")
assert "JeiOS fatal:" not in output, f"mode 14 D9 scenario failed:\n{output}"
print(f"device mode 14 (D9): PASS\n{output}")

