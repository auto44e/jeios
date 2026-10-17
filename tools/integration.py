#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and execute independent fault-injection kernels (never contaminate normal build)."""
import subprocess
from pathlib import Path
from emulator import boot

for mode, vector, error in [(1, 6, 0), (2, 13, 0x38), (3, 8, 0), (4, 14, 3), (5, 14, 0), (6, 14, 17), (7, 8, 0),
                            (8, 14, 0), (9, 14, 3), (10, 6, 0)]:
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
    if mode == 10:
        # M3a #4 step 3 D7 proof: a user task executes SSE at CPL3; CR0.EM=1 makes
        # the CPU raise #UD (vector 6) from a user-origin frame. The cs=0x33 in the
        # PANIC proves the fault came from ring 3, not the kernel.
        expected.insert(1, b"M3a-s3: hostile-FP user thread created; entering ring 3")
        expected.append(f"cs=0x{0x33:016x}".encode())
    output = boot(Path(f"build/{mode}/jeios.iso"), terminal=b"JeiOS fatal: exception; halted",
                  expected=expected)
    print(f"fault mode {mode}: PASS\n{output}")

