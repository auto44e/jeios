#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Black-box self-test for tools/mkimage.py (ADR 0010 D10).

Written from the FROZEN spec in mkimage.py's docstring (rules E1-E10, CLI, exit
codes, output format), not from its implementation. Stdlib only.

It crafts ELF64 files with struct, runs mkimage.py as a subprocess and checks:
  - every reject rule E1-E10: exit 2 and "mkimage: reject: <RULE-ID>: " on stderr,
    in both --verify and --out mode, with no OUT.c left behind;
  - boundary inputs that must still be accepted;
  - for accepted inputs: the generated .c compiles (hosted strict warnings and
    kernel freestanding flags), defines exactly je_img_NAME_{blob,len,desc} as
    read-only data, has the expected descriptor (read back by a compiled host
    probe), the blob is the vaddr-ordered concatenation, the output is
    deterministic, and --verify writes nothing;
  - a cross-check: tests/test_image.c (real kernel je_vm_image_validate) is built
    against several generated images and must pass;
  - an ELF from the real host toolchain (gcc/ld with a W^X linker script).

Usage: python3 tools/test_mkimage.py [--keep]
Exit 0 iff every check passes. Prints one PASS/FAIL summary line last.
"""
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "mkimage.py"
INCLUDE = ROOT / "include"

BASE = 0x40000000
PAGE = 0x1000
GUARD = 0x1FB000
STACK = 0x1FC000
ARENA = 0x200000
MiB = 1 << 20
U64 = (1 << 64) - 1

PT_NULL, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS = 0, 1, 2, 3, 4, 5, 6, 7
PT_GNU_EH_FRAME, PT_GNU_STACK, PT_GNU_RELRO, PT_GNU_PROPERTY = 0x6474E550, 0x6474E551, 0x6474E552, 0x6474E553
PF_X, PF_W, PF_R = 1, 2, 4
JE_R, JE_W, JE_X = 1, 2, 4
ELF_TO_JE = {PF_R | PF_X: JE_R | JE_X, PF_R: JE_R, PF_R | PF_W: JE_R | JE_W}

WARN = ["-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow", "-Wstrict-prototypes"]
KERNEL_CFLAGS = ["-std=c11", "-O2", "-ffreestanding", "-fno-builtin", "-fno-pie", "-fno-pic",
                 "-fno-stack-protector", "-m64", "-march=x86-64", "-mno-red-zone",
                 "-mgeneral-regs-only"]


# --------------------------------------------------------------------------- ELF builder

def seg(p_type, flags, vaddr=0, filesz=0, memsz=None, offset=None, data=None, align=PAGE):
    """One program header. offset=None -> placed by build_elf (congruent to vaddr)."""
    return {"type": p_type, "flags": flags, "vaddr": vaddr, "filesz": filesz,
            "memsz": filesz if memsz is None else memsz, "offset": offset,
            "data": data, "align": align}


def load(vaddr, flags, filesz, memsz=None, **kw):
    return seg(PT_LOAD, flags, vaddr, filesz, memsz, **kw)


def gnu_stack(flags=PF_R | PF_W):
    return seg(PT_GNU_STACK, flags, align=16)


def pattern(n, salt):
    return bytes((i * 31 + salt * 7 + 1) & 0xFF for i in range(n))


def build_elf(phdrs, entry=BASE, *, ei_class=2, ei_data=1, ei_version=1, magic=b"\x7fELF",
              e_type=2, machine=62, version=1, phentsize=56, phnum=None, phoff=None,
              ph_at_end=False, pad_to=None):
    """Return (bytes, placed_phdrs). Segment bytes go at page-aligned file offsets from
    0x1000 upward (congruent mod 4 KiB to vaddr) unless an explicit offset is given.
    data=None -> a pattern of filesz bytes; data=b"" -> header only, no file bytes
    (used for past-EOF and overflow cases)."""
    placed = []
    chunks = []
    next_off = PAGE
    for i, p in enumerate(phdrs):
        p = dict(p)
        if p["type"] == PT_LOAD and p["data"] is None and p["filesz"] <= MiB:
            p["data"] = pattern(p["filesz"], i)
        if p["offset"] is None:
            if p["type"] == PT_LOAD:
                start = (next_off + PAGE - 1) & ~(PAGE - 1)
                p["offset"] = start + (p["vaddr"] % PAGE)
                next_off = p["offset"] + max(p["filesz"], 1)
            else:
                p["offset"] = 0
        if p["data"] is not None and p["offset"] + len(p["data"]) <= 4 * MiB:
            chunks.append((p["offset"], p["data"]))
        placed.append(p)

    n = len(placed)
    size = 64 + n * 56
    for off, d in chunks:
        size = max(size, off + len(d))
    if ph_at_end:
        size = (size + 7) & ~7
        ph_off = size
        size += n * 56
    else:
        ph_off = 64
    img = bytearray(size)
    for off, d in chunks:
        img[off:off + len(d)] = d
    for i, p in enumerate(placed):
        struct.pack_into("<IIQQQQQQ", img, ph_off + i * 56, p["type"], p["flags"], p["offset"],
                         p["vaddr"], p["vaddr"], p["filesz"], p["memsz"], p["align"])
    ident = magic + bytes([ei_class, ei_data, ei_version]) + bytes(16 - 4 - 3)
    hdr = struct.pack("<16sHHIQQQIHHHHHH", ident, e_type, machine, version, entry,
                      ph_off if phoff is None else phoff, 0, 0, 64, phentsize,
                      n if phnum is None else phnum, 64, 0, 0)
    img[0:64] = hdr
    if pad_to is not None:
        assert pad_to >= len(img)
        img += bytes(pad_to - len(img))
    return bytes(img), placed


def default_phdrs():
    """W^X three-segment image like the real toolchain emits: RX text, R rodata, RW
    data whose tail is BSS, plus a non-executable GNU_STACK."""
    return [load(BASE, PF_R | PF_X, 0x20, 0x20),
            load(BASE + 0x1000, PF_R, 0x10),
            load(BASE + 0x2000, PF_R | PF_W, 0x8, 0x2000),
            gnu_stack()]


def expected_desc(elf, placed, entry):
    """Descriptor + blob the SPEC prescribes for an accepted input."""
    loads = sorted((p for p in placed if p["type"] == PT_LOAD), key=lambda p: p["vaddr"])
    blob = b""
    segs = []
    for p in loads:
        segs.append((len(blob), p["filesz"], p["vaddr"] - BASE,
                     (p["memsz"] + PAGE - 1) & ~(PAGE - 1), ELF_TO_JE[p["flags"]], 0))
        blob += elf[p["offset"]:p["offset"] + p["filesz"]]
    segs += [(0, 0, 0, 0, 0, 0)] * (8 - len(segs))
    return {"version": 1, "count": len(loads), "entry": entry - BASE, "reserved": 0,
            "segs": segs, "blob": blob}


# --------------------------------------------------------------------------- harness

class Harness:
    def __init__(self, tmp):
        self.tmp = Path(tmp)
        self.passed = 0
        self.failed = []
        self.skipped = []
        self.n = 0

    def ok(self, name, cond, detail=""):
        if cond:
            self.passed += 1
        else:
            self.failed.append(f"{name}: {detail}" if detail else name)
            print(f"FAIL {name}" + (f": {detail}" if detail else ""), file=sys.stderr)
        return cond

    def fresh(self, tag):
        self.n += 1
        d = self.tmp / f"{self.n:03d}_{re.sub(r'[^A-Za-z0-9_]', '_', tag)[:40]}"
        d.mkdir()
        return d

    def write_elf(self, tag, elf):
        d = self.fresh(tag)
        p = d / "in.elf"
        p.write_bytes(elf)
        return d, p


def run_tool(args, cwd=None):
    r = subprocess.run([sys.executable, str(TOOL)] + [str(a) for a in args], cwd=cwd,
                       capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout, r.stderr


def run(cmd, cwd=None):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout, r.stderr


def rule_of(stderr):
    m = re.search(r"^mkimage: reject: ([A-Z0-9]+): ", stderr, re.M)
    return m.group(1) if m else None


def expect_reject(h, tag, elf, rule):
    """Exit 2 + exact rule ID in --verify and --out mode; no OUT.c left behind."""
    d, p = h.write_elf(tag, elf)
    rc, _, err = run_tool(["--name", "t", "--elf", p, "--verify"], cwd=d)
    h.ok(f"{tag} [verify] -> {rule}", rc == 2 and rule_of(err) == rule,
         f"rc={rc} rule={rule_of(err)} stderr={err.strip()[:160]!r}")
    out = d / "out.c"
    rc, _, err = run_tool(["--name", "t", "--elf", p, "--out", out], cwd=d)
    h.ok(f"{tag} [out] -> {rule}", rc == 2 and rule_of(err) == rule,
         f"rc={rc} rule={rule_of(err)} stderr={err.strip()[:160]!r}")
    h.ok(f"{tag} [out] writes no OUT.c on reject", not out.exists())


PROBE_C = r"""
#include <stdio.h>
#include <jeios/vm.h>
#include JE_GEN_C
#define CAT_(a, b, c) a##b##c
#define CAT(a, b, c) CAT_(a, b, c)
int main(void)
{
    const je_vm_image *d = &CAT(je_img_, JE_GEN_NAME, _desc);
    const unsigned char *b = CAT(je_img_, JE_GEN_NAME, _blob);
    size_t n = sizeof CAT(je_img_, JE_GEN_NAME, _blob);
    printf("len %lu\n", CAT(je_img_, JE_GEN_NAME, _len));
    printf("sizeof %zu\n", n);
    printf("hdr %u %u %llu %llu\n", d->version, d->segment_count,
           (unsigned long long)d->entry_offset, (unsigned long long)d->reserved);
    for (unsigned i = 0; i < JE_VM_MAX_SEGMENTS; ++i) {
        const je_vm_segment *s = &d->segments[i];
        printf("seg %llu %llu %llu %llu %u %u\n", (unsigned long long)s->source_offset,
               (unsigned long long)s->source_size, (unsigned long long)s->target_offset,
               (unsigned long long)s->memory_size, s->perm, s->reserved);
    }
    printf("blob ");
    for (size_t i = 0; i < n; ++i) printf("%02x", b[i]);
    printf("\n");
    return 0;
}
"""


def probe(h, tag, d, out, name):
    """Compile + run a host probe that #includes the generated source; return dict."""
    pc = d / "probe.c"
    pc.write_text(PROBE_C)
    exe = d / "probe"
    rc, _, err = run(["gcc", "-std=c11", *WARN, f"-I{INCLUDE}", f'-DJE_GEN_C="{out}"',
                      f"-DJE_GEN_NAME={name}", pc, "-o", exe])
    if not h.ok(f"{tag}: probe compiles", rc == 0, err.strip()[:400]):
        return None
    rc, so, err = run([exe])
    if not h.ok(f"{tag}: probe runs", rc == 0, err.strip()[:200]):
        return None
    res = {"segs": []}
    for line in so.splitlines():
        k, _, v = line.partition(" ")
        if k == "len":
            res["len"] = int(v)
        elif k == "sizeof":
            res["sizeof"] = int(v)
        elif k == "hdr":
            a = [int(x) for x in v.split()]
            res.update(version=a[0], count=a[1], entry=a[2], reserved=a[3])
        elif k == "seg":
            res["segs"].append(tuple(int(x) for x in v.split()))
        elif k == "blob":
            res["blob"] = bytes.fromhex(v.strip())
    return res


def expect_accept(h, tag, elf, placed, entry, name="t", deep=True, cross=False):
    """Accepted: exit 0 in both modes; with deep=True also compile + descriptor + blob +
    determinism + symbol checks; cross=True also runs tests/test_image.c against it."""
    d, p = h.write_elf(tag, elf)
    rc, _, err = run_tool(["--name", name, "--elf", p, "--verify"], cwd=d)
    h.ok(f"{tag}: --verify accepts", rc == 0, f"rc={rc} stderr={err.strip()[:160]!r}")
    h.ok(f"{tag}: --verify writes nothing", sorted(x.name for x in d.iterdir()) == ["in.elf"])
    out = d / f"image_{name}.c"
    rc, _, err = run_tool(["--name", name, "--elf", p, "--out", out], cwd=d)
    if not h.ok(f"{tag}: --out accepts", rc == 0 and out.exists(),
                f"rc={rc} stderr={err.strip()[:160]!r}"):
        return None
    if not deep:
        return out
    src = out.read_bytes()

    # Compiles: hosted strict, and with the kernel's freestanding flags.
    rc, _, err = run(["gcc", "-std=c11", *WARN, f"-I{INCLUDE}", "-c", out, "-o", d / "hosted.o"])
    h.ok(f"{tag}: generated .c compiles (hosted strict)", rc == 0, err.strip()[:400])
    ko = d / "kernel.o"
    rc, _, err = run(["gcc", *KERNEL_CFLAGS, *WARN, f"-I{INCLUDE}", "-c", out, "-o", ko])
    if h.ok(f"{tag}: generated .c compiles (kernel flags)", rc == 0, err.strip()[:400]):
        rc, so, _ = run(["nm", "--defined-only", "-g", ko])
        syms = {}
        for line in so.splitlines():
            parts = line.split()
            if len(parts) == 3:
                syms[parts[2]] = parts[1]
        want = {f"je_img_{name}_{s}" for s in ("blob", "len", "desc")}
        h.ok(f"{tag}: defines exactly je_img_{name}_{{blob,len,desc}}", set(syms) == want,
             f"got {sorted(syms)}")
        h.ok(f"{tag}: all three symbols are read-only data",
             all(syms.get(s) == "R" for s in want), f"types {syms}")

    # Descriptor + blob, read back from the compiled generated object.
    got = probe(h, tag, d, out, name)
    exp = expected_desc(elf, placed, entry)
    if got is not None:
        h.ok(f"{tag}: len == blob length", got.get("len") == len(exp["blob"]) == got.get("sizeof"),
             f"len={got.get('len')} sizeof={got.get('sizeof')} expected={len(exp['blob'])}")
        h.ok(f"{tag}: header (version, count, entry, reserved)",
             (got.get("version"), got.get("count"), got.get("entry"), got.get("reserved")) ==
             (1, exp["count"], exp["entry"], 0),
             f"got {(got.get('version'), got.get('count'), got.get('entry'), got.get('reserved'))} "
             f"want {(1, exp['count'], exp['entry'], 0)}")
        h.ok(f"{tag}: segment table (incl. ELF->JE perm map, zero unused slots)",
             got["segs"] == exp["segs"], f"got {got['segs']} want {exp['segs']}")
        h.ok(f"{tag}: blob is the vaddr-ordered concatenation", got.get("blob") == exp["blob"])

    # Header comment records the hashes and length (spec "leading comment").
    text = src.decode("utf-8", "replace").lower()
    h.ok(f"{tag}: comment records input sha256", hashlib.sha256(elf).hexdigest() in text)
    h.ok(f"{tag}: comment records blob sha256", hashlib.sha256(exp["blob"]).hexdigest() in text)

    # Determinism: a second run (different directory, same file names) is byte-identical.
    d2 = h.fresh(tag + "_again")
    (d2 / "in.elf").write_bytes(elf)
    out2 = d2 / out.name
    rc, _, err = run_tool(["--name", name, "--elf", d2 / "in.elf", "--out", out2], cwd=d2)
    h.ok(f"{tag}: deterministic (second run byte-identical)",
         rc == 0 and out2.exists() and out2.read_bytes() == src, f"rc={rc}")

    if cross:
        cross_check(h, tag, d, out, name)
    return out


def cross_check(h, tag, d, out, name):
    """tests/test_image.c: the REAL kernel validator must accept the generated image."""
    exe = d / "test_image"
    rc, _, err = run(["gcc", f"-I{INCLUDE}", "-DJE_TEST_MODE=0", f'-DJE_TEST_IMAGE_C="{out}"',
                      f"-DJE_TEST_IMAGE_NAME={name}", "-std=c11", "-O1", "-g", *WARN,
                      "-fsanitize=undefined", "-fno-sanitize-recover=all",
                      ROOT / "tests" / "test_image.c", ROOT / "kernel" / "core" / "vm.c",
                      ROOT / "kernel" / "core" / "handle.c", "-o", exe])
    if h.ok(f"{tag}: tests/test_image.c builds against it", rc == 0, err.strip()[:400]):
        rc, so, err = run([exe])
        h.ok(f"{tag}: kernel je_vm_image_validate accepts + mutations rejected", rc == 0,
             (so + err).strip()[-400:])


# --------------------------------------------------------------------------- cases

def reject_cases(h):
    E = lambda tag, rule, phdrs=None, entry=BASE, **kw: expect_reject(
        h, tag, build_elf(default_phdrs() if phdrs is None else phdrs, entry, **kw)[0], rule)
    good, _ = build_elf(default_phdrs())

    # E1: size, magic, ident, type, machine, version.
    E("E1 file 1 MiB + 1", "E1", pad_to=MiB + 1)
    expect_reject(h, "E1 empty file", b"", "E1")
    E("E1 bad magic", "E1", magic=b"\x7fELG")
    expect_reject(h, "E1 MZ file", b"MZ" + good[2:], "E1")
    E("E1 ELFCLASS32", "E1", ei_class=1)
    E("E1 ELFCLASSNONE", "E1", ei_class=0)
    E("E1 big-endian", "E1", ei_data=2)
    E("E1 EI_VERSION 0", "E1", ei_version=0)
    E("E1 ET_DYN", "E1", e_type=3)
    E("E1 ET_REL", "E1", e_type=1)
    E("E1 EM_386", "E1", machine=3)
    E("E1 EM_AARCH64", "E1", machine=183)
    E("E1 e_version 0", "E1", version=0)
    E("E1 e_version 2", "E1", version=2)
    E("E1 before E3 (bad machine + PT_INTERP)", "E1",
      default_phdrs() + [seg(PT_INTERP, PF_R)], machine=3)

    # E2: phentsize, phnum bounds, phoff range/overflow.
    E("E2 phentsize 64", "E2", phentsize=64)
    E("E2 phentsize 0", "E2", phentsize=0)
    E("E2 phnum 0", "E2", phnum=0)
    E("E2 phnum 17", "E2", default_phdrs() + [seg(PT_NULL, 0)] * 13)
    E("E2 phoff past EOF", "E2", phoff=len(good))
    at_end, _ = build_elf(default_phdrs(), ph_at_end=True)   # file ends at phoff + 4*56
    E("E2 phoff + phnum*56 = size + 1", "E2", ph_at_end=True, phoff=len(at_end) - 4 * 56 + 1)
    E("E2 phoff 64-bit overflow", "E2", phoff=U64 - 55)
    E("E2 phoff 2^63", "E2", phoff=1 << 63)

    # E3: only PT_LOAD / PT_GNU_STACK / PT_NULL.
    for t, n in ((PT_INTERP, "PT_INTERP"), (PT_DYNAMIC, "PT_DYNAMIC"), (PT_TLS, "PT_TLS"),
                 (PT_NOTE, "PT_NOTE"), (PT_PHDR, "PT_PHDR"), (PT_SHLIB, "PT_SHLIB"),
                 (PT_GNU_RELRO, "PT_GNU_RELRO"), (PT_GNU_PROPERTY, "PT_GNU_PROPERTY"),
                 (PT_GNU_EH_FRAME, "PT_GNU_EH_FRAME"), (0x70000001, "PT_LOPROC+1")):
        E(f"E3 {n}", "E3", default_phdrs() + [seg(t, PF_R)])
    E("E3 PT_INTERP first", "E3", [seg(PT_INTERP, PF_R)] + default_phdrs())
    # Literal reading of "the first violated rule": rule order, not header order.
    E("E3 before E7 (W|X LOAD at index 0, PT_NOTE later)", "E3",
      [load(BASE, PF_R | PF_W | PF_X, 0x20)] + default_phdrs()[1:] + [seg(PT_NOTE, PF_R)])

    # E4: GNU_STACK must not be executable.
    E("E4 GNU_STACK RWX", "E4", default_phdrs()[:3] + [gnu_stack(PF_R | PF_W | PF_X)])
    E("E4 GNU_STACK X only", "E4", default_phdrs()[:3] + [gnu_stack(PF_X)])

    # E5: 1..8 PT_LOAD.
    E("E5 zero PT_LOAD", "E5", [gnu_stack()])
    E("E5 zero PT_LOAD (only PT_NULL)", "E5", [seg(PT_NULL, 0)])
    E("E5 nine PT_LOAD", "E5",
      [load(BASE, PF_R | PF_X, 0x10)] + [load(BASE + PAGE * i, PF_R, 0x10) for i in range(1, 9)])

    # E6: per-PT_LOAD alignment/base/congruence/sizes/file range.
    E("E6 unaligned vaddr (congruent offset)", "E6",
      [load(BASE + 0x10, PF_R | PF_X, 0x20)], entry=BASE + 0x10)
    E("E6 vaddr below base", "E6",
      [load(BASE - PAGE, PF_R, 0x10), load(BASE, PF_R | PF_X, 0x20)])
    E("E6 vaddr 0", "E6", [load(0, PF_R, 0x10), load(BASE, PF_R | PF_X, 0x20)])
    E("E6 offset not congruent to vaddr", "E6",
      [load(BASE, PF_R | PF_X, 0x20, offset=0x1008, data=pattern(0x20, 1))])
    E("E6 filesz > memsz", "E6",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W, 0x10, 0x8)])
    E("E6 memsz 0", "E6", [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R, 0, 0)])
    E("E6 offset + filesz = file size + 1", "E6",
      [load(BASE, PF_R | PF_X, 0x20),
       load(BASE + PAGE, PF_R, 0x11, offset=0x2000, data=pattern(0x10, 2))])
    E("E6 offset past EOF", "E6",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R, 0x10, offset=0x40000, data=b"")])
    E("E6 offset + filesz 64-bit overflow", "E6",
      [load(BASE, PF_R | PF_X, 0x20),
       load(BASE + PAGE, PF_R, 0x2000, offset=U64 - PAGE + 1, data=b"")])

    # E7: flags must map to exactly R|X, R or R|W. Every other combination of the
    # three ELF bits, and unknown bits, are rejected.
    for fl, n in ((0, "none"), (PF_X, "X"), (PF_W, "W"), (PF_W | PF_X, "W|X"),
                  (PF_R | PF_W | PF_X, "R|W|X"), (PF_R | 0x00100000, "R|PF_MASKOS bit"),
                  (PF_R | 0x80000000, "R|PF_MASKPROC bit"), (PF_R | 0x8, "R|0x8")):
        E(f"E7 rodata flags {n}", "E7",
          [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, fl, 0x10)])
    E("E7 code segment R|W|X (entry inside it)", "E7", [load(BASE, PF_R | PF_W | PF_X, 0x20)])
    E("E7 W|X data", "E7",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_W | PF_X, 0x10)])

    # E8: page-granular overlap; [target, target+round_up(memsz)) within [0, 0x1fb000).
    E("E8 overlap: text memsz 0x1001 runs into next page", "E8",
      [load(BASE, PF_R | PF_X, 0x20, 0x1001), load(BASE + PAGE, PF_R, 0x10)])
    E("E8 overlap: duplicate vaddr", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE, PF_R, 0x10)])
    E("E8 overlap: out-of-order headers, BSS covers later page", "E8",
      [load(BASE + 0x3000, PF_R, 0x10), load(BASE, PF_R | PF_X, 0x20),
       load(BASE + PAGE, PF_R | PF_W, 0x8, 0x2001)])
    E("E8 guard intrusion: memsz rounds into 0x1fb000", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + GUARD - PAGE, PF_R | PF_W, 0x8, 0x1001)])
    E("E8 segment at the guard page", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + GUARD, PF_R, 0x10)])
    E("E8 segment on the stack", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + STACK, PF_R | PF_W, 0x10)])
    E("E8 segment at arena top", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + ARENA, PF_R, 0x10)])
    E("E8 vaddr near 2^64", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(U64 - PAGE + 1, PF_R, 0x10)])
    E("E8 memsz near 2^64 (round_up overflow)", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W, 0x8, U64)])

    # E9: image pages + 4 stack pages <= 64.
    E("E9 page budget 65 (1 + 60 + 4)", "E9",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W, 0x8, 60 * PAGE)])
    E("E9 page budget 65 via page rounding (1 + 59 pages + 1 byte)", "E9",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W, 0x8, 59 * PAGE + 1)])
    E("E9 single huge BSS ending at the guard", "E9",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W, 0x8, GUARD - PAGE)])

    # E10: entry in an R|X segment's initialized bytes.
    E("E10 entry in RX BSS", "E10", [load(BASE, PF_R | PF_X, 0x20, 0x1000)], entry=BASE + 0x20)
    E("E10 entry in rodata (non-X)", "E10", entry=BASE + 0x1000)
    E("E10 entry in data initialized bytes (non-X)", "E10", entry=BASE + 0x2000)
    E("E10 entry in data BSS (non-X)", "E10", entry=BASE + 0x2100)
    E("E10 entry in an unmapped gap", "E10",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + 0x5000, PF_R, 0x10)], entry=BASE + 0x3000)
    E("E10 entry 0", "E10", entry=0)
    E("E10 entry below base", "E10", entry=BASE - 1)
    E("E10 entry near 2^64", "E10", entry=U64)

    # Rule-ordering precedence: when two rules are violated at once, the LOWEST
    # rule number must be reported. These pair cases guard against an
    # implementation that checks segments (or rules) in the wrong order and so
    # reports whichever violation it hits first. Each case is constructed to
    # break exactly the two named rules and assert the lower one. (A naive
    # per-segment loop that merges E6+E7 or E8+E9 would report the higher rule
    # for the second segment -- this is the regression these cases catch.)
    E("precedence E2<E3 (phentsize bad + PT_INTERP)", "E2",
      default_phdrs() + [seg(PT_INTERP, PF_R)], phentsize=64)
    E("precedence E3<E4 (PT_NOTE + GNU_STACK X)", "E3",
      default_phdrs()[:3] + [seg(PT_NOTE, PF_R), gnu_stack(PF_X)])
    E("precedence E3<E5 (nine PT_LOAD + PT_INTERP)", "E3",
      [load(BASE, PF_R | PF_X, 0x10)] + [load(BASE + PAGE * i, PF_R, 0x10) for i in range(1, 9)]
      + [seg(PT_INTERP, PF_R)])
    E("precedence E5<E6 (nine PT_LOAD, one below base)", "E5",
      [load(BASE - PAGE, PF_R, 0x10)] + [load(BASE + PAGE * i, PF_R | PF_X, 0x10) for i in range(9)]),
    # E6 on a LATER segment vs E7 on an EARLIER segment: a per-segment loop that
    # checks E7 before E6 within each segment would report E7 (seg 0) first.
    # Lowest-rule-wins requires E6 (seg 1) to win.
    E("precedence E6<E7 (seg0 W|X, seg1 unaligned vaddr)", "E6",
      [load(BASE, PF_R | PF_W | PF_X, 0x20), load(BASE + PAGE + 0x10, PF_R, 0x10)],
      entry=BASE),
    # E7 on a later segment vs a clean earlier one, with an E6 violation only on
    # the earlier one: here the lower rule E6 is on seg 0, so E6 wins regardless.
    E("precedence E6<E7 (seg0 unaligned, seg1 W|X)", "E6",
      [load(BASE + 0x10, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W | PF_X, 0x10)],
      entry=BASE + 0x10),
    # E8 on an earlier pair vs E9 on the total: overlap must be reported before
    # the page-budget total, which a merged loop computes as it walks.
    E("precedence E8<E9 (overlap + 65-page total)", "E8",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE, PF_R | PF_W, 0x8, 60 * PAGE)]),
    # E9 vs E10: an over-budget image whose entry is also invalid must report E9.
    E("precedence E9<E10 (65 pages + bad entry)", "E9",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W, 0x8, 60 * PAGE)],
      entry=BASE + 0x2100),


def accept_cases(h):
    def A(tag, phdrs, entry=BASE, name="t", deep=True, cross=False, **kw):
        elf, placed = build_elf(phdrs, entry, **kw)
        return expect_accept(h, tag, elf, placed, entry, name=name, deep=deep, cross=cross)

    A("accept default W^X image", default_phdrs(), name="c_hello", cross=True)
    # Explicit ELF -> JE mapping (R|X is bit-palindromic, R and R|W are not).
    elf, placed = build_elf(default_phdrs())
    d, p = h.write_elf("perm map", elf)
    out = d / "o.c"
    rc, _, err = run_tool(["--name", "pm", "--elf", p, "--out", out], cwd=d)
    if h.ok("perm map: accepted", rc == 0, err.strip()[:160]):
        got = probe(h, "perm map", d, out, "pm")
        if got is not None:
            h.ok("perm map: ELF R|X(5) -> JE R|X(5)", got["segs"][0][4] == JE_R | JE_X)
            h.ok("perm map: ELF R(4) -> JE R(1)", got["segs"][1][4] == JE_R, str(got["segs"][1]))
            h.ok("perm map: ELF R|W(6) -> JE R|W(3)", got["segs"][2][4] == JE_R | JE_W,
                 str(got["segs"][2]))
            h.ok("perm map: data memory_size page-rounded BSS (0x2000), source_size 8",
                 got["segs"][2][1] == 8 and got["segs"][2][3] == 0x2000, str(got["segs"][2]))

    A("accept out-of-order headers (blob in vaddr order)",
      [load(BASE + 0x2000, PF_R | PF_W, 0x18, 0x1800), gnu_stack(),
       load(BASE + PAGE, PF_R, 0x30), seg(PT_NULL, 0), load(BASE, PF_R | PF_X, 0x40)],
      entry=BASE + 0x3F, cross=True)
    A("accept single RX segment, no GNU_STACK", [load(BASE, PF_R | PF_X, 0x10)])
    A("accept GNU_STACK R only", [load(BASE, PF_R | PF_X, 0x10), gnu_stack(PF_R)], deep=False)
    A("accept GNU_STACK flags 0", [load(BASE, PF_R | PF_X, 0x10), gnu_stack(0)], deep=False)
    A("accept GNU_STACK R|W first", [gnu_stack(), load(BASE, PF_R | PF_X, 0x10)], deep=False)
    A("accept unaligned memsz rounds up (0xfff -> 0x1000)",
      [load(BASE, PF_R | PF_X, 0x10, 0xFFF), load(BASE + PAGE, PF_R | PF_W, 0, 1)])
    A("accept pure-BSS RW segment (filesz 0)",
      [load(BASE, PF_R | PF_X, 0x10), load(BASE + PAGE, PF_R | PF_W, 0, 0x3000)], cross=True)
    A("accept phnum 16 (PT_NULL padding)", default_phdrs() + [seg(PT_NULL, 0)] * 12, deep=False)
    A("accept program headers end exactly at EOF", default_phdrs(), ph_at_end=True)
    A("accept file exactly 1 MiB", default_phdrs(), pad_to=MiB, deep=False)
    A("accept last segment ends exactly at EOF", default_phdrs())
    A("accept eight PT_LOAD",
      [load(BASE, PF_R | PF_X, 0x10)] + [load(BASE + PAGE * i, PF_R, 0x10 + i) for i in range(1, 8)],
      cross=True)
    A("accept page budget exactly 64 (1 + 59 + 4)",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_W, 0x8, 59 * PAGE)], cross=True)
    A("accept segment ending exactly at the guard page",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + GUARD - PAGE, PF_R | PF_W, 0x8, PAGE)],
      cross=True)
    A("accept segments with gaps", [load(BASE, PF_R | PF_X, 0x20),
                                    load(BASE + 0x10000, PF_R | PF_W, 0x8)])
    A("accept entry at last initialized RX byte", [load(BASE, PF_R | PF_X, 0x20, 0x1000)],
      entry=BASE + 0x1F, cross=True)
    A("accept entry in second RX segment",
      [load(BASE, PF_R | PF_X, 0x20), load(BASE + PAGE, PF_R | PF_X, 0x20)], entry=BASE + 0x1010)

    # NAME grammar boundaries.
    A("accept name of 32 chars", [load(BASE, PF_R | PF_X, 0x10)], name="a" + "b_9" * 10 + "z")
    A("accept name 'a'", [load(BASE, PF_R | PF_X, 0x10)], name="a", deep=False)


def cli_cases(h):
    elf, _ = build_elf(default_phdrs())
    d, p = h.write_elf("cli", elf)
    for name in ("", "Hello", "1abc", "_a", "a-b", "a.b", "a b", "a" * 33, "é"):
        rc, _, err = run_tool(["--name", name, "--elf", p, "--verify"], cwd=d)
        h.ok(f"CLI bad NAME {name!r} -> exit 1", rc == 1, f"rc={rc} {err.strip()[:120]!r}")
    rc, _, _ = run_tool(["--name", "Bad", "--elf", p, "--out", d / "x.c"], cwd=d)
    h.ok("CLI bad NAME with --out -> exit 1, nothing written", rc == 1 and not (d / "x.c").exists(),
         f"rc={rc}")
    rc, _, _ = run_tool(["--name", "t", "--elf", d / "missing.elf", "--verify"], cwd=d)
    h.ok("CLI missing input file -> exit 1", rc == 1, f"rc={rc}")
    rc, _, _ = run_tool(["--name", "t", "--elf", d, "--verify"], cwd=d)
    h.ok("CLI input is a directory -> exit 1", rc == 1, f"rc={rc}")
    rc, _, _ = run_tool(["--name", "t", "--elf", p], cwd=d)
    h.ok("CLI neither --out nor --verify -> exit 1", rc == 1, f"rc={rc}")
    rc, _, _ = run_tool(["--elf", p, "--verify"], cwd=d)
    h.ok("CLI missing --name -> exit 1", rc == 1, f"rc={rc}")
    rc, _, _ = run_tool([], cwd=d)
    h.ok("CLI no arguments -> exit 1", rc == 1, f"rc={rc}")
    rc, _, _ = run_tool(["--name", "t", "--elf", p, "--verify", "--bogus"], cwd=d)
    h.ok("CLI unknown option -> exit 1", rc == 1, f"rc={rc}")
    rc, _, _ = run_tool(["--name", "t", "--elf", p, "--out", d / "no_such_dir" / "o.c"], cwd=d)
    h.ok("CLI unwritable --out path -> exit 1", rc == 1, f"rc={rc}")
    h.ok("CLI cases left the input directory unchanged",
         sorted(x.name for x in d.iterdir()) == ["in.elf"], str(sorted(x.name for x in d.iterdir())))


TOOLCHAIN_LD = """ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x40000000;
  .text : { *(.text.start) *(.text .text.*) } :text
  . = ALIGN(4096);
  .rodata : { *(.rodata .rodata.*) } :rodata
  . = ALIGN(4096);
  .data : { *(.data .data.*) } :data
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.gnu.property) }
}
"""
TOOLCHAIN_C = """const char ro[64] = "rodata-canary";
volatile unsigned long dv = 0x1234;
unsigned char bss_buf[8192];
__attribute__((section(".text.start"))) void _start(void)
{
    bss_buf[0] = (unsigned char)(ro[0] + dv);
    for (;;) { }
}
"""


def toolchain_case(h):
    """A real gcc/ld ring-3-style ELF (W^X linker script; test-local, not user/user.ld)."""
    if shutil.which("gcc") is None:
        h.skipped.append("toolchain ELF: gcc not found")
        return
    d = h.fresh("toolchain")
    (d / "u.ld").write_text(TOOLCHAIN_LD)
    (d / "u.c").write_text(TOOLCHAIN_C)
    rc, _, err = run(["gcc", "-std=c11", "-O2", "-ffreestanding", "-nostdlib", "-static", "-no-pie",
                      "-fno-pic", "-fno-pie", "-fcf-protection=none",
                      "-fno-asynchronous-unwind-tables", "-mgeneral-regs-only", "-mno-red-zone",
                      "-march=x86-64", "-Wl,-T,u.ld", "-Wl,--build-id=none",
                      "-Wl,-z,max-page-size=4096", "-Wl,-z,noexecstack", "u.c", "-o", "u.elf"],
                     cwd=d)
    if not h.ok("toolchain ELF links", rc == 0, err.strip()[:400]):
        return
    elf = (d / "u.elf").read_bytes()
    # Re-parse its program headers to build the expected descriptor.
    phoff = struct.unpack_from("<Q", elf, 32)[0]
    phnum = struct.unpack_from("<H", elf, 56)[0]
    entry = struct.unpack_from("<Q", elf, 24)[0]
    placed = []
    for i in range(phnum):
        t, fl, off, va, _, fs, ms, al = struct.unpack_from("<IIQQQQQQ", elf, phoff + i * 56)
        placed.append({"type": t, "flags": fl, "offset": off, "vaddr": va, "filesz": fs,
                       "memsz": ms, "align": al})
    expect_accept(h, "toolchain ELF", elf, placed, entry, name="tc", cross=True)
    h.ok("toolchain ELF: three W^X PT_LOADs (R|X, R, R|W)",
         [p["flags"] for p in placed if p["type"] == PT_LOAD] == [5, 4, 6],
         str([(hex(p["type"]), p["flags"]) for p in placed]))


def main(argv):
    keep = "--keep" in argv
    tmp = tempfile.mkdtemp(prefix="test_mkimage.")
    h = Harness(tmp)
    try:
        for section in (reject_cases, accept_cases, cli_cases, toolchain_case):
            try:
                section(h)
            except Exception as e:  # a crashing section is a failure, not a pass
                h.ok(f"{section.__name__} raised", False, repr(e))
    finally:
        if keep:
            print(f"kept {tmp}", file=sys.stderr)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    skip = f", {len(h.skipped)} skipped: {'; '.join(h.skipped)}" if h.skipped else ""
    if h.failed:
        print(f"test_mkimage: FAIL ({len(h.failed)} of {h.passed + len(h.failed)} checks failed{skip})")
        return 1
    print(f"test_mkimage: PASS ({h.passed} checks, rules E1-E10 + CLI + output{skip})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
