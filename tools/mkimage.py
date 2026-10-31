#!/usr/bin/env python3
"""mkimage.py: ELF64 ring-3 executable -> JeiOS je_vm_image v1 C source (ADR 0010 D10).

The kernel never parses ELF (ADR 0007 D8). This host tool converts a statically linked
ring-3 ELF into the checked descriptor and blob that the kernel embeds. It rejects
anything je_vm_image_validate (kernel/core/vm.c) would reject, plus stricter
tool-only rules. The kernel still re-validates every image at boot as defense in depth.

THIS FILE IS THE FROZEN SPECIFICATION (PR-E pre-wiring, lead). The implementation
replaces only the body below the spec, never the spec.

CLI
  mkimage.py --name NAME --elf IN.elf --out OUT.c     convert; write OUT.c
  mkimage.py --name NAME --elf IN.elf --verify        check only; write nothing
Exit status: 0 = accepted; 2 = rejected (the first violated rule is printed to
stderr, prefixed "mkimage: reject: <RULE-ID>: "); 1 = usage or I/O error.
NAME matches [a-z][a-z0-9_]{0,31}.

Input rules. The first violated rule is the one reported.
  E1  file size <= 1 MiB; ELF magic; EI_CLASS = ELFCLASS64; EI_DATA = ELFDATA2LSB;
      EI_VERSION = 1; e_type = ET_EXEC (2); e_machine = EM_X86_64 (62); e_version = 1.
  E2  e_phentsize = 56; 1 <= e_phnum <= 16; e_phoff + e_phnum*56 <= file size
      (no overflow).
  E3  every program header type is PT_LOAD (1), PT_GNU_STACK (0x6474e551) or PT_NULL
      (0). Anything else is rejected, including PT_INTERP, PT_DYNAMIC, PT_TLS and
      PT_NOTE.
  E4  PT_GNU_STACK must not have PF_X.
  E5  1 <= number of PT_LOAD <= 8 (JE_VM_MAX_SEGMENTS).
  E6  each PT_LOAD: p_vaddr is 4 KiB-aligned; p_vaddr >= 0x40000000;
      p_offset % 4096 == p_vaddr % 4096; p_filesz <= p_memsz; p_memsz > 0;
      p_offset + p_filesz <= file size (no overflow).
  E7  each PT_LOAD's flags, mapped ELF -> JE (ELF PF_X=1, PF_W=2, PF_R=4; JE R=1, W=2,
      X=4), must be exactly R|X, R, or R|W. W together with X is rejected.
  E8  segments sorted by p_vaddr do not overlap at page granularity. Each segment's
      target_offset = p_vaddr - 0x40000000 and memory_size = round_up(p_memsz, 4096).
      [target_offset, target_offset + memory_size) must lie within
      [0, 0x1fb000): below the guard page (0x1fb000) and therefore also below the
      stack (0x1fc000..0x200000).
  E9  sum over segments of memory_size/4096, plus 4 stack pages, <= 64
      (JE_VM_MAX_USER_PAGES).
  E10 e_entry lies in an R|X segment's INITIALIZED bytes:
      p_vaddr <= e_entry < p_vaddr + p_filesz.

Output (accepted input). Deterministic: identical input bytes give identical output
bytes; no timestamps.
  - The blob is the concatenation of each PT_LOAD's file bytes, in ascending p_vaddr
    order. Each segment's source_offset is its running offset in the blob, and its
    source_size = p_filesz.
  - Descriptor: version 1; segment_count = number of PT_LOADs; entry_offset =
    e_entry - 0x40000000; every reserved field 0; unused segment slots all zero.
  - OUT.c defines exactly (NAME substituted):
        #include <jeios/vm.h>
        const unsigned char je_img_NAME_blob[] = { ... };
        const unsigned long je_img_NAME_len = <blob length>;
        const je_vm_image   je_img_NAME_desc = { ... };
    The leading comment records the input sha256, blob sha256, blob length, segment
    table and tool version, and no timestamp.
"""
import sys
import argparse
import hashlib
import re
import struct

FILE_LIMIT = 1024 * 1024
PAGE_SIZE = 4096
BASE = 0x40000000
GUARD = 0x1FB000
MAX_SEGMENTS = 8
MAX_USER_PAGES = 64
STACK_PAGES = 4
PHDR_SIZE = 56
PT_NULL = 0
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
PF_R = 4
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
NAME_RE = re.compile(r"[a-z][a-z0-9_]{0,31}\Z")


class Reject(Exception):
    def __init__(self, rule, detail):
        super().__init__(detail)
        self.rule = rule
        self.detail = detail


def reject(rule, detail):
    raise Reject(rule, detail)


def round_up(value):
    return (value + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE


def parse_elf(data):
    if len(data) > FILE_LIMIT:
        reject("E1", "file exceeds 1 MiB")
    if len(data) < ELF_HEADER.size:
        reject("E1", "truncated ELF header")
    fields = ELF_HEADER.unpack_from(data)
    ident, e_type, machine, version, _entry, phoff, _shoff, _flags, \
        _ehsize, phentsize, phnum, _shnum, _shentsize, _shstrndx = fields
    if ident[:4] != b"\x7fELF":
        reject("E1", "bad ELF magic")
    if ident[4] != 2:
        reject("E1", "ELFCLASS64 required")
    if ident[5] != 1:
        reject("E1", "ELFDATA2LSB required")
    if ident[6] != 1:
        reject("E1", "ELF identification version must be 1")
    if e_type != 2:
        reject("E1", "ET_EXEC required")
    if machine != 62:
        reject("E1", "EM_X86_64 required")
    if version != 1:
        reject("E1", "ELF version must be 1")
    if phentsize != PHDR_SIZE:
        reject("E2", "program-header entry size must be 56")
    if phnum < 1 or phnum > 16:
        reject("E2", "program-header count must be 1..16")
    ph_bytes = phnum * PHDR_SIZE
    if phoff > len(data) or ph_bytes > len(data) - phoff:
        reject("E2", "program-header table is outside the file")

    headers = [
        PROGRAM_HEADER.unpack_from(data, phoff + index * PHDR_SIZE)
        for index in range(phnum)
    ]
    for header in headers:
        p_type = header[0]
        if p_type not in (PT_NULL, PT_LOAD, PT_GNU_STACK):
            reject("E3", "unsupported program-header type")
    for header in headers:
        if header[0] == PT_GNU_STACK and header[1] & PF_X:
            reject("E4", "PT_GNU_STACK is executable")
    loads = [header for header in headers if header[0] == PT_LOAD]
    if len(loads) < 1 or len(loads) > MAX_SEGMENTS:
        reject("E5", "PT_LOAD count must be 1..8")

    for header in loads:
        _p_type, flags, offset, vaddr, _paddr, filesz, memsz, _align = header
        if vaddr % PAGE_SIZE != 0:
            reject("E6", "PT_LOAD virtual address is not page-aligned")
        if vaddr < BASE:
            reject("E6", "PT_LOAD virtual address is below the user base")
        if offset % PAGE_SIZE != vaddr % PAGE_SIZE:
            reject("E6", "PT_LOAD offset and virtual address are misaligned")
        if filesz > memsz:
            reject("E6", "PT_LOAD filesz exceeds memsz")
        if memsz == 0:
            reject("E6", "PT_LOAD memsz must be nonzero")
        if offset > len(data) or filesz > len(data) - offset:
            reject("E6", "PT_LOAD file range is outside the file")
        if flags & ~0x7:
            reject("E7", "PT_LOAD has unknown permission bits")
        je_perm = ((1 if flags & PF_R else 0)
                   | (2 if flags & PF_W else 0)
                   | (4 if flags & PF_X else 0))
        if je_perm not in (1 | 4, 1, 1 | 2):
            reject("E7", "PT_LOAD permissions are not RX, R, or RW")

    ordered = sorted(loads, key=lambda header: header[3])
    previous_end = 0
    total_pages = STACK_PAGES
    segments = []
    for header in ordered:
        _p_type, flags, offset, vaddr, _paddr, filesz, memsz, _align = header
        target_offset = vaddr - BASE
        memory_size = round_up(memsz)
        end = target_offset + memory_size
        if target_offset < previous_end:
            reject("E8", "PT_LOADs overlap at page granularity")
        if end > GUARD:
            reject("E8", "PT_LOAD reaches the guard page or stack")
        previous_end = end
        total_pages += memory_size // PAGE_SIZE
        if total_pages > MAX_USER_PAGES:
            reject("E9", "image exceeds the user-page budget")
        je_perm = ((1 if flags & PF_R else 0)
                   | (2 if flags & PF_W else 0)
                   | (4 if flags & PF_X else 0))
        segments.append({
            "offset": offset,
            "vaddr": vaddr,
            "filesz": filesz,
            "memory_size": memory_size,
            "perm": je_perm,
            "target_offset": target_offset,
        })

    entry = fields[4]
    entry_ok = any(
        segment["perm"] == (1 | 4)
        and segment["vaddr"] <= entry < segment["vaddr"] + segment["filesz"]
        for segment in segments
    )
    if not entry_ok:
        reject("E10", "entry is outside initialized RX bytes")
    return entry - BASE, segments


def make_output(name, data, entry_offset, segments):
    blob = bytearray()
    output_segments = []
    for segment in segments:
        source_offset = len(blob)
        source_size = segment["filesz"]
        blob.extend(data[segment["offset"]:segment["offset"] + source_size])
        output_segments.append((source_offset, source_size,
                                segment["target_offset"], segment["memory_size"],
                                segment["perm"]))
    input_hash = hashlib.sha256(data).hexdigest()
    blob_hash = hashlib.sha256(blob).hexdigest()
    table = ", ".join(
        "off=0x{:x}/src={}/dst=0x{:x}/mem=0x{:x}/perm=0x{:x}".format(
            source_offset, source_size, target_offset, memory_size, perm)
        for source_offset, source_size, target_offset, memory_size, perm
        in output_segments
    )
    lines = [
        "/* mkimage.py v1; input_sha256={}; blob_sha256={}; blob_len={}; "
        "segments: {} */".format(input_hash, blob_hash, len(blob), table),
        "#include <jeios/vm.h>",
        "",
        "const unsigned char je_img_{}_blob[] = {{".format(name),
    ]
    for start in range(0, len(blob), 16):
        values = ", ".join("0x{:02x}u".format(byte)
                           for byte in blob[start:start + 16])
        lines.append("    " + values + ",")
    if not blob:
        lines.append("    0u,")
    lines.extend([
        "};",
        "const unsigned long je_img_{}_len = {}ul;".format(name, len(blob)),
        "const je_vm_image je_img_{}_desc = {{".format(name),
        "    .version = JE_VM_IMAGE_VERSION,",
        "    .segment_count = {}u,".format(len(output_segments)),
        "    .entry_offset = UINT64_C(0x{:x}),".format(entry_offset),
        "    .reserved = 0u,",
        "    .segments = {",
    ])
    for source_offset, source_size, target_offset, memory_size, perm \
            in output_segments:
        lines.append(
            "        {{ UINT64_C(0x{:x}), UINT64_C({}), UINT64_C(0x{:x}), "
            "UINT64_C(0x{:x}), {}u, 0u }},".format(
                source_offset, source_size, target_offset, memory_size, perm))
    for _ in range(MAX_SEGMENTS - len(output_segments)):
        lines.append("        { 0u, 0u, 0u, 0u, 0u, 0u },")
    lines.extend([
        "    },",
        "};",
        "",
    ])
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(prog="mkimage.py")
    parser.add_argument("--name", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--out")
    parser.add_argument("--verify", action="store_true")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 1 if exc.code else 0
    if not NAME_RE.fullmatch(args.name):
        sys.stderr.write("mkimage: reject: usage: invalid NAME\n")
        return 1
    if args.verify and args.out is not None:
        sys.stderr.write("mkimage: reject: usage: --verify and --out are exclusive\n")
        return 1
    if not args.verify and args.out is None:
        sys.stderr.write("mkimage: reject: usage: --out is required\n")
        return 1
    try:
        with open(args.elf, "rb") as input_file:
            data = input_file.read()
        entry_offset, segments = parse_elf(data)
        if not args.verify:
            output = make_output(args.name, data, entry_offset, segments)
            with open(args.out, "w", encoding="ascii", newline="\n") as output_file:
                output_file.write(output)
    except Reject as exc:
        sys.stderr.write("mkimage: reject: {}: {}\n".format(
            exc.rule, exc.detail))
        return 2
    except (OSError, ValueError, struct.error) as exc:
        sys.stderr.write("mkimage: {}\n".format(exc))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
