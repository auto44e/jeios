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


def main(argv):
    # Pre-wiring STUB (lead): fails closed until the PR-E implementation lands.
    sys.stderr.write("mkimage: reject: STUB: not implemented\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
