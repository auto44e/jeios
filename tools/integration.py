#!/usr/bin/env python3
"""Build and execute independent fault-injection kernels (never contaminate normal build)."""
import subprocess
from pathlib import Path
from emulator import boot

for mode, vector, error in [(1, 6, 0), (2, 13, 0x28), (3, 8, 0)]:
    result = subprocess.run(["make", "iso", f"TEST_MODE={mode}"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    assert result.returncode == 0, result.stdout
    output = boot(Path(f"build/{mode}/jeios.iso"), terminal=b"JeiOS fatal: exception; halted",
                  expected=[b"M1: traps ready", f"PANIC vector=0x{vector:016x} error=0x{error:016x}".encode()])
    print(f"fault mode {mode}: PASS\n{output}")
