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
# #PF and the kernel must reach the normal halt — a "JeiOS fatal:" would be a
# FAILURE. This is NOT a fatal fault-injection, so its terminal is the normal
# completion line.
result = subprocess.run(["make", "iso", "TEST_MODE=11"], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
assert result.returncode == 0, result.stdout
output = boot(Path("build/11/jeios.iso"),
              terminal=b"JeiOS: initialization complete; halted",
              expected=[b"M3a-s5b: exception-fixup recovered load+store copy-site #PF (dev inject); kernel alive"])
assert "JeiOS fatal:" not in output, f"mode 11 kernel died instead of recovering the copy fault:\n{output}"
print(f"fault mode 11 (exception-fixup recovery): PASS\n{output}")

