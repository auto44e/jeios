#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate bootstrap metadata, ELF target, and absence of runtime dependencies.
This is a structural check, not evidence that the CPU can boot this image.
"""
import struct
import subprocess
import sys
from pathlib import Path
p = Path(sys.argv[1])
data = p.read_bytes()
assert data[:6] == b"\x7fELF\x02\x01", "expected little-endian ELF64"
assert struct.unpack_from("<H", data, 18)[0] == 62, "expected AMD64"
magic = struct.pack("<I", 0xe85250d6)
offsets = [i for i in range(0, min(len(data), 32768) - 15, 8) if data[i:i+4] == magic]
assert len(offsets) == 1, "one aligned Multiboot2 header in first 32KiB required"
off = offsets[0]
h = struct.unpack_from("<4I", data, off)
assert h[1] == 0 and h[2] == 24 and sum(h) % (1 << 32) == 0, "bad header"
assert data[off+16:off+24] == struct.pack("<HHI", 0, 0, 8), "bad end tag"
assert not subprocess.check_output(["nm", "-u", str(p)]).strip(), "undefined symbols"
# Segment flags are checked here; QEMU fault tests verify hardware enforcement.
phoff = struct.unpack_from("<Q", data, 32)[0]
phsize, phnum = struct.unpack_from("<HH", data, 54)
entry = struct.unpack_from("<Q", data, 24)[0]
executable_entry = False
for i in range(phnum):
    t, flags, _, vaddr, _, _, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize)
    assert t != 3, "unexpected dynamic interpreter"
    if t == 1:
        assert flags & 3 != 3, "RWX ELF segment"
        assert vaddr + memsz <= 0x40000000, "outside bootstrap mapping"
        executable_entry |= bool(flags & 1 and vaddr <= entry < vaddr + memsz)
assert executable_entry, "entry must be in executable load segment"
print("ELF checks: PASS (AMD64, Multiboot2, entry, segments, no undefined symbols)")

