#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
MP1 (ADR 0002, issue #52): boot the ARM64 Image on the pinned QEMU virt-8.2 /
cortex-a72 target and assert the ordered boot markers, including the bounded
DTB copy/parse/validate markers.

Discipline (mirrors the x86 smoke harness + the ADR 0002 D6 evidence rule):
  - capture serial to a FILE, never stdout (stdout races the timeout kill and
    looks empty even when the UART produced output); --log keeps that file;
  - require the exact ordered markers and the terminal line; a halted emulator
    with no markers is a FAILURE, not success;
  - reject any "FATAL" line (in --dtb-bad mode: only the one expected FATAL);
  - -no-reboot so a triple-fault/reset can't masquerade as a hang;
  - the machine string and -m are PINNED to exactly what
    tools/gen_dtb_fixture.py dumped, and the boot's printed CRC32 of the copied
    live DTB extent must equal QEMU_VIRT82_DTB_CRC32 from the committed fixture
    — proving the DTB that booted is byte-identical to the one the hosted
    parser tests use;
  - record the image SHA-256, tool versions, and the exact QEMU command;
  - markers count only on COMPLETE serial lines (terminated by "\n"; one
    trailing "\r" is stripped, so LF and CRLF logs are judged identically).
    Every expected marker must match a WHOLE line, in order: fixed lines by
    exact equality, value-carrying lines by a full-line pattern anchored at
    the line start (fixed text + exact-width hex fields). A marker's text
    appearing mid-line, or in a line still being written, never counts. The
    mode's terminal marker (in --dtb-bad mode the expected FATAL line) must be
    the last line;
  - QEMU stderr is judged on EVERY run, PASS included: after QEMU is
    terminated and reaped, each non-empty stderr line must be exactly the
    SIGTERM notice QEMU 8.2 prints when this harness kills it
    ("qemu-system-aarch64: terminating on signal 15 from pid <our pid>
    (<name>)", name optional), at most once; any other stderr output (a QEMU
    internal error, a device warning) is a FAIL and is printed;
  - poll the serial FILE while QEMU runs: once the terminal line is seen, keep
    QEMU running for a full SETTLE_S window to catch late output, then
    terminate and reap QEMU and re-read the WHOLE log. All validation runs on
    that final log, so anything QEMU wrote up to the kill is judged: the
    terminal line must be the LAST line (any line after it is a FAIL), a
    trailing partial (unterminated) line is a FAIL, and any FATAL other than
    --dtb-bad's one expected line is a FAIL. Only that path is PASS-eligible;
  - --timeout (finite, > 0) bounds the whole run INCLUDING the settle window:
    terminal line not seen by the deadline -> FAIL ("timeout before final
    marker"); seen with less than SETTLE_S left -> FAIL ("timeout before
    settle complete"), since a PASS requires a full settle window. The kernel
    halts forever in WFE, so QEMU exiting on its own is a crash/abort: FAIL
    with its return code. The child is always terminated and reaped (no
    zombie); that reap (<= 5 s, then SIGKILL) is the only time outside the
    deadline. Every --version probe is bounded by VERSION_TIMEOUT_S;
  - --vector-test pins every vector line EXACTLY: the PASS line is the full
    literal text main.c prints, and the raw "elr=" must equal the address of
    the global symbol je_arm_brk_site, resolved by a bounded pure-Python
    .symtab scan of the ELF the Image was built from (--elf, default the
    Image's sibling jeios-arm.elf). The ELF is bound to the Image: the Image
    word at (je_arm_brk_site - _text) must be BRK #0xa55a. A missing,
    malformed, stripped or mismatched ELF under --vector-test is a FAIL (never
    a SKIP); --require additionally demands the ELF exist in every mode;
  - assert pa_bits EXACTLY (--expect-pa-bits, default 44 for the pinned
    virt-8.2 / cortex-a72 profile);
  - --phys-alloc (MP2 PR-1a, ADR 0011 D5) pins the physmem bring-up block and
    runs an INDEPENDENT oracle: the block must be contiguous, between the
    "validated" and the MMU marker block, in the exact grammar order main.c
    emits;
    every input it prints is cross-bound to a source the kernel does not
    control here (window == pinned constants, kernel reserve == ELF
    [_text,_end), dtb reserve == recorded dtb pointer + fixture totalsize,
    ram/rsv/initrd lines == the earlier "dtb parsed" lines); the free set is
    then recomputed from first principles (page fully inside some RAM range
    AND touching no reservation) and the kernel's free/alloc1/allocmax/overcap
    values must agree with it. The kernel's own count is never trusted;
  - every good-boot mode runs the MMU oracle (MP2 PR-1b, ADR 0011): the 7
    mmu lines form one contiguous block and each value is recomputed from the
    ELF symtab / DDI0487 layouts (map ranges, guard, root, leaf count, exact
    SCTLR incl. M|C|I|WXN and EE=0, PAR F/PA/ATTR/SH/FST) — never trusted;
  - --mmu-fault NAME (ARM_MMU_FAULT build) pins the deliberate W^X / guard
    fault: AT probes, raw ESR fields (EC, IL, FSC, WnR, S1PTW/CM/EA/FnV),
    FAR/ELR/VBAR against ELF symbols bound to the Image, the in-kernel PASS
    line and the halted terminal. TCG has no caches: no cache evidence;
  - SKIP (exit 0, clearly labelled) if the aarch64 toolchain /
    qemu-system-aarch64 / image is unavailable — never report PASS when nothing
    ran. With --require (remote CI) every such SKIP is a FAIL (exit 1), and
    the preflight additionally requires QEMU 8.2.x and a parseable fixture.
"""
import argparse
import hashlib
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

QEMU = "qemu-system-aarch64"
TOOLCHAIN_CC = "aarch64-linux-gnu-gcc"
# Pinned to match tools/gen_dtb_fixture.py exactly (the DTB depends on every one
# of these: dtb-randomness=off removes rng-seed/kaslr-seed; -m sets /memory).
MACHINE = "virt-8.2,gic-version=2,virtualization=off,secure=off,dtb-randomness=off"
CPU = "cortex-a72"
MEMORY = "256M"
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                       "tests", "fixtures", "qemu_virt82_dtb.h")
JE_FDT_MAX_COPY = 64 * 1024
# The fixture was dumped by QEMU 8.2; another QEMU major/minor may emit a
# different DTB for the same versioned machine, so remote CI pins 8.2.x.
QEMU_REQUIRED_VERSION = (8, 2)
QEMU_VERSION_RE = re.compile(r"QEMU emulator version (\d+)\.(\d+)\.(\d+)")
# After the final marker: how long to keep capturing so unexpected trailing
# output (a late FATAL, an exception dump) is still caught before the kill.
SETTLE_S = 0.5
POLL_S = 0.05
# Bound on every `tool --version` probe so a wedged binary cannot hang CI.
VERSION_TIMEOUT_S = 10
# A 16-digit lowercase hex field, the width the kernel prints every hex value at.
HEX16 = rb"0x[0-9a-f]{16}"

# Ordered step-1 markers. "bss cleared" (default) vs "bss clear verified"
# (patterned-BSS build) — accept either, but if the verified form is present it
# must be the verified one; see --bss-verified.
#
# A marker is (description, line -> bool) and matches one WHOLE normalized
# line (see split_lines): exact() for fixed lines, valued() for lines whose
# tail carries build-dependent values (addresses), matched as a full-line
# pattern anchored at the line start.


def exact(line):
    return (line, lambda ln: ln == line)


def valued(desc, pattern):
    rx = re.compile(pattern)
    return (desc, lambda ln: rx.fullmatch(ln) is not None)


BASE_MARKERS = [
    exact(b"JeiOS/aarch64: entry EL1"),
    valued(b"JeiOS/aarch64: stack ready sp=<hex>",
           rb"JeiOS/aarch64: stack ready sp=" + HEX16),
    valued(b"JeiOS/aarch64: tcr_el1 initialized (tbi0=0) tcr=<hex>",
           rb"JeiOS/aarch64: tcr_el1 initialized \(tbi0=0\) tcr=" + HEX16),
    None,  # bss line, filled below
    valued(b"JeiOS/aarch64: dtb pointer recorded dtb=<hex>",
           rb"JeiOS/aarch64: dtb pointer recorded dtb=" + HEX16),
    valued(b"JeiOS/aarch64: vectors installed vbar=<hex>",
           rb"JeiOS/aarch64: vectors installed vbar=" + HEX16),
]
# Default build terminal marker (no vector test).
TERM_DEFAULT = b"JeiOS/aarch64: initialization complete; halted"
VALIDATED = b"JeiOS/aarch64: boot context validated pa_bits="
VALIDATED_RE = re.compile(rb"JeiOS/aarch64: boot context validated pa_bits=(\d+)")
# The one stderr line QEMU 8.2.2 (os-posix.c qemu_system_killed path) emits
# when we SIGTERM it; observed verbatim on 8.2.2:
#   "qemu-system-aarch64: terminating on signal 15 from pid 1561788 (/bin/bash)"
# The "(name)" part is absent when QEMU cannot resolve the sender's name.
QEMU_KILL_RE = re.compile(
    rb"qemu-system-aarch64: terminating on signal 15 from pid (\d+)(?: \([^\n]*\))?")
# Bound on how much stderr is read/judged; more than this is itself a FAIL.
STDERR_MAX = 64 * 1024
# Vector-test build: the planted BRK must vector to the EL1h sync slot and the
# handler must assert the exact syndrome. These ordered markers replace the
# default terminal and prove the deliberate exception end-to-end. Every line is
# pinned EXACTLY: the PASS text is the full literal kernel/arch/aarch64/main.c
# prints (if main.c's text changes, this must change with it), and the ELR is
# the address of the global symbol je_arm_brk_site resolved from the ELF the
# Image was built from (see resolve_brk_site), not "any hex".
VECTOR_PASS = (b"JeiOS/aarch64: vector self-test PASS (planted BRK #0xa55a "
               b"vectored to EL1h sync slot 4 with exact ESR 0xf200a55a; "
               b"ELR==brk site; SPSR EL1h+DAIF-masked)")
BRK_SITE_SYM = b"je_arm_brk_site"
IMAGE_BASE_SYM = b"_text"  # linker.ld: the Image is the ELF from _text on
# A64 BRK #imm16 encoding (DDI0487 C6.2 BRK): 0xd4200000 | imm16 << 5.
BRK_A55A_INSN = 0xD4200000 | (0xA55A << 5)


def vector_test_markers(brk_site):
    return [
        exact(b"JeiOS/aarch64: triggering deliberate BRK #0xa55a ..."),
        exact(b"JeiOS/aarch64: EXCEPTION vector=0x0000000000000004 "
              b"ec=0x000000000000003c il=0x0000000000000001 iss=0x000000000000a55a"),
        # Assert the RAW syndrome line independently (not just the in-kernel
        # PASS), so a bug in the kernel's own comparator cannot produce a false
        # PASS: exact ESR, SPSR AND ELR (== &je_arm_brk_site from the ELF).
        exact(b"JeiOS/aarch64:   esr=0x00000000f200a55a elr=" + hx(brk_site)
              + b" spsr=0x00000000800003c5"),
        exact(VECTOR_PASS),
        exact(b"JeiOS/aarch64: exception handled; halted"),
    ]


COPIED_RE = re.compile(
    rb"JeiOS/aarch64: dtb copied len=0x([0-9a-f]{16}) totalsize=0x([0-9a-f]{16})"
    rb" crc32=0x([0-9a-f]{16}) crc_len=0x([0-9a-f]{16})")
COPIED = valued(b"JeiOS/aarch64: dtb copied len=<hex> totalsize=<hex> "
                b"crc32=<hex> crc_len=<hex>", COPIED_RE.pattern)
VALIDATED_LINE = valued(b"JeiOS/aarch64: boot context validated pa_bits=<N>",
                        VALIDATED_RE.pattern)


def hx(v):
    return b"0x%016x" % v


class Fixture:
    """Values parsed from the committed generated fixture header."""

    def __init__(self, path):
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()

        def define(name):
            m = re.search(r"#define\s+%s\s+(0x[0-9a-fA-F]+|\d+)u?\b" % name, src)
            if not m:
                raise ValueError(f"{name} not found in {path}")
            return int(m.group(1), 0)

        self.crc32 = define("QEMU_VIRT82_DTB_CRC32")
        self.length = define("QEMU_VIRT82_DTB_LEN")
        self.totalsize = define("QEMU_VIRT82_DTB_TOTALSIZE")
        body = src.split("{", 1)[1].split("}", 1)[0]
        blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
        if len(blob) < 40:
            raise ValueError(f"fixture {path}: fewer than 40 header bytes")
        if len(blob) != self.length:
            raise ValueError(f"fixture {path}: {len(blob)} bytes != "
                             f"QEMU_VIRT82_DTB_LEN {self.length}")
        self.blob = blob
        self.off_dt_struct = int.from_bytes(blob[8:12], "big")


# Bound on the ELF read: the kernel ELF (with DWARF) is far smaller; anything
# bigger is not the artifact we expect and is refused rather than slurped.
ELF_MAX = 64 * 1024 * 1024
EM_AARCH64 = 183
SHT_SYMTAB = 2
STB_GLOBAL = 1
SHN_LORESERVE = 0xFF00  # st_shndx >= this is ABS/COMMON/XINDEX, not a section


class ElfError(Exception):
    pass


def elf_defined_symbols(data, wanted):
    """Pure-Python ELF64 little-endian AArch64 .symtab scan (no nm, no
    subprocess). Returns {name: [(st_value, binding), ...]} for every name in
    `wanted` that is DEFINED in a real section (0 < st_shndx < SHN_LORESERVE).
    Every offset/size is bounds-checked against `data`; any malformation
    raises ElfError (fail closed, never a guessed address)."""
    def unpack(fmt, off):
        size = struct.calcsize(fmt)
        if off < 0 or off + size > len(data):
            raise ElfError(f"truncated ELF reading {size} bytes at 0x{off:x}")
        return struct.unpack_from(fmt, data, off)

    if data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file (bad magic)")
    if len(data) < 64:  # ELF64 header; checked before indexing data[4]/[5]
        raise ElfError(f"truncated ELF header ({len(data)} < 64 bytes)")
    if data[4] != 2 or data[5] != 1:
        raise ElfError("not ELFCLASS64 little-endian")
    (_, _, e_machine, _, _, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
     _) = unpack("<16sHHIQQQIHHHHHH", 0)
    if e_machine != EM_AARCH64:
        raise ElfError(f"e_machine={e_machine}, need EM_AARCH64 ({EM_AARCH64})")
    if e_shnum == 0 or e_shentsize != 64:
        raise ElfError(f"unsupported section table (shnum={e_shnum}, "
                       f"shentsize={e_shentsize})")
    shdrs = [unpack("<IIQQQQIIQQ", e_shoff + i * 64) for i in range(e_shnum)]
    symtabs = [sh for sh in shdrs if sh[1] == SHT_SYMTAB]
    if len(symtabs) != 1:
        raise ElfError(f"expected exactly one SHT_SYMTAB, found {len(symtabs)} "
                       f"(stripped ELF?)")
    _, _, _, _, sym_off, sym_size, sym_link, _, _, sym_entsize = symtabs[0]
    if sym_entsize != 24 or sym_size % 24 != 0:
        raise ElfError(f"bad .symtab entsize={sym_entsize} size={sym_size}")
    if sym_link >= e_shnum:
        raise ElfError(f".symtab sh_link={sym_link} out of range")
    str_off, str_size = shdrs[sym_link][4], shdrs[sym_link][5]
    if str_off + str_size > len(data):
        raise ElfError("string table extends past end of file")
    strtab = data[str_off:str_off + str_size]
    found = {name: [] for name in wanted}
    for i in range(sym_size // 24):
        st_name, st_info, _, st_shndx, st_value, _ = unpack("<IBBHQQ",
                                                            sym_off + i * 24)
        if st_name >= len(strtab):
            raise ElfError(f"symbol {i}: name offset 0x{st_name:x} out of range")
        end = strtab.find(b"\0", st_name)
        if end < 0:
            raise ElfError(f"symbol {i}: unterminated name")
        name = strtab[st_name:end]
        if name in found and 0 < st_shndx < SHN_LORESERVE:
            found[name].append((st_value, st_info >> 4))
    return found


def resolve_brk_site(elf_path, image_path):
    """Address of the global je_arm_brk_site from the ELF the Image was built
    from, cross-checked against the Image itself: the Image (objcopy -O binary
    from _text) must hold BRK #0xa55a at offset (je_arm_brk_site - _text). That
    binds the ELF to the booted Image, so a stale/foreign ELF FAILs instead of
    supplying a plausible address. Calls fail() on any problem."""
    try:
        if os.path.getsize(elf_path) > ELF_MAX:
            fail(f"--elf {elf_path} larger than {ELF_MAX} bytes")
        with open(elf_path, "rb") as f:
            data = f.read(ELF_MAX + 1)
        syms = elf_defined_symbols(data, (BRK_SITE_SYM, IMAGE_BASE_SYM))
    except (OSError, ElfError) as e:
        fail(f"--vector-test: cannot resolve {BRK_SITE_SYM.decode()} from "
             f"--elf {elf_path}: {e}")
    brk = syms[BRK_SITE_SYM]
    if len(brk) != 1 or brk[0][1] != STB_GLOBAL:
        fail(f"--vector-test: need exactly one defined GLOBAL "
             f"{BRK_SITE_SYM.decode()} in {elf_path}, found {brk}")
    base = syms[IMAGE_BASE_SYM]
    if len(base) != 1:
        fail(f"--vector-test: need exactly one defined {IMAGE_BASE_SYM.decode()} "
             f"in {elf_path}, found {base}")
    site, text = brk[0][0], base[0][0]
    with open(image_path, "rb") as f:
        image = f.read()
    off = site - text
    if off < 0 or off + 4 > len(image):
        fail(f"--vector-test: {BRK_SITE_SYM.decode()}=0x{site:x} outside Image "
             f"[_text=0x{text:x}, +0x{len(image):x}) (ELF/Image mismatch)")
    insn = int.from_bytes(image[off:off + 4], "little")
    if insn != BRK_A55A_INSN:
        fail(f"--vector-test: Image word at {BRK_SITE_SYM.decode()} offset "
             f"0x{off:x} is 0x{insn:08x}, not BRK #0xa55a (0x{BRK_A55A_INSN:08x})"
             f"; --elf {elf_path} is not the ELF this Image was built from")
    print(f"elf:   {elf_path}  sha256={hashlib.sha256(data).hexdigest()}  "
          f"{BRK_SITE_SYM.decode()}=0x{site:016x} (Image word 0x{insn:08x} "
          f"== BRK #0xa55a)")
    return site


def tool_version(cmd):
    try:
        out = subprocess.check_output([cmd, "--version"], text=True,
                                      stderr=subprocess.STDOUT,
                                      timeout=VERSION_TIMEOUT_S)
        return out.splitlines()[0]
    except (OSError, subprocess.SubprocessError, IndexError):
        return "(unknown)"


def qemu_version(cmd):
    """(major, minor, micro) parsed from `cmd --version`, or None."""
    try:
        out = subprocess.check_output([cmd, "--version"], text=True,
                                      stderr=subprocess.STDOUT,
                                      timeout=VERSION_TIMEOUT_S)
    except (OSError, subprocess.SubprocessError):
        return None
    m = QEMU_VERSION_RE.search(out)
    return tuple(int(g) for g in m.groups()) if m else None


def split_lines(out):
    """(complete_lines, partial): complete lines are those terminated by \n,
    with the \n and one trailing \r removed; partial is any unterminated
    tail (b"" if the log ends on a line boundary)."""
    parts = out.split(b"\n")
    partial = parts.pop()  # bytes after the last \n (all of out if none)
    return [ln[:-1] if ln.endswith(b"\r") else ln for ln in parts], partial


def line_of(raw):
    """A raw log segment as a bare line (one trailing \r stripped)."""
    return raw[:-1] if raw.endswith(b"\r") else raw


def run_qemu(cmd, serial_path, term_line, early_stop, timeout):
    """Run QEMU, polling the serial FILE, bounded by `timeout` seconds overall
    (settle included). Returns (final_log, early, stderr): final_log is the
    WHOLE serial file re-read after QEMU has been terminated and reaped;
    stderr is everything QEMU wrote to stderr up to that reap (at most
    STDERR_MAX + 1 bytes, so an oversize stream is detectable); early is
    True if we stopped on the early_stop substring (a run that cannot PASS;
    the caller's validation must then FAIL). PASS-eligible only when the
    terminal line was seen as a complete line, a full SETTLE_S window then
    elapsed inside the deadline with QEMU still running, and we killed it.
    Every other path calls fail()."""
    # stderr goes to an unlinked temp FILE, not a pipe: an undrained pipe could
    # fill and block QEMU mid-boot.
    errf = tempfile.TemporaryFile(prefix="jeios-arm-qemu-stderr-")

    def read_serial():
        try:
            with open(serial_path, "rb") as f:
                return f.read()
        except OSError:
            return b""  # QEMU may not have created it yet

    deadline = time.monotonic() + timeout
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=errf)

    def stop():
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    def read_stderr():
        errf.seek(0)
        return errf.read(STDERR_MAX + 1)

    def fail_with_log(msg):
        stop()  # so the dumped log is final (everything up to the kill)
        sys.stdout.write(read_serial().decode("ascii", "replace") + "\n")
        err = read_stderr()[:4096].decode("utf-8", "replace").strip()
        if err:
            print("qemu stderr: " + err)
        fail(msg)

    def check_alive():
        rc = proc.poll()
        if rc is not None:
            fail_with_log(f"QEMU exited on its own with return code {rc} (the "
                          f"kernel halts forever, so a self-exit is a "
                          f"crash/abort)")

    try:
        seen = early = False
        while True:
            # Only complete lines count; a line still being written (or a log
            # cut mid-line) can never satisfy the terminal marker.
            lines, _ = split_lines(read_serial())
            if term_line in lines:
                seen = True
                break
            if early_stop is not None and any(early_stop in ln for ln in lines):
                early = True
                break
            check_alive()
            if time.monotonic() >= deadline:
                break
            time.sleep(POLL_S)
        if early:
            stop()
            return read_serial(), True, read_stderr()
        if not seen:
            fail_with_log(f"timeout before final marker after {timeout}s "
                          f"(waiting for: {term_line.decode()})")
        # Settle: PASS needs a FULL window inside the deadline, else late output
        # would go unobserved; never extend past --timeout.
        settle_end = time.monotonic() + SETTLE_S
        if settle_end > deadline:
            fail_with_log(f"timeout before settle complete: terminal marker seen "
                          f"with {max(0.0, deadline - time.monotonic()):.3f}s of "
                          f"--timeout {timeout}s left, need {SETTLE_S}s")
        while time.monotonic() < settle_end:
            check_alive()
            time.sleep(POLL_S)
        check_alive()
        stop()
        # Re-read AFTER terminate+reap: the caller judges everything QEMU wrote
        # up to the kill, not the snapshot that satisfied the poll.
        return read_serial(), False, read_stderr()
    finally:
        # Always stop and reap the child, on every path (incl. fail()'s
        # SystemExit and KeyboardInterrupt).
        stop()
        errf.close()


def fail(msg):
    print(f"QEMU aarch64 smoke: FAIL ({msg})")
    sys.exit(1)


def check_stderr(err):
    """Judge QEMU's stderr on every run (PASS included): each non-empty line
    must be QEMU's SIGTERM notice naming THIS process as the sender, at most
    once. Anything else (a QEMU internal error, a warning) is a FAIL."""
    if len(err) > STDERR_MAX:
        fail(f"qemu stderr exceeds {STDERR_MAX} bytes")
    kills = 0
    bad = []
    for ln in err.split(b"\n"):
        if not ln:
            continue
        m = QEMU_KILL_RE.fullmatch(ln)
        if m is not None and int(m.group(1)) == os.getpid():
            kills += 1
            print("stderr: " + ln.decode("utf-8", "replace") + "  (expected kill notice)")
        else:
            bad.append(ln)
    if bad or kills > 1:
        sys.stdout.write("qemu stderr:\n" + err.decode("utf-8", "replace") + "\n")
        fail(f"unexpected QEMU stderr ({len(bad)} unrecognised line(s), "
             f"{kills} kill notice(s); only one SIGTERM notice from pid "
             f"{os.getpid()} is allowed)")


def check_ordered(lines, markers):
    """Markers in order, each matching one WHOLE normalized line strictly
    after the previous marker's line. Callers have already rejected a partial
    trailing line, so `lines` are exactly the complete lines."""
    pos = 0
    for desc, match in markers:
        idx = next((i for i in range(pos, len(lines)) if match(lines[i])), None)
        if idx is None:
            fail(f"missing/out-of-order marker (whole line): {desc.decode()}")
        pos = idx + 1


def check_lines(out, term):
    """Whole-log line discipline on the FINAL (post-kill) log: no unterminated
    tail, and the terminal marker is exactly the last complete line, appearing
    once — so nothing (a late FATAL, a stray marker, an exception dump) may
    follow it."""
    lines, partial = split_lines(out)
    if partial:
        fail(f"trailing partial (unterminated) serial line: "
             f"{partial[:120].decode('ascii', 'replace')!r}")
    n = lines.count(term)
    if n != 1:
        fail(f"terminal line {term.decode()!r} must appear exactly once as a "
             f"whole line, found {n}")
    if lines[-1] != term:
        after = lines[lines.index(term) + 1:]
        fail(f"unexpected output after terminal line: "
             f"{[ln.decode('ascii', 'replace') for ln in after[:5]]}")


def check_dtb_copied(lines, fx):
    """The copied line must exist once and its CRC must match the fixture."""
    found = [m.groups() for m in map(COPIED_RE.fullmatch, lines) if m]
    if len(found) != 1:
        fail(f"expected exactly one well-formed 'dtb copied' line, found {len(found)}")
    length, total, crc, crc_len = (int(x, 16) for x in found[0])
    want_len = min(fx.totalsize, JE_FDT_MAX_COPY)
    if total != fx.totalsize or length != want_len:
        fail(f"dtb copied len=0x{length:x} totalsize=0x{total:x}; fixture expects "
             f"len=0x{want_len:x} totalsize=0x{fx.totalsize:x}")
    if crc_len != fx.length:
        fail(f"dtb crc_len=0x{crc_len:x} != fixture live extent 0x{fx.length:x}")
    if crc != fx.crc32:
        fail(f"booted DTB crc32=0x{crc:08x} != fixture QEMU_VIRT82_DTB_CRC32="
             f"0x{fx.crc32:08x} (booted DTB differs from the hosted-test fixture)")
    print(f"dtb:   crc32=0x{crc:08x} over 0x{crc_len:x} bytes == fixture")


def dtb_good_markers():
    # Exact for the pinned machine/-m: fixture has an empty memreserve map, no
    # /reserved-memory, no initrd, one /memory reg [0x40000000, +256 MiB).
    return [
        COPIED,
        exact(b"JeiOS/aarch64: dtb parsed ram=1 rsv=0 ignored=0 dynamic=0 initrd=0"),
        exact(b"JeiOS/aarch64:   ram[0] base=" + hx(0x40000000) + b" size="
              + hx(0x10000000)),
        VALIDATED_LINE,
    ]


# ---- --phys-alloc (MP2 PR-1a, ADR 0011 D5) ---------------------------------
# Pinned allocator window for the aarch64 build (include/jeios/boot.h). The
# oracle uses THIS, not the kernel's printed window, and requires they agree.
PHYS_WINDOW = (0x40000000, 0x50000000)
PHYS_PAGE = 4096
PHYS_ALLOC_MAX_PAGES = 256  # JE_ALLOC_MAX_PAGES, include/jeios/alloc.h
PM = rb"JeiOS/aarch64: physmem "
H = rb"0x([0-9a-f]{16})"
PM_WINDOW_RE = re.compile(PM + rb"window base=" + H + rb" limit=" + H)
PM_RESERVE_RE = re.compile(PM + rb"reserve kind=(kernel|dtb|initrd) base=" + H
                           + rb" size=" + H)
PM_RAM_RE = re.compile(PM + rb"ram base=" + H + rb" size=" + H)
PM_RSV_RE = re.compile(PM + rb"reserve kind=rsv base=" + H + rb" size=" + H
                       + rb" nomap=([01])")
PM_UNMANAGED_RE = re.compile(PM + rb"unmanaged ram bytes=" + H)
PM_FREE_RE = re.compile(PM + rb"free pages=" + H + rb" bytes=" + H)
PM_ALLOC1_RE = re.compile(PM + rb"alloc1 pa=" + H)
PM_FREE1 = PM + b"free1 ok"
PM_ALLOCMAX_RE = re.compile(PM + rb"allocmax pa=" + H)
PM_OVERCAP_RE = re.compile(PM + rb"overcap rejected pages=" + H)
DTB_PTR_RE = re.compile(rb"JeiOS/aarch64: dtb pointer recorded dtb=" + H)
DTB_PARSED_RE = re.compile(rb"JeiOS/aarch64: dtb parsed ram=(\d+) rsv=(\d+) "
                           rb"ignored=(\d+) dynamic=(\d+) initrd=([01])")
DTB_RAM_RE = re.compile(rb"JeiOS/aarch64:   ram\[(\d+)\] base=" + H + rb" size=" + H)
MMU = b"JeiOS/aarch64: mmu "
MMU_FAULT_PREFIX = MMU + b"fault "
MMU_BUILD_RE = re.compile(MMU + rb"build root=" + H)
MMU_MAP_RE = re.compile(
    MMU + rb"map text=\[" + H + rb"," + H +
    rb"\) rodata=\[" + H + rb"," + H +
    rb"\) data=\[" + H + rb"," + H +
    rb"\) device=" + H + rb" gicd=" + H + rb" gicc=" + H)
MMU_AUDIT_RE = re.compile(MMU + rb"audit ok leaves=" + H)
MMU_ENABLED_RE = re.compile(MMU + rb"enabled sctlr=" + H)
MMU_XLATE_RE = re.compile(MMU + rb"xlate mapped va=" + H + rb" par=" + H)
MMU_GUARD_RE = re.compile(MMU + rb"xlate guard va=" + H + rb" par=" + H)
MMU_UART_RE = re.compile(MMU + rb"attr uart va=" + H + rb" par=" + H)
# MP2 PR-2a (ADR 0012 D2): independent AT S1E1R probes of the GIC frames.
MMU_GICD_RE = re.compile(MMU + rb"xlate gicd va=" + H + rb" par=" + H)
MMU_GICC_RE = re.compile(MMU + rb"xlate gicc va=" + H + rb" par=" + H)
MMU_GICC_DIR_RE = re.compile(MMU + rb"xlate gicc_dir va=" + H + rb" par=" + H)
MMU_BLOCK_RES = (MMU_BUILD_RE, MMU_MAP_RE, MMU_AUDIT_RE, MMU_ENABLED_RE,
                 MMU_XLATE_RE, MMU_GUARD_RE, MMU_UART_RE,
                 MMU_GICD_RE, MMU_GICC_RE, MMU_GICC_DIR_RE)
MMU_BLOCK_LAST_RE = MMU_BLOCK_RES[-1]

def phys_alloc_markers():
    """Ordered whole-line markers for the pinned -m 256M fixture (one RAM
    range, no rsv, no initrd). The oracle below re-derives every value."""
    return [
        exact(PM + b"window base=" + hx(PHYS_WINDOW[0]) + b" limit="
              + hx(PHYS_WINDOW[1])),
        valued(PM + b"reserve kind=kernel base=<hex> size=<hex>",
               PM + rb"reserve kind=kernel base=" + HEX16 + rb" size=" + HEX16),
        valued(PM + b"reserve kind=dtb base=<hex> size=<hex>",
               PM + rb"reserve kind=dtb base=" + HEX16 + rb" size=" + HEX16),
        valued(PM + b"ram base=<hex> size=<hex>", PM_RAM_RE.pattern),
        valued(PM + b"unmanaged ram bytes=<hex>", PM_UNMANAGED_RE.pattern),
        valued(PM + b"free pages=<hex> bytes=<hex>", PM_FREE_RE.pattern),
        valued(PM + b"alloc1 pa=<hex>", PM_ALLOC1_RE.pattern),
        exact(PM_FREE1),
        valued(PM + b"allocmax pa=<hex>", PM_ALLOCMAX_RE.pattern),
        valued(PM + b"overcap rejected pages=<hex>", PM_OVERCAP_RE.pattern),
    ]


def mmu_markers():
    """Ordered default MMU-enable markers (ADR 0011 D2/D3/D4/D6/D7)."""
    return [
        valued(MMU + b"build root=<hex>", MMU_BUILD_RE.pattern),
        valued(MMU + b"map text/rodata/data/device/gicd/gicc", MMU_MAP_RE.pattern),
        valued(MMU + b"audit ok leaves=<hex>", MMU_AUDIT_RE.pattern),
        valued(MMU + b"enabled sctlr=<hex>", MMU_ENABLED_RE.pattern),
        valued(MMU + b"xlate mapped va=<hex> par=<hex>", MMU_XLATE_RE.pattern),
        valued(MMU + b"xlate guard va=<hex> par=<hex>", MMU_GUARD_RE.pattern),
        valued(MMU + b"attr uart va=<hex> par=<hex>", MMU_UART_RE.pattern),
        valued(MMU + b"xlate gicd va=<hex> par=<hex>", MMU_GICD_RE.pattern),
        valued(MMU + b"xlate gicc va=<hex> par=<hex>", MMU_GICC_RE.pattern),
        valued(MMU + b"xlate gicc_dir va=<hex> par=<hex>", MMU_GICC_DIR_RE.pattern),
    ]


# ---- MMU oracle (MP2 PR-1b, ADR 0011 D2/D3/D4/D7) ---------------------------
# Runs in EVERY good-boot mode. Nothing the kernel prints is trusted as a
# verdict: every value is recomputed from the ELF symtab or from the
# architecture's bit layouts (DDI0487 D19 PAR_EL1, D17.2 ESR_EL1) and compared.
# QEMU TCG models no caches: none of this is cache-maintenance evidence; it
# proves TRANSLATION and PERMISSIONS only.
PL011_PA = 0x09000000
# MP2 PR-2a (ADR 0012 D2): pinned GICv2 frames, one 4 KiB Device page each.
# Cross-checked at startup against the committed fixture DTB (which the boot's
# CRC binds to the DTB QEMU actually passed): /intc@8000000 compatible
# "arm,cortex-a15-gic" reg = <0x0 0x08000000 0x0 0x10000  0x0 0x08010000 0x0
# 0x10000>; /timer interrupts[1] = <GIC_PPI 14 0x104> -> INTID 16+14 = 30.
GICD_PA, GICC_PA = 0x08000000, 0x08010000
TIMER_INTID = 30
SCTLR_INTERIM = 0x30D00808     # boot.S MP1 SCTLR_EL1 (RES1 + SA), MMU/caches off
SCTLR_M, SCTLR_C, SCTLR_I, SCTLR_WXN, SCTLR_EE = 1 << 0, 1 << 2, 1 << 12, 1 << 19, 1 << 25
SCTLR_SET = SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_WXN
MAIR_NORMAL_WB, MAIR_DEVICE_NGNRNE = 0xFF, 0x00
FSC_TRANSLATION_L3, FSC_PERMISSION_L3 = 0x07, 0x0F
MMU_LAYOUT_SYMS = (b"_text", b"__text_end", b"__rodata_end", b"__data_end",
                   b"__stack_guard", b"__stack_bottom", b"_end",
                   b"je_arm_vectors")
A64_RET = 0xD65F03C0
# STR Xt, [Xn, #0] (unsigned-offset, 64-bit) with Rt == XZR; Rn free.
A64_STR_XZR_MASK, A64_STR_XZR_BITS = 0xFFFFFC1F, 0xF900001F


def resolve_syms(elf_path, names, what):
    """{name: address} for each name, exactly one defined copy, from the ELF."""
    try:
        if os.path.getsize(elf_path) > ELF_MAX:
            fail(f"{what}: --elf {elf_path} larger than {ELF_MAX} bytes")
        with open(elf_path, "rb") as f:
            data = f.read(ELF_MAX + 1)
        syms = elf_defined_symbols(data, names)
    except (OSError, ElfError) as e:
        fail(f"{what}: cannot read symbols from --elf {elf_path}: {e}")
    out = {}
    for n in names:
        if len(syms[n]) != 1:
            fail(f"{what}: need exactly one defined {n.decode()} in {elf_path}, "
                 f"found {syms[n]}")
        out[n] = syms[n][0][0]
    return out


def image_word(image, text, addr, what):
    off = addr - text
    if off < 0 or off + 4 > len(image):
        fail(f"{what}: 0x{addr:x} outside the Image [0x{text:x},+0x{len(image):x})")
    return int.from_bytes(image[off:off + 4], "little")


def par_expect_ok(par, va, attr, sh, what):
    """PAR_EL1 after a successful AT (F=0): PA[47:12] must equal the probed VA's
    frame (identity map, recomputed here), ATTR[63:56] and SH[8:7] as given."""
    if par & 1:
        fail(f"{what}: PAR=0x{par:016x} has F=1; VA 0x{va:x} must translate")
    pa = par & 0x0000FFFFFFFFF000
    if pa != (va & 0x0000FFFFFFFFF000):
        fail(f"{what}: PAR.PA=0x{pa:x} != identity frame of VA 0x{va:x}")
    if (par >> 56) & 0xFF != attr:
        fail(f"{what}: PAR.ATTR=0x{(par >> 56) & 0xFF:02x} != 0x{attr:02x}")
    if (par >> 7) & 3 != sh:
        fail(f"{what}: PAR.SH=0b{(par >> 7) & 3:02b} != 0b{sh:02b}")


def par_expect_fault(par, fst, what):
    """PAR_EL1 after a faulting AT (F=1): FST[6:1] must be the given stage-1
    fault, PTW[8]=0 (not a walk fault), S[9]=0 (stage 1)."""
    if not par & 1:
        fail(f"{what}: PAR=0x{par:016x} has F=0; the access must NOT translate")
    if (par >> 1) & 0x3F != fst:
        fail(f"{what}: PAR.FST=0x{(par >> 1) & 0x3F:02x} != 0x{fst:02x}")
    if (par >> 8) & 1 or (par >> 9) & 1:
        fail(f"{what}: PAR=0x{par:016x} reports PTW/S2 fault; expected stage-1 leaf fault")


def mmu_expected_leaves(regions_pages, root):
    """Independent leaf count for the audit line: every mapped 4 KiB page of the
    requested regions, plus one RW identity leaf per page-table frame. The table
    frames are modelled as a contiguous first-fit run [root, root + T pages)
    from the PR-1a allocator (root is the first allocation); T is the number of
    distinct L0/L1/L2/L3 tables the full VA set needs, iterated to a fixed point
    because mapping a table frame can itself need a new table. If the allocator
    ever stops being first-fit-contiguous this check FAILS (never false-passes)
    and must be re-derived."""
    base = set(regions_pages)
    tables = 1
    while True:
        vas = base | {root + i * PHYS_PAGE for i in range(tables)}
        need = 1 + len({va >> 39 for va in vas}) + len({va >> 30 for va in vas}) \
            + len({va >> 21 for va in vas})
        if need == tables:
            return len(vas), tables
        tables = need


def check_mmu(lines, syms):
    """Default MMU block: one contiguous block of the 10 lines, each value
    recomputed independently. Returns the parsed block for later checks."""
    idx = [i for i, ln in enumerate(lines)
           if ln.startswith(MMU) and not ln.startswith(MMU_FAULT_PREFIX)]
    if len(idx) != len(MMU_BLOCK_RES) or idx != list(range(idx[0], idx[0] + len(idx))):
        fail(f"mmu: expected one contiguous block of {len(MMU_BLOCK_RES)} mmu lines, "
             f"found {len(idx)} at {idx}")
    m = []
    for i, rx in zip(idx, MMU_BLOCK_RES):
        g = rx.fullmatch(lines[i])
        if g is None:
            fail(f"mmu: unknown/out-of-order mmu line {lines[i].decode('ascii', 'replace')!r}")
        m.append([int(x, 16) for x in g.groups()])
    ((root,), mp, (leaves,), (sctlr,), (x_va, x_par), (g_va, g_par), (u_va, u_par),
     (gd_va, gd_par), (gc_va, gc_par), (dir_va, dir_par)) = m

    s = syms
    want_map = [s[b"_text"], s[b"__text_end"], s[b"__text_end"], s[b"__rodata_end"],
                s[b"__rodata_end"], s[b"__data_end"], PL011_PA, GICD_PA, GICC_PA]
    if mp != want_map:
        fail("mmu map: kernel " + " ".join(f"0x{v:x}" for v in mp) +
             " != ELF " + " ".join(f"0x{v:x}" for v in want_map))
    if s[b"__stack_guard"] != s[b"__data_end"] or \
            s[b"__stack_bottom"] != s[b"__stack_guard"] + PHYS_PAGE:
        fail(f"mmu: ELF guard layout broken: __data_end=0x{s[b'__data_end']:x} "
             f"__stack_guard=0x{s[b'__stack_guard']:x} __stack_bottom=0x{s[b'__stack_bottom']:x}")
    if root % PHYS_PAGE or not PHYS_WINDOW[0] <= root < PHYS_WINDOW[1] or \
            s[b"_text"] <= root < s[b"_end"]:
        fail(f"mmu build: root=0x{root:x} not a page in the allocator window "
             f"outside the image [0x{s[b'_text']:x},0x{s[b'_end']:x})")

    pages = []
    for lo, hi in ((s[b"_text"], s[b"__data_end"]), (s[b"__stack_bottom"], s[b"_end"]),
                   (PL011_PA, PL011_PA + PHYS_PAGE), (GICD_PA, GICD_PA + PHYS_PAGE),
                   (GICC_PA, GICC_PA + PHYS_PAGE)):
        pages += range(lo, hi, PHYS_PAGE)
    if s[b"__stack_guard"] in pages:
        fail("mmu: oracle bug: guard page in the mapped set")
    want_leaves, tables = mmu_expected_leaves(pages, root)
    if leaves != want_leaves:
        fail(f"mmu audit: leaves=0x{leaves:x} != independent count 0x{want_leaves:x} "
             f"({len(pages)} region pages + {tables} table frames)")

    if sctlr & SCTLR_SET != SCTLR_SET:
        fail(f"mmu enabled: sctlr=0x{sctlr:x} missing "
             f"{[n for n, b in (('M', SCTLR_M), ('C', SCTLR_C), ('I', SCTLR_I), ('WXN', SCTLR_WXN)) if not sctlr & b]}")
    if sctlr & SCTLR_EE:
        fail(f"mmu enabled: sctlr=0x{sctlr:x} has EE=1 (must stay little-endian)")
    if sctlr != SCTLR_INTERIM | SCTLR_SET:
        fail(f"mmu enabled: sctlr=0x{sctlr:x} != interim 0x{SCTLR_INTERIM:x} | "
             f"M|C|I|WXN (0x{SCTLR_INTERIM | SCTLR_SET:x}); a bit beyond the pinned set changed")

    if x_va != s[b"_text"]:
        fail(f"mmu xlate mapped: va=0x{x_va:x} != ELF _text 0x{s[b'_text']:x}")
    # RAM probe: Normal WB (MAIR Attr0 0xff), Inner Shareable (0b11).
    par_expect_ok(x_par, x_va, MAIR_NORMAL_WB, 0b11, "mmu xlate mapped")
    if g_va != s[b"__stack_guard"]:
        fail(f"mmu xlate guard: va=0x{g_va:x} != ELF __stack_guard 0x{s[b'__stack_guard']:x}")
    par_expect_fault(g_par, FSC_TRANSLATION_L3, "mmu xlate guard")
    if u_va != PL011_PA:
        fail(f"mmu attr uart: va=0x{u_va:x} != PL011 0x{PL011_PA:x}")
    # Device-nGnRnE (MAIR Attr1 0x00). PAR.SH reads 0b10 for Device memory
    # (DDI0487 PAR_EL1.SH: Device / Normal-NC are reported Outer Shareable).
    par_expect_ok(u_par, u_va, MAIR_DEVICE_NGNRNE, 0b10, "mmu attr uart")
    # GIC frames (ADR 0012 D2): each pinned page translates identity as
    # Device-nGnRnE; GICC_DIR (GICC + 0x1000, IHI0048B 4.4.16) must NOT
    # translate — an EOImode=1/DIR path would take a stage-1 L3 translation
    # fault (GICD, GICC and GICC+0x1000 share one 2 MiB L2 slot, so the L3
    # table exists and the missing leaf is reported at level 3).
    if (gd_va, gc_va) != (GICD_PA, GICC_PA):
        fail(f"mmu xlate gicd/gicc: va=0x{gd_va:x}/0x{gc_va:x} != pinned "
             f"0x{GICD_PA:x}/0x{GICC_PA:x}")
    par_expect_ok(gd_par, gd_va, MAIR_DEVICE_NGNRNE, 0b10, "mmu xlate gicd")
    par_expect_ok(gc_par, gc_va, MAIR_DEVICE_NGNRNE, 0b10, "mmu xlate gicc")
    if dir_va != GICC_PA + PHYS_PAGE:
        fail(f"mmu xlate gicc_dir: va=0x{dir_va:x} != GICC+0x1000 "
             f"0x{GICC_PA + PHYS_PAGE:x}")
    if (GICC_PA >> 21) != (dir_va >> 21):
        fail("mmu: oracle bug: GICC_DIR not in GICC's 2 MiB slot")
    par_expect_fault(dir_par, FSC_TRANSLATION_L3, "mmu xlate gicc_dir")
    print(f"mmu:   root=0x{root:x} leaves=0x{leaves:x} (== {len(pages)} region pages + "
          f"{tables} tables, ELF) sctlr=0x{sctlr:x} (M|C|I|WXN, EE=0)")
    print(f"mmu:   PAR mapped=0x{x_par:016x} (F=0 PA==VA ATTR=0xff SH=IS) "
          f"guard=0x{g_par:016x} (F=1 FST=xlate L3) uart=0x{u_par:016x} "
          f"(F=0 PA==VA ATTR=0x00)")
    print(f"mmu:   PAR gicd=0x{gd_par:016x} gicc=0x{gc_par:016x} (F=0 PA==VA "
          f"ATTR=0x00 Device) gicc_dir=0x{dir_par:016x} (F=1 FST=xlate L3, unmapped)")


# ---- GIC + CNTP timer (MP2 PR-2a, ADR 0012 D2/D3/D4/D6/D8) -----------------
# Runs in EVERY good-boot mode (gic_timer_bringup follows mmu_bringup in every
# build). IRQs stay MASKED in PR-2a: this proves the GIC configuration and, in
# --timer-poll, the polled acknowledge/handle/EOI chain — NOT interrupt
# delivery or the vector path (PR-2b). QEMU TCG models no real interrupt
# latency: the cntpct deltas are a loose lower-bound sanity check, never a
# timing-precision claim.
GIC_INIT_RE = re.compile(rb"JeiOS/aarch64: gic init gicd_typer=" + H
                         + rb" gicd_iidr=" + H + rb" gicc_iidr=" + H)
GIC_CONFIG_RE = re.compile(rb"JeiOS/aarch64: gic config gicc_ctlr=" + H
                           + rb" gicc_pmr=" + H + rb" gicd_ctlr=" + H
                           + rb" isenabler0=" + H + rb" igroupr0=" + H
                           + rb" prio30=" + H)
TIMER_RE = re.compile(rb"JeiOS/aarch64: timer cntfrq=" + H + rb" interval=" + H
                      + rb" intid=" + H)
TIMER_POLL_RE = re.compile(rb"JeiOS/aarch64: timer poll n=" + H + rb" intid=" + H
                           + rb" cntpct=" + H + rb" ticks=" + H)
TIMER_POLL_PREFIX = b"JeiOS/aarch64: timer poll "
TIMER_POLL_CYCLES = 4
TIMER_POLL_PASS = TIMER_POLL_PREFIX + b"PASS cycles=" + hx(TIMER_POLL_CYCLES)
# Machine-type-fixed for QEMU 8.2 virt (TCG generic timer: 62.5 MHz). Pinned,
# NOT recomputed from the kernel's printed cntfrq (that would be circular).
VIRT82_CNTFRQ = 62500000
VIRT82_INTERVAL = VIRT82_CNTFRQ // 100     # 100 Hz (ADR 0012 D6) = 625000
# Observed QEMU 8.2 GICv2 identity (IHI0048B 4.3.3 / 4.4.15): Arm implementer
# 0x43b in [11:0] of both IIDRs.
GIC_ARM_IMPLEMENTER = 0x43B
GICD_TYPER_SECEXT = 1 << 10
GICC_CTLR_ENGRP0, GICC_CTLR_FIQEN, GICC_CTLR_EOIMODE = 1 << 0, 1 << 3, 1 << 9
GIC_TIMER_PRIORITY = 0x80      # ADR 0012 D3 pinned INTID 30 priority


def gic_markers():
    return [
        valued(b"JeiOS/aarch64: gic init gicd_typer=<hex> gicd_iidr=<hex> gicc_iidr=<hex>",
               GIC_INIT_RE.pattern),
        valued(b"JeiOS/aarch64: gic config gicc_ctlr=<hex> gicc_pmr=<hex> "
               b"gicd_ctlr=<hex> isenabler0=<hex> igroupr0=<hex> prio30=<hex>",
               GIC_CONFIG_RE.pattern),
        valued(b"JeiOS/aarch64: timer cntfrq=<hex> interval=<hex> intid=0x..1e",
               TIMER_RE.pattern),
    ]


def timer_poll_markers():
    return [valued(TIMER_POLL_PREFIX + b"n=" + hx(n) +
                   b" intid=<hex> cntpct=<hex> ticks=<hex>",
                   re.escape(TIMER_POLL_PREFIX + b"n=" + hx(n)) + rb" intid="
                   + HEX16 + rb" cntpct=" + HEX16 + rb" ticks=" + HEX16)
            for n in range(1, TIMER_POLL_CYCLES + 1)] + [exact(TIMER_POLL_PASS)]


def fdt_walk(blob):
    """Bounded FDT structure walk of the committed fixture: yields
    (node_path, prop_name, value). Any malformed token/offset is a FAIL."""
    if len(blob) < 40 or blob[:4] != b"\xd0\x0d\xfe\xed":
        fail("fixture DTB: bad magic/header")
    off_s, off_str = struct.unpack(">II", blob[8:16])
    size_s = struct.unpack(">I", blob[36:40])[0]
    i, end, path = off_s, off_s + size_s, []
    if end > len(blob):
        fail("fixture DTB: struct block outside the blob")
    while i + 4 <= end:
        (tok,) = struct.unpack(">I", blob[i:i + 4])
        i += 4
        if tok == 1:            # FDT_BEGIN_NODE
            e = blob.find(b"\0", i, end)
            if e < 0:
                fail("fixture DTB: unterminated node name")
            path.append(blob[i:e].decode("ascii", "replace"))
            i = (e + 4) & ~3
        elif tok == 2:          # FDT_END_NODE
            if not path:
                fail("fixture DTB: unbalanced END_NODE")
            path.pop()
        elif tok == 3:          # FDT_PROP
            ln, no = struct.unpack(">II", blob[i:i + 8])
            i += 8
            if i + ln > end:
                fail("fixture DTB: property overruns struct block")
            e = blob.find(b"\0", off_str + no)
            yield "/".join(path), blob[off_str + no:e], blob[i:i + ln]
            i = (i + ln + 3) & ~3
        elif tok == 4:          # FDT_NOP
            continue
        elif tok == 9:          # FDT_END
            return
        else:
            fail(f"fixture DTB: unknown token 0x{tok:x}")
    fail("fixture DTB: no FDT_END")


def check_dtb_gic_timer(fx):
    """Cross-check the pinned GIC bases + CNTP INTID against the fixture DTB
    (the booted DTB is CRC-bound to it). Root #address/#size-cells are 2."""
    props = {}
    for node, name, val in fdt_walk(fx.blob):
        props.setdefault(node, {})[name] = val
    gic = [n for n, p in props.items()
           if p.get(b"compatible", b"").split(b"\0")[0] == b"arm,cortex-a15-gic"]
    timer = [n for n, p in props.items()
             if b"arm,armv8-timer" in p.get(b"compatible", b"").split(b"\0")]
    if len(gic) != 1 or len(timer) != 1:
        fail(f"fixture DTB: expected one GICv2 and one armv8-timer node, got "
             f"{gic} {timer}")
    reg = props[gic[0]].get(b"reg", b"")
    if len(reg) < 32:
        fail(f"fixture DTB: {gic[0]} reg too short")
    d_base, d_size, c_base, c_size = struct.unpack(">QQQQ", reg[:32])
    if (d_base, c_base) != (GICD_PA, GICC_PA) or d_size < PHYS_PAGE or c_size < PHYS_PAGE:
        fail(f"fixture DTB: GIC reg GICD=0x{d_base:x}+0x{d_size:x} GICC=0x{c_base:x}"
             f"+0x{c_size:x} != pinned GICD=0x{GICD_PA:x} GICC=0x{GICC_PA:x}")
    irq = props[timer[0]].get(b"interrupts", b"")
    if len(irq) != 48:
        fail(f"fixture DTB: timer interrupts has {len(irq)} bytes, want 4 triples")
    # binding order (arm,arch_timer.yaml): secure phys, NON-SECURE EL1 PHYS
    # (CNTP), virt, hyp -> QEMU virt PPIs 13, 14, 11, 10. GICv2 3-cell flags
    # (arm,gic.yaml): [3:0] trigger (4 = level-high), [15:8] PPI CPU mask
    # (QEMU: (1 << smp) - 1 = 0x01 for the pinned single CPU), all else 0.
    # Validate the FULL cell, not only the trigger nibble.
    triples = [struct.unpack(">III", irq[k:k + 12]) for k in range(0, 48, 12)]
    want_flags = (0x01 << 8) | 4
    for (k_, n_, f_), want_num in zip(triples, (13, 14, 11, 10)):
        if k_ != 1 or n_ != want_num or f_ != want_flags:
            fail(f"fixture DTB: timer interrupt <{k_} {n_} 0x{f_:x}> != "
                 f"<1 {want_num} 0x{want_flags:x}> (PPI, level-high, cpumask 0x01)")
    kind, num, flags = triples[1]
    if kind != 1 or 16 + num != TIMER_INTID or flags != 0x104:
        fail(f"fixture DTB: CNTP interrupt <{kind} {num} 0x{flags:x}> is not "
             f"<1 {TIMER_INTID - 16} 0x104> (level-high PPI INTID {TIMER_INTID})")
    print(f"dtb:   gicd=0x{d_base:x} gicc=0x{c_base:x} cntp=PPI {num} "
          f"(INTID {16 + num}, flags 0x{flags:x} = level-high, cpumask 0x01) == pinned")


def check_gic(lines, timer_poll, next_ok):
    """GIC/timer block: three lines directly after the MMU block, each value
    judged against GICv2/DDI0487 semantics and machine-fixed constants (never
    against the kernel's own expectations). In --timer-poll the poll block must
    follow immediately and is re-checked line by line; otherwise no poll line
    may appear at all (a stale poll build in the default dir is a FAIL)."""
    idx = [i for i, ln in enumerate(lines) if GIC_INIT_RE.fullmatch(ln)]
    if len(idx) != 1:
        fail(f"gic: expected exactly one 'gic init' line, found {len(idx)}")
    i = idx[0]
    if MMU_BLOCK_LAST_RE.fullmatch(lines[i - 1]) is None:
        fail("gic: 'gic init' must directly follow the MMU block")
    if i + 2 >= len(lines):
        fail("gic: block truncated")
    gi = GIC_INIT_RE.fullmatch(lines[i])
    gc = GIC_CONFIG_RE.fullmatch(lines[i + 1])
    tm = TIMER_RE.fullmatch(lines[i + 2])
    if gc is None or tm is None:
        fail("gic: 'gic init', 'gic config', 'timer' must be contiguous in order")
    typer, d_iidr, c_iidr = (int(x, 16) for x in gi.groups())
    c_ctlr, pmr, d_ctlr, isen, igrp, prio = (int(x, 16) for x in gc.groups())
    freq, interval, intid = (int(x, 16) for x in tm.groups())
    if typer & GICD_TYPER_SECEXT:
        fail(f"gic init: GICD_TYPER=0x{typer:x} reports SecurityExtn (secure=off pinned)")
    for nm, v in (("gicd_iidr", d_iidr), ("gicc_iidr", c_iidr)):
        if v == 0 or v >> 32 or v & 0xFFF != GIC_ARM_IMPLEMENTER:
            fail(f"gic init: {nm}=0x{v:x} not an Arm (0x43b) GICv2 identity")
    if pmr != 0xFF:
        fail(f"gic config: gicc_pmr=0x{pmr:x} != 0xff")
    if not c_ctlr & GICC_CTLR_ENGRP0 or c_ctlr & (GICC_CTLR_FIQEN | GICC_CTLR_EOIMODE) \
            or c_ctlr != 1:
        fail(f"gic config: gicc_ctlr=0x{c_ctlr:x}: want EnableGrp0=1 FIQEn=0 "
             f"EOImode=0 (exactly 0x1)")
    if d_ctlr != 1:
        fail(f"gic config: gicd_ctlr=0x{d_ctlr:x} != 0x1")
    if not isen >> TIMER_INTID & 1 or isen >> 32:
        fail(f"gic config: isenabler0=0x{isen:x} lacks bit {TIMER_INTID}")
    if isen & ~(0xFFFF | 1 << TIMER_INTID):
        fail(f"gic config: isenabler0=0x{isen:x} enables a PPI other than "
             f"INTID {TIMER_INTID} (SGIs 0-15 are always-enabled on GICv2)")
    # ADR 0012 D3: INTID 30 in group 0 (IHI0048B 4.3.4 IGROUPR bit 30 = 0) at
    # priority 0x80 (IPRIORITYR byte 30, 4.3.11), strictly below PMR 0xff.
    if igrp >> TIMER_INTID & 1 or igrp >> 32:
        fail(f"gic config: igroupr0=0x{igrp:x}: INTID {TIMER_INTID} must be group 0")
    if prio != GIC_TIMER_PRIORITY:
        fail(f"gic config: prio30=0x{prio:x} != 0x{GIC_TIMER_PRIORITY:x}")
    if not prio < pmr:
        fail(f"gic config: prio30=0x{prio:x} not below PMR 0x{pmr:x} (masked)")
    if freq != VIRT82_CNTFRQ or interval != VIRT82_INTERVAL:
        fail(f"timer: cntfrq={freq} interval={interval}, virt-8.2 pins "
             f"{VIRT82_CNTFRQ}/{VIRT82_INTERVAL}")
    if intid != TIMER_INTID:
        fail(f"timer: intid=0x{intid:x} != 0x{TIMER_INTID:x}")
    print(f"gic:   typer=0x{typer:x} iidr=0x{d_iidr:x}/0x{c_iidr:x} gicc_ctlr=0x{c_ctlr:x} "
          f"pmr=0x{pmr:x} gicd_ctlr=0x{d_ctlr:x} isenabler0=0x{isen:x} "
          f"igroupr0=0x{igrp:x} (INTID {TIMER_INTID} group 0) prio30=0x{prio:x}")
    print(f"timer: cntfrq={freq} interval={interval} intid={intid} (pinned virt-8.2)")

    poll = [k for k, ln in enumerate(lines) if ln.startswith(TIMER_POLL_PREFIX)]
    if not timer_poll:
        if poll:
            fail("timer poll lines present without --timer-poll (wrong build?)")
        if i + 3 >= len(lines) or not next_ok(lines[i + 3]):
            fail("gic: timer line not directly followed by the mode's next marker")
        return
    want = list(range(i + 3, i + 3 + TIMER_POLL_CYCLES + 1))
    if poll != want:
        fail(f"timer poll: expected {TIMER_POLL_CYCLES} poll lines + PASS directly "
             f"after the timer line, found at {poll}")
    prev_pct = None
    for n, k in enumerate(want[:-1], start=1):
        m = TIMER_POLL_RE.fullmatch(lines[k])
        if m is None:
            fail(f"timer poll: malformed line {lines[k].decode('ascii', 'replace')!r}")
        pn, pid, pct, ticks = (int(x, 16) for x in m.groups())
        if pn != n or pid != TIMER_INTID:
            fail(f"timer poll: line {n} has n={pn} intid=0x{pid:x}")
        # Only handle_one bumps ticks and nothing ran before the proof, so the
        # count must be exactly n (strict 1..4), not merely increasing.
        if ticks != n:
            fail(f"timer poll: ticks={ticks} at n={n}, want exactly {n}")
        if prev_pct is not None:
            if pct <= prev_pct:
                fail(f"timer poll: cntpct not increasing at n={n}")
            if pct - prev_pct < VIRT82_INTERVAL:
                fail(f"timer poll: cntpct delta {pct - prev_pct} < interval "
                     f"{VIRT82_INTERVAL} at n={n} (re-arm/ISTATUS broken)")
        prev_pct = pct
    if lines[want[-1]] != TIMER_POLL_PASS:
        fail("timer poll: PASS line missing or malformed")
    if want[-1] + 1 >= len(lines) or not next_ok(lines[want[-1] + 1]):
        fail("timer poll: PASS line not directly followed by the terminal line")
    print(f"poll:  {TIMER_POLL_CYCLES} polled ACK/EOI cycles, ticks 1..{TIMER_POLL_CYCLES}, "
          f"each cntpct delta >= {VIRT82_INTERVAL} (TCG: lower bound only; IRQs masked)")


# ---- --timer-test (MP2 PR-2b returning EL1h IRQ, ADR 0012 D5/D8) -------------
# The kernel prints per-tick evidence recorded IN the IRQ handler after the
# register-canary loop (vectors.S je_arm_canary_loop) returns, then runs a BRK
# negative control. Every field is judged here against architecture semantics
# and the ELF/Image, never against the kernel's own PASS. QEMU TCG has no
# latency model: cntpct deltas are a lower-bound sanity check only.
IRQ_TICK_PREFIX = b"JeiOS/aarch64: irq "
IRQ_TICK_RE = re.compile(rb"JeiOS/aarch64: irq tick n=" + H + rb" vector=" + H
                         + rb" intid=" + H + rb" isr=" + H + rb" spsr=" + H
                         + rb" elr=" + H + rb" sp=" + H + rb" cntpct=" + H
                         + rb" hb=" + H)
IRQ_PASS_RE = re.compile(rb"JeiOS/aarch64: irq timer PASS ticks=" + H)
IRQ_CANARY_HI = b"JeiOS/aarch64: irq canary-hi PASS (x29,x30)"
IRQ_TICKS = 4                  # JE_ARM_IRQ_EVIDENCE_CAP; loop exits at >= 4
IRQ_VECTOR = 5                 # VBAR+0x280: Current EL, SP_ELx, IRQ
ISR_EL1_I = 1 << 7             # DDI0487 D19 ISR_EL1.I (IRQ pending)
SPSR_M_EL1H = 0x5              # SPSR.M[3:0] EL1h; M[4]=0 (AArch64)
SPSR_D, SPSR_A, SPSR_I, SPSR_F = 1 << 9, 1 << 8, 1 << 7, 1 << 6
IRQ_BRK_SYM = b"je_arm_irq_brk_site"
CANARY_SYM = b"je_arm_canary_loop"
CANARY_HI_SYM = b"je_arm_canary_hi_loop"   # (b) rotating-scratch x29/x30 phase
STACK_SYMS = (b"__stack_bottom", b"__stack_top")
BRK_B2B2_INSN = 0xD4200000 | (0xB2B2 << 5)
# A64 encodings (DDI0487 C6.2): SUB (imm) sp,sp,#128; MSR DAIFClr/DAIFSet #2.
CANARY_ENTRY_INSN = 0xD10203FF
MSR_DAIFCLR_I = 0xD50342FF
MSR_DAIFSET_I = 0xD50342DF
CANARY_SCAN_MAX = 4096         # bytes scanned from the symbol for the window
IRQ_BRK_TRIGGER = b"JeiOS/aarch64: triggering deliberate IRQ-path BRK #0xb2b2 ..."
IRQ_BRK_EXC = (b"JeiOS/aarch64: EXCEPTION vector=0x0000000000000004 "
               b"ec=0x000000000000003c il=0x0000000000000001 iss=0x000000000000b2b2")
IRQ_BRK_ESR_RE = re.compile(rb"JeiOS/aarch64:   esr=0x00000000f200b2b2 elr=" + H
                            + rb" spsr=" + H)
IRQ_BRK_FAR_RE = re.compile(rb"JeiOS/aarch64:   far=n/a vbar=" + H)
VBAR_RE = re.compile(rb"JeiOS/aarch64: vectors installed vbar=" + H)
TERM_EXC = b"JeiOS/aarch64: exception handled; halted"
DOUBLE_FAULT = b"double fault"
TIMER_STALLED = b"JeiOS/aarch64: FATAL timer stalled"
LATE_LOW_SYM = b"je_arm_canary_late_low_site"
LATE_HIGH_SYM = b"je_arm_canary_late_high_site"
CANARY_NOP = 0xD503201F
MOVZ_X0_ZERO = 0xD2800000
MOVZ_X29_ZERO = MOVZ_X0_ZERO | 29
MSR_CNTP_TVAL_MASK = 0xFFFFFFE0
MSR_CNTP_TVAL_BITS = 0xD51BE200
MSR_CNTP_CTL_ENABLE_X4 = 0xD51BE224


def timer_test_expect(elf_path, image_path):
    """Resolve the canary loop's IRQ-unmasked window, the BRK site and the
    kernel stack bounds from the ELF, each bound to the booted Image.

    The canary symbol carries no .size, so the window is derived from the
    Image bytes instead of a next-symbol guess: the symbol's first word must be
    the loop's `sub sp, sp, #128`; the single `msr daifclr, #2` that follows
    opens the window, and the first `msr daifset, #2` after it closes it. With
    I clear, an IRQ is taken before the next instruction executes, so a valid
    ELR is in [daifclr + 4, daifset] (inclusive: an IRQ taken just before the
    daifset executes has ELR == daifset). The failure path's second daifset
    lies after the first and is therefore OUTSIDE the window — a tick that
    interrupted the FAIL path is rejected."""
    syms = resolve_syms(elf_path, (CANARY_SYM, CANARY_HI_SYM, IRQ_BRK_SYM,
                        IMAGE_BASE_SYM) + STACK_SYMS, "timer-test")
    with open(image_path, "rb") as f:
        image = f.read()
    text, loop = syms[IMAGE_BASE_SYM], syms[CANARY_SYM]
    if image_word(image, text, loop, "timer-test canary") != CANARY_ENTRY_INSN:
        fail(f"timer-test: Image word at {CANARY_SYM.decode()}=0x{loop:x} is not "
             f"`sub sp, sp, #128` (ELF/Image mismatch or loop changed)")
    # Scan only the PRIMARY loop's body — stop at je_arm_canary_hi_loop, the (b)
    # phase, which has its own daifclr/daifset. The printed `irq tick` evidence
    # lines (cap 4) are all from the primary loop, so their ELRs belong to THIS
    # window; the hi-loop's ticks are not printed as tick lines.
    hi = syms[CANARY_HI_SYM]
    scan_end = min(CANARY_SCAN_MAX, hi - loop) if hi > loop else CANARY_SCAN_MAX
    words = [(loop + k, image_word(image, text, loop + k, "timer-test canary"))
             for k in range(0, scan_end, 4) if loop + k + 4 - text <= len(image)]
    clr = [a for a, w in words if w == MSR_DAIFCLR_I]
    if len(clr) != 1:
        fail(f"timer-test: expected exactly one `msr daifclr, #2` within "
             f"{CANARY_SCAN_MAX} bytes of {CANARY_SYM.decode()}, found {len(clr)}")
    sets = [a for a, w in words if w == MSR_DAIFSET_I and a > clr[0]]
    if not sets:
        fail("timer-test: no `msr daifset, #2` after the canary unmask")
    brk = syms[IRQ_BRK_SYM]
    if image_word(image, text, brk, "timer-test brk") != BRK_B2B2_INSN:
        fail(f"timer-test: Image word at {IRQ_BRK_SYM.decode()}=0x{brk:x} is not "
             f"BRK #0xb2b2 (0x{BRK_B2B2_INSN:08x})")
    exp = {"lo": clr[0] + 4, "hi": sets[0], "brk": brk,
           "stack": (syms[STACK_SYMS[0]], syms[STACK_SYMS[1]])}
    print(f"irq:   canary unmasked window ELR in [0x{exp['lo']:x}, 0x{exp['hi']:x}] "
          f"(Image-derived from {CANARY_SYM.decode()}=0x{loop:x}); "
          f"{IRQ_BRK_SYM.decode()}=0x{brk:x} == BRK #0xb2b2")
    return exp


def make_timer_negative_image(elf_path, image_path, late_corruption,
                              stalled_timer):
    """Create a disposable timer-test Image with one deliberate proof fault.

    The source image remains untouched. Late-register profiles replace the
    named assembly NOP with MOVZ X0/X29,#0. The stalled-timer profile replaces
    the initial CNTP_CTL enable (following the first CNTP_TVAL write in the
    Image) with NOP, leaving the handler's re-arm write intact. All sites are
    symbol/encoding checked so a changed Image layout fails closed.
    """
    if late_corruption is not None and stalled_timer:
        fail("timer negative profiles are mutually exclusive")
    if late_corruption is None and not stalled_timer:
        return image_path

    names = [IMAGE_BASE_SYM]
    if late_corruption == "low":
        names.append(LATE_LOW_SYM)
    elif late_corruption == "high":
        names.append(LATE_HIGH_SYM)
    syms = resolve_syms(elf_path, tuple(names), "timer negative profile")
    with open(image_path, "rb") as f:
        image = bytearray(f.read())
    text = syms[IMAGE_BASE_SYM]

    if late_corruption is not None:
        site = syms[LATE_LOW_SYM if late_corruption == "low"
                    else LATE_HIGH_SYM]
        old = image_word(image, text, site, "timer negative patch site")
        if old != CANARY_NOP:
            fail(f"timer negative profile: {late_corruption} site "
                 f"0x{site:x} contains 0x{old:08x}, not NOP")
        off = site - text
        image[off:off + 4] = (MOVZ_X0_ZERO if late_corruption == "low"
                               else MOVZ_X29_ZERO).to_bytes(4, "little")
        print(f"negative: patched late-{late_corruption} canary site "
              f"0x{site:x} in disposable Image")

    if stalled_timer:
        matches = [
            off for off in range(0, len(image) - 3, 4)
            if (int.from_bytes(image[off:off + 4], "little")
                & MSR_CNTP_TVAL_MASK) == MSR_CNTP_TVAL_BITS
        ]
        if len(matches) != 2:
            fail("timer negative profile: expected exactly two CNTP_TVAL "
                 f"writes, found {len(matches)}")
        enable_off = matches[0] + 8
        enable = int.from_bytes(image[enable_off:enable_off + 4], "little")
        if enable != MSR_CNTP_CTL_ENABLE_X4:
            fail("timer negative profile: initial CNTP_TVAL is not followed "
                 f"by the expected CNTP_CTL enable (0x{enable:08x})")
        image[enable_off:enable_off + 4] = CANARY_NOP.to_bytes(4, "little")
        print(f"negative: patched initial CNTP_CTL enable at Image offset "
              f"0x{enable_off:x} in disposable Image")

    fd, patched = tempfile.mkstemp(prefix="jeios-arm-timer-negative-",
                                    suffix=".Image")
    with os.fdopen(fd, "wb") as f:
        f.write(image)
    return patched


def check_timer_test(lines, exp):
    """The timer-test block must directly follow the timer line: IRQ_TICKS
    tick lines, the kernel PASS, then the BRK negative control ending on the
    terminal line. Every tick is re-judged independently."""
    tm = [i for i, ln in enumerate(lines) if TIMER_RE.fullmatch(ln)]
    if len(tm) != 1:
        fail(f"timer-test: expected exactly one timer line, found {len(tm)}")
    start = tm[0] + 1
    # (b) rotating-scratch x29/x30 proof prints its PASS line first, right after
    # the timer line and before the recorded tick evidence.
    if start >= len(lines) or lines[start] != IRQ_CANARY_HI:
        fail("timer-test: missing 'irq canary-hi PASS (x29,x30)' line directly "
             "after the timer line (the (b) rotating-scratch proof)")
    start += 1
    irq = [k for k, ln in enumerate(lines)
           if k >= start and ln.startswith(IRQ_TICK_PREFIX)]
    want = list(range(start, start + IRQ_TICKS + 1))
    if irq != want:
        fail(f"timer-test: expected {IRQ_TICKS} irq tick lines + PASS directly after "
             f"the timer line (lines {want}), found irq lines at {irq}")
    if any(DOUBLE_FAULT in ln for ln in lines):
        fail("timer-test: double-fault marker present (transient guard not cleared "
             "or a fault inside the IRQ path)")
    lo, hi = exp["lo"], exp["hi"]
    s_lo, s_hi = exp["stack"]
    prev = None
    sp0 = None
    for n, k in enumerate(want[:-1], start=1):
        m = IRQ_TICK_RE.fullmatch(lines[k])
        if m is None:
            fail(f"timer-test: malformed tick line {lines[k].decode('ascii', 'replace')!r}")
        tn, vec, intid, isr, spsr, elr, sp, pct, hb = (int(x, 16) for x in m.groups())
        where = f"tick {n}"
        if tn != n:
            fail(f"timer-test: {where} has n={tn} (want strictly 1..{IRQ_TICKS})")
        if vec != IRQ_VECTOR:
            fail(f"timer-test: {where} vector={vec}, want {IRQ_VECTOR} (EL1h IRQ); "
                 f"4 would be a synchronous exception misrouted")
        if intid != TIMER_INTID:
            fail(f"timer-test: {where} intid=0x{intid:x} != 0x{TIMER_INTID:x}")
        # ISR_EL1 bits other than A/I/F are RES0 here (no FEAT_NMI); a pending
        # SError/FIQ alongside the IRQ is unexpected, so pin exactly I.
        if isr != ISR_EL1_I:
            fail(f"timer-test: {where} isr=0x{isr:x}, want 0x{ISR_EL1_I:x} (only "
                 f"ISR_EL1.I: async IRQ pending at handler entry)")
        if spsr & 0x1F != SPSR_M_EL1H:
            fail(f"timer-test: {where} spsr=0x{spsr:x} M[4:0] != 0b00101 (EL1h)")
        # The interrupted context had I CLEAR (else the IRQ could not be taken)
        # and D/A/F still SET (only `daifclr #2` was issued). NZCV is free.
        if spsr & SPSR_I or (spsr & (SPSR_D | SPSR_A | SPSR_F)) != (SPSR_D | SPSR_A | SPSR_F):
            fail(f"timer-test: {where} spsr=0x{spsr:x}: want I=0 and D=A=F=1")
        if spsr & 0x0FFFFC20 or spsr >> 32:
            fail(f"timer-test: {where} spsr=0x{spsr:x} has unexpected bits set "
                 f"(IL/SS/PAN/BTYPE/high)")
        if not lo <= elr <= hi or elr & 3:
            fail(f"timer-test: {where} elr=0x{elr:x} outside the canary unmasked "
                 f"window [0x{lo:x}, 0x{hi:x}] (IRQ did not interrupt the loop)")
        if sp0 is None:
            sp0 = sp
            if sp & 0xF or not s_lo <= sp < s_hi:
                fail(f"timer-test: {where} sp=0x{sp:x} not 16-aligned inside "
                     f"[__stack_bottom 0x{s_lo:x}, __stack_top 0x{s_hi:x})")
        elif sp != sp0:
            fail(f"timer-test: {where} sp=0x{sp:x} != tick 1 sp=0x{sp0:x} "
                 f"(frame leak / SP not restored)")
        if prev is not None:
            p_pct, p_hb = prev
            if pct <= p_pct:
                fail(f"timer-test: {where} cntpct not strictly increasing")
            if pct - p_pct < VIRT82_INTERVAL:
                fail(f"timer-test: {where} cntpct delta {pct - p_pct} < interval "
                     f"{VIRT82_INTERVAL} (re-arm broken)")
            if hb <= p_hb:
                fail(f"timer-test: {where} heartbeat 0x{hb:x} <= previous 0x{p_hb:x} "
                     f"(interrupted loop did not resume: no eret return)")
        prev = (pct, hb)
    pm = IRQ_PASS_RE.fullmatch(lines[want[-1]])
    if pm is None or int(pm.group(1), 16) != IRQ_TICKS:
        fail(f"timer-test: PASS line missing/malformed or ticks != {IRQ_TICKS} "
             f"(a tick beyond the evidence cap is unrecorded)")
    # BRK negative control, contiguous, ending on the terminal line.
    tail = lines[want[-1] + 1:]
    if len(tail) != 5:
        fail(f"timer-test: expected exactly 5 BRK-control lines after PASS, got "
             f"{len(tail)}")
    if tail[0] != IRQ_BRK_TRIGGER or tail[1] != IRQ_BRK_EXC:
        fail("timer-test: BRK control trigger/EXCEPTION line mismatch (want vector 4, "
             "EC 0x3c, ISS 0xb2b2)")
    em = IRQ_BRK_ESR_RE.fullmatch(tail[2])
    if em is None:
        fail("timer-test: BRK control esr line != esr=0xf200b2b2 elr=<hex> spsr=<hex>")
    b_elr, b_spsr = int(em.group(1), 16), int(em.group(2), 16)
    if b_elr != exp["brk"]:
        fail(f"timer-test: BRK elr=0x{b_elr:x} != {IRQ_BRK_SYM.decode()} 0x{exp['brk']:x}")
    if b_spsr & 0x1F != SPSR_M_EL1H or (b_spsr >> 6) & 0xF != 0xF:
        fail(f"timer-test: BRK spsr=0x{b_spsr:x}: want EL1h with DAIF all masked "
             f"(I re-masked after the proof)")
    fm = IRQ_BRK_FAR_RE.fullmatch(tail[3])
    vb = [VBAR_RE.fullmatch(ln) for ln in lines if VBAR_RE.fullmatch(ln)]
    if fm is None or len(vb) != 1 or fm.group(1) != vb[0].group(1):
        fail("timer-test: BRK control far/vbar line missing or vbar != installed vbar")
    if tail[4] != TERM_EXC:
        fail("timer-test: BRK control did not end on 'exception handled; halted'")
    print(f"irq:   {IRQ_TICKS} returning IRQ ticks via vector 5: intid {TIMER_INTID}, "
          f"ISR_EL1.I set, SPSR EL1h I=0/DAF=1, ELR in canary window, sp=0x{sp0:x} "
          f"stable, cntpct deltas >= {VIRT82_INTERVAL} (TCG lower bound), heartbeat "
          f"strictly increasing; BRK control -> vector 4 EC 0x3c ELR==site, "
          f"no double fault")


# ---- --mmu-fault (MP2 PR-1b deliberate W^X / guard faults) ------------------
# name -> (target symbol, ESR EC, FSC, WnR, data-abort?, PAR_R spec, PAR_W spec,
#          section-range symbols the target must lie in)
# PAR spec: None = must translate (Normal WB, IS), int = must fault with FST.
MMU_FAULTS = {
    "write-to-text": (b"je_arm_mmu_fault_text_target", 0x25, FSC_PERMISSION_L3, 1,
                      None, FSC_PERMISSION_L3, (b"_text", b"__text_end")),
    "execute-from-data": (b"je_arm_mmu_fault_data_insn", 0x21, FSC_PERMISSION_L3, 0,
                          None, None, (b"__rodata_end", b"__data_end")),
    "execute-from-rodata": (b"je_arm_mmu_fault_rodata_insn", 0x21, FSC_PERMISSION_L3, 0,
                            None, FSC_PERMISSION_L3, (b"__text_end", b"__rodata_end")),
    "stack-guard": (b"__stack_guard", 0x25, FSC_TRANSLATION_L3, 1,
                    FSC_TRANSLATION_L3, FSC_TRANSLATION_L3, (b"__stack_guard", b"__stack_bottom")),
}
MMU_FAULT_PASS = {
    "write-to-text": b"JeiOS/aarch64: mmu fault self-test PASS write-to-text (DABT_CUR ec=0x25 dfsc=0x0f permission L3 WnR=1; FAR==.text target; ELR==store site)",
    "execute-from-data": b"JeiOS/aarch64: mmu fault self-test PASS execute-from-data (IABT_CUR ec=0x21 ifsc=0x0f permission L3; FAR==ELR==.data buffer proven RW)",
    "execute-from-rodata": b"JeiOS/aarch64: mmu fault self-test PASS execute-from-rodata (IABT_CUR ec=0x21 ifsc=0x0f permission L3; FAR==ELR==.rodata target)",
    "stack-guard": b"JeiOS/aarch64: mmu fault self-test PASS stack-guard (DABT_CUR ec=0x25 dfsc=0x07 translation L3 WnR=1; FAR==__stack_guard; ELR==store site)",
}
STORE_SITE_SYM = b"je_arm_mmu_fault_store_site"
MMU_FAULT_TEST_RE = re.compile(MMU_FAULT_PREFIX + rb"test ([a-z-]+) target=" + H
                               + rb" par_r=" + H + rb" par_w=" + H)
EXC_ESR_RE = re.compile(rb"JeiOS/aarch64:   esr=" + H + rb" elr=" + H + rb" spsr=" + H)
EXC_FAR_RE = re.compile(rb"JeiOS/aarch64:   far=" + H + rb" vbar=" + H)


def mmu_fault_expect(name, elf_path, image_path, syms):
    """Expected values for one fault mode, all from the ELF + architecture
    encodings, with the ELF bound to the booted Image by instruction words."""
    tsym, ec, fsc, wnr, _, _, (lo, hi) = MMU_FAULTS[name]
    fs = resolve_syms(elf_path, (tsym, STORE_SITE_SYM) if wnr else (tsym,),
                      f"--mmu-fault {name}")
    target = fs[tsym]
    if not syms[lo] <= target < syms[hi]:
        fail(f"--mmu-fault {name}: target 0x{target:x} not inside "
             f"[{lo.decode()}=0x{syms[lo]:x},{hi.decode()}=0x{syms[hi]:x})")
    with open(image_path, "rb") as f:
        image = f.read()
    text = syms[b"_text"]
    if name != "stack-guard":
        w = image_word(image, text, target, f"--mmu-fault {name}")
        if w != A64_RET:
            fail(f"--mmu-fault {name}: Image word at target is 0x{w:08x}, not RET; "
                 f"ELF/Image mismatch")
    if wnr:
        elr = fs[STORE_SITE_SYM]
        w = image_word(image, text, elr, f"--mmu-fault {name}")
        if w & A64_STR_XZR_MASK != A64_STR_XZR_BITS:
            fail(f"--mmu-fault {name}: Image word at store site is 0x{w:08x}, not "
                 f"STR XZR,[Xn]; ELF/Image mismatch")
    else:
        elr = target          # instruction abort: the faulting fetch IS the target
    # ISS = FSC | WnR<<6; ISV/SAS/SRT (ISS[24:14]) are 0 for an EL1-taken abort
    # on QEMU 8.2 (pinned); CM/S1PTW/EA/FnV must be 0. IL=1 (32-bit insn).
    iss = fsc | (wnr << 6)
    esr = (ec << 26) | (1 << 25) | iss
    return target, elr, esr, iss, ec


def mmu_fault_markers(name, exp):
    target, elr, esr, iss, ec = exp
    return [
        valued(MMU_FAULT_PREFIX + b"test " + name.encode() + b" target=<hex> par_r=<hex> par_w=<hex>",
               re.escape(MMU_FAULT_PREFIX + b"test " + name.encode() + b" target=" + hx(target))
               + rb" par_r=" + HEX16 + rb" par_w=" + HEX16),
        exact(b"JeiOS/aarch64: EXCEPTION vector=0x0000000000000004 ec=" + hx(ec)
              + b" il=0x0000000000000001 iss=" + hx(iss)),
        valued(b"JeiOS/aarch64:   esr=<exact> elr=<exact> spsr=<hex>",
               re.escape(b"JeiOS/aarch64:   esr=" + hx(esr) + b" elr=" + hx(elr)
                         + b" spsr=") + HEX16),
        valued(b"JeiOS/aarch64:   far=<exact> vbar=<hex>",
               re.escape(b"JeiOS/aarch64:   far=" + hx(target) + b" vbar=") + HEX16),
        exact(MMU_FAULT_PASS[name]),
        exact(b"JeiOS/aarch64: exception handled; halted"),
    ]


def check_mmu_fault(lines, name, exp, syms):
    """Re-decode the raw syndrome lines field by field (not only the in-kernel
    PASS), and the pre-access AT probes, so a bug in the kernel's comparator or
    a wrong mapping class cannot yield a false PASS."""
    target, elr, esr, _, ec = exp
    _, _, fsc, wnr, par_r_spec, par_w_spec, _ = MMU_FAULTS[name]
    t = [MMU_FAULT_TEST_RE.fullmatch(ln) for ln in lines]
    t = [g for g in t if g]
    if len(t) != 1 or t[0].group(1).decode() != name:
        fail(f"--mmu-fault {name}: expected exactly one 'mmu fault test {name}' line")
    tgt, par_r, par_w = (int(x, 16) for x in t[0].groups()[1:])
    ti = next(i for i, ln in enumerate(lines) if MMU_FAULT_TEST_RE.fullmatch(ln))
    if TIMER_RE.fullmatch(lines[ti - 1]) is None:
        fail(f"--mmu-fault {name}: fault test line must directly follow the "
             f"MMU + GIC/timer block")
    if tgt != target:
        fail(f"--mmu-fault {name}: target=0x{tgt:x} != ELF 0x{target:x}")
    for par, spec, what in ((par_r, par_r_spec, "par_r (AT S1E1R)"),
                            (par_w, par_w_spec, "par_w (AT S1E1W)")):
        if spec is None:
            par_expect_ok(par, target, MAIR_NORMAL_WB, 0b11, f"--mmu-fault {name} {what}")
        else:
            par_expect_fault(par, spec, f"--mmu-fault {name} {what}")
    e = [EXC_ESR_RE.fullmatch(ln) for ln in lines]
    e = [g for g in e if g]
    f = [EXC_FAR_RE.fullmatch(ln) for ln in lines]
    f = [g for g in f if g]
    if len(e) != 1 or len(f) != 1:
        fail(f"--mmu-fault {name}: expected exactly one esr and one far line "
             f"(a second exception means the handler itself faulted)")
    r_esr, r_elr, r_spsr = (int(x, 16) for x in e[0].groups())
    r_far, r_vbar = (int(x, 16) for x in f[0].groups())
    iss = r_esr & 0x1FFFFFF
    got = {"EC": r_esr >> 26 & 0x3F, "IL": r_esr >> 25 & 1, "FSC": iss & 0x3F,
           "WnR": iss >> 6 & 1, "S1PTW": iss >> 7 & 1, "CM": iss >> 8 & 1,
           "EA": iss >> 9 & 1, "FnV": iss >> 10 & 1}
    want = {"EC": ec, "IL": 1, "FSC": fsc, "WnR": wnr, "S1PTW": 0, "CM": 0,
            "EA": 0, "FnV": 0}
    if got != want or r_esr != esr:
        fail(f"--mmu-fault {name}: ESR=0x{r_esr:x} fields {got} != {want}")
    if r_far != target or r_elr != elr:
        fail(f"--mmu-fault {name}: FAR=0x{r_far:x} ELR=0x{r_elr:x}, want "
             f"FAR=0x{target:x} ELR=0x{elr:x}")
    if r_spsr & 0x1F != 0x5 or (r_spsr >> 6) & 0xF != 0xF:
        fail(f"--mmu-fault {name}: SPSR=0x{r_spsr:x} not EL1h with DAIF masked")
    if r_vbar != syms[b"je_arm_vectors"]:
        fail(f"--mmu-fault {name}: VBAR=0x{r_vbar:x} != ELF je_arm_vectors")
    print(f"fault: {name} ESR=0x{r_esr:08x} (EC=0x{ec:02x} FSC=0x{fsc:02x} WnR={wnr}) "
          f"FAR=0x{r_far:x} ELR=0x{r_elr:x} par_r=0x{par_r:016x} par_w=0x{par_w:016x}")


def one(lines, rx, what):
    found = [m for m in map(rx.fullmatch, lines) if m]
    if len(found) != 1:
        fail(f"--phys-alloc: expected exactly one {what} line, found {len(found)}")
    return found[0]


def resolve_image_extent(elf_path):
    """[_text, _end) from the ELF symtab (the kernel_image fact main.c uses)."""
    try:
        if os.path.getsize(elf_path) > ELF_MAX:
            fail(f"--elf {elf_path} larger than {ELF_MAX} bytes")
        with open(elf_path, "rb") as f:
            data = f.read(ELF_MAX + 1)
        syms = elf_defined_symbols(data, (b"_text", b"_end"))
    except (OSError, ElfError) as e:
        fail(f"--phys-alloc: cannot resolve _text/_end from --elf {elf_path}: {e}")
    if len(syms[b"_text"]) != 1 or len(syms[b"_end"]) != 1:
        fail(f"--phys-alloc: need exactly one defined _text and _end in {elf_path}")
    text, end = syms[b"_text"][0][0], syms[b"_end"][0][0]
    if end <= text:
        fail(f"--phys-alloc: ELF _end=0x{end:x} <= _text=0x{text:x}")
    return text, end


def physmem_free_pages(window, ram, reserved):
    """From-first-principles free set: page p in [base, limit) step 4096 is free
    iff [p, p+4096) is a subset of SOME ram range AND intersects NO reserved
    range (all half-open). Returns the sorted list of free page addresses."""
    base, limit = window
    free = []
    for p in range(base, limit, PHYS_PAGE):
        e = p + PHYS_PAGE
        if not any(rb <= p and e <= rb + rs for rb, rs in ram):
            continue
        if any(p < qb + qs and qb < e for qb, qs in reserved):
            continue
        free.append(p)
    return free


def check_phys_alloc(lines, fx, image_extent):
    """The --phys-alloc block grammar, cross-bindings and independent oracle.
    Calls fail() on any mismatch; returns the oracle's free-page count."""
    # Grammar: all physmem lines form ONE contiguous block right after the
    # "validated" line and right before the terminal line, in main.c's order:
    # window, kernel, dtb, [initrd], ram+, rsv*, free, alloc1, free1, allocmax,
    # overcap. Any unknown physmem line is a FAIL. PR-1b then emits the
    # contiguous MMU marker block, and PR-2a the GIC/timer block (checked by
    # check_gic up to the terminal line).
    idx = [i for i, ln in enumerate(lines) if ln.startswith(PM)]
    if not idx or idx != list(range(idx[0], idx[0] + len(idx))):
        fail("--phys-alloc: physmem lines missing or not one contiguous block")
    if VALIDATED_RE.fullmatch(lines[idx[0] - 1]) is None:
        fail("--phys-alloc: physmem block does not directly follow 'validated'")
    after = idx[-1] + 1
    while after < len(lines) and lines[after].startswith(MMU):
        after += 1
    if after >= len(lines) or GIC_INIT_RE.fullmatch(lines[after]) is None:
        fail("--phys-alloc: physmem block not followed by the MMU block and the "
             "GIC/timer block")
    block = [lines[i] for i in idx]
    pos = 0

    def take(rx, what, optional=False):
        nonlocal pos
        m = rx.fullmatch(block[pos]) if pos < len(block) else None
        if m is None:
            if optional:
                return None
            got = block[pos].decode("ascii", "replace") if pos < len(block) else "<end>"
            fail(f"--phys-alloc: expected {what} at block line {pos}, got {got!r}")
        pos += 1
        return m

    def ints(m, *groups):
        return tuple(int(m.group(g), 16) for g in groups)

    window = ints(take(PM_WINDOW_RE, "window"), 1, 2)
    reserves = {}
    for kind in (b"kernel", b"dtb", b"initrd"):
        m = take(PM_RESERVE_RE, f"reserve kind={kind.decode()}", optional=kind == b"initrd")
        if m is None:
            continue
        if m.group(1) != kind:
            fail(f"--phys-alloc: reserve kind={m.group(1).decode()} where "
                 f"kind={kind.decode()} expected")
        reserves[kind] = ints(m, 2, 3)
    ram = []
    while (m := take(PM_RAM_RE, "ram", optional=bool(ram))) is not None:
        ram.append(ints(m, 1, 2))
    rsv = []
    while (m := take(PM_RSV_RE, "rsv", optional=True)) is not None:
        rsv.append(ints(m, 1, 2))
    unmanaged, = ints(take(PM_UNMANAGED_RE, "unmanaged"), 1)
    free_pages, free_bytes = ints(take(PM_FREE_RE, "free"), 1, 2)
    alloc1, = ints(take(PM_ALLOC1_RE, "alloc1"), 1)
    if pos >= len(block) or block[pos] != PM_FREE1:
        fail("--phys-alloc: expected 'physmem free1 ok' after alloc1")
    pos += 1
    allocmax, = ints(take(PM_ALLOCMAX_RE, "allocmax"), 1)
    overcap, = ints(take(PM_OVERCAP_RE, "overcap"), 1)
    if pos != len(block):
        fail(f"--phys-alloc: unexpected trailing physmem line "
             f"{block[pos].decode('ascii', 'replace')!r}")

    # Cross-bindings: every oracle input must equal a source independent of
    # the physmem code path.
    if window != PHYS_WINDOW:
        fail(f"--phys-alloc: window {window} != pinned {PHYS_WINDOW}")
    ktext, kend = image_extent
    if reserves[b"kernel"] != (ktext, kend - ktext):
        fail(f"--phys-alloc: kernel reserve {reserves[b'kernel']} != ELF "
             f"[_text=0x{ktext:x}, _end=0x{kend:x})")
    dtb_ptr, = ints(one(lines, DTB_PTR_RE, "'dtb pointer recorded'"), 1)
    if reserves[b"dtb"] != (dtb_ptr, fx.totalsize):
        fail(f"--phys-alloc: dtb reserve {reserves[b'dtb']} != (dtb pointer "
             f"0x{dtb_ptr:x}, fixture totalsize 0x{fx.totalsize:x})")
    parsed = one(lines, DTB_PARSED_RE, "'dtb parsed'")
    n_ram, n_rsv, n_init = int(parsed.group(1)), int(parsed.group(2)), int(parsed.group(5))
    dtb_ram = [(int(m.group(2), 16), int(m.group(3), 16))
               for m in map(DTB_RAM_RE.fullmatch, lines) if m]
    if ram != dtb_ram or len(ram) != n_ram:
        fail(f"--phys-alloc: physmem ram {ram} != dtb parsed ram {dtb_ram} (n={n_ram})")
    if len(rsv) != n_rsv or (b"initrd" in reserves) != bool(n_init):
        fail(f"--phys-alloc: rsv/initrd lines ({len(rsv)}, "
             f"{b'initrd' in reserves}) != dtb parsed (rsv={n_rsv}, initrd={n_init})")

    reserved = list(reserves.values()) + rsv
    free = physmem_free_pages(PHYS_WINDOW, ram, reserved)
    want = len(free)
    print(f"physmem oracle: window=[0x{PHYS_WINDOW[0]:x},0x{PHYS_WINDOW[1]:x}) "
          f"ram={[(hex(b), hex(z)) for b, z in ram]} "
          f"reserved={[(hex(b), hex(z)) for b, z in reserved]} -> "
          f"computed free pages={want} (0x{want:x}); kernel says 0x{free_pages:x}")
    if free_pages != want:
        fail(f"--phys-alloc: kernel free pages=0x{free_pages:x} != oracle 0x{want:x}")
    if free_bytes != want * PHYS_PAGE:
        fail(f"--phys-alloc: free bytes=0x{free_bytes:x} != pages*4096")
    # Independent recompute of the degraded-profile "unmanaged RAM" figure: RAM
    # bytes lying outside the managed window. Derived from the parsed ram ranges
    # and the pinned window, not from the kernel's own number.
    wbase, wlimit = PHYS_WINDOW
    want_unmanaged = 0
    for rb, rs in ram:
        re_ = rb + rs
        if rb < wbase:
            want_unmanaged += min(re_, wbase) - rb
        if re_ > wlimit:
            want_unmanaged += re_ - max(rb, wlimit)
    if unmanaged != want_unmanaged:
        fail(f"--phys-alloc: kernel unmanaged bytes=0x{unmanaged:x} != "
             f"oracle 0x{want_unmanaged:x}")
    free_set = set(free)
    # Hard property: what the allocator hands out is genuinely free. The
    # equality to the lowest free page / first free 256-run additionally pins
    # the allocator's first-fit scan (tested contract, tests/test_alloc.c).
    if alloc1 not in free_set:
        fail(f"--phys-alloc: alloc1 pa=0x{alloc1:x} is NOT an oracle-free page")
    if alloc1 != free[0]:
        fail(f"--phys-alloc: alloc1 pa=0x{alloc1:x} != lowest free 0x{free[0]:x}")
    run = [allocmax + i * PHYS_PAGE for i in range(PHYS_ALLOC_MAX_PAGES)]
    if not all(p in free_set for p in run):
        fail(f"--phys-alloc: allocmax run at 0x{allocmax:x} covers a non-free page")
    first_run = next((p for p in free if all(p + i * PHYS_PAGE in free_set
                                             for i in range(PHYS_ALLOC_MAX_PAGES))), None)
    if allocmax != first_run:
        fail(f"--phys-alloc: allocmax pa=0x{allocmax:x} != first free "
             f"{PHYS_ALLOC_MAX_PAGES}-page run {first_run}")
    if overcap != want:
        fail(f"--phys-alloc: overcap pages=0x{overcap:x} != oracle 0x{want:x} "
             f"(allocations were not fully rolled back)")
    return want


def timeout_arg(text):
    """argparse type: a finite number of seconds > 0 (nan/inf/0/negative are
    parse errors — a non-finite deadline would never expire)."""
    try:
        v = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number: {text!r}")
    if not math.isfinite(v) or v <= 0:
        raise argparse.ArgumentTypeError(f"must be finite and > 0, got {text!r}")
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--timeout", type=timeout_arg, default=15.0,
                    help="bound in seconds (finite, > 0) on the whole QEMU run "
                         "INCLUDING the %ss settle window; final marker not seen, "
                         "or seen with < %ss left, is a FAIL" % (SETTLE_S, SETTLE_S))
    ap.add_argument("--expect-pa-bits", type=int, default=44, metavar="N",
                    help="exact pa_bits the boot must report (pinned virt-8.2 "
                         "cortex-a72 profile: 44)")
    ap.add_argument("--qemu", default=QEMU, metavar="PATH",
                    help="QEMU binary (test hook, e.g. a shim that exits early "
                         "to prove self-exit detection)")
    ap.add_argument("--bss-verified", action="store_true",
                    help="expect the patterned-BSS 'bss clear verified' marker")
    ap.add_argument("--vector-test", action="store_true",
                    help="expect the deliberate-BRK vector self-test markers")
    ap.add_argument("--dtb-bad", action="store_true",
                    help="expect the JE_ARM_DTB_BAD parser rejection (BAD_TOKEN)")
    ap.add_argument("--phys-alloc", action="store_true",
                    help="assert the physmem bring-up block and recompute its "
                         "free-page count with an independent oracle")
    ap.add_argument("--mmu-fault", choices=sorted(MMU_FAULTS), metavar="NAME",
                    help="expect the ARM_MMU_FAULT deliberate fault: "
                         + ", ".join(sorted(MMU_FAULTS)))
    ap.add_argument("--timer-poll", action="store_true",
                    help="expect the ARM_TIMER_POLL polled GIC/timer proof "
                         "(4 ACK/EOI cycles, IRQs masked)")
    ap.add_argument("--timer-test", action="store_true",
                    help="expect the ARM_TIMER_TEST returning-IRQ proof (4 ticks "
                         "via vector 5 interrupting the register-canary loop, "
                         "then the BRK #0xb2b2 negative control)")
    ap.add_argument("--late-corruption", choices=("low", "high"),
                    help="deliberately patch a timer-test Image copy at the "
                         "late low/high canary site; this profile must FAIL "
                         "with the diagnosed register marker")
    ap.add_argument("--stalled-timer", action="store_true",
                    help="deliberately suppress initial CNTP arming in an Image "
                         "copy; this profile must FAIL with the internal "
                         "timer-stalled marker")
    ap.add_argument("--require", action="store_true",
                    help="missing QEMU/toolchain/image is a FAIL, not a SKIP")
    ap.add_argument("--elf", metavar="PATH",
                    help="ELF the Image was built from (default: the Image's "
                         "sibling jeios-arm.elf); --vector-test resolves "
                         "je_arm_brk_site from it and requires ELR to equal it")
    ap.add_argument("--log", metavar="PATH",
                    help="write the raw serial log here (kept after the run)")
    args = ap.parse_args()

    def skip(msg):
        if args.require:
            fail(f"--require: {msg}")
        print(f"QEMU aarch64 smoke: SKIP ({msg})")
        sys.exit(0)

    if args.dtb_bad and args.vector_test:
        fail("--dtb-bad and --vector-test are mutually exclusive (the DTB "
             "rejection halts before the BRK)")
    if args.phys_alloc and (args.dtb_bad or args.vector_test):
        fail("--phys-alloc is only valid with the default good-boot build")
    if args.mmu_fault and (args.dtb_bad or args.vector_test or args.phys_alloc
                           or args.bss_verified):
        fail("--mmu-fault is only valid alone (its build excludes the other toggles)")
    if args.timer_poll and (args.dtb_bad or args.vector_test or args.mmu_fault
                            or args.bss_verified):
        fail("--timer-poll is only valid with the default good-boot build "
             "(optionally --phys-alloc)")
    if args.timer_test and (args.dtb_bad or args.vector_test or args.mmu_fault
                            or args.bss_verified or args.timer_poll
                            or args.phys_alloc):
        fail("--timer-test is only valid alone (its build excludes the other "
             "toggles)")
    if (args.late_corruption is not None or args.stalled_timer) and not args.timer_test:
        fail("--late-corruption/--stalled-timer require --timer-test")
    if args.late_corruption is not None and args.stalled_timer:
        fail("--late-corruption and --stalled-timer are mutually exclusive")
    qemu = args.qemu
    if shutil.which(qemu) is None:
        skip(f"{qemu} not installed")
    if shutil.which(TOOLCHAIN_CC) is None:
        skip(f"{TOOLCHAIN_CC} not installed")
    # The fixture is required in every mode (it is committed; a missing or
    # unparseable one is a repository defect, never a SKIP).
    try:
        fx = Fixture(FIXTURE)
    except (OSError, ValueError) as e:
        fail(f"{'--require: ' if args.require else ''}cannot read DTB fixture: {e}")
    qv = qemu_version(qemu)
    if qv is None or qv[:2] != QEMU_REQUIRED_VERSION:
        want = "%d.%d.x" % QEMU_REQUIRED_VERSION
        got = "unparseable" if qv is None else "%d.%d.%d" % qv
        if args.require:
            fail(f"--require: {qemu} version {got}, need {want} (the DTB "
                 f"fixture was dumped by QEMU {want})")
        print(f"WARNING: {qemu} version {got}, fixture pinned to {want}; the "
              f"DTB CRC check may fail")
    if not os.path.exists(args.image):
        skip(f"image {args.image} not built")
    elf = args.elf or os.path.join(os.path.dirname(args.image), "jeios-arm.elf")
    # The ELF is the only source of the expected ELR, so under --vector-test it
    # is mandatory (FAIL, never SKIP); --require demands it in every mode.
    # Every good-boot mode runs the MMU oracle, which needs the ELF: missing is
    # a FAIL (never a SKIP) outside --dtb-bad.
    if not os.path.isfile(elf) and (not args.dtb_bad or args.require):
        fail(f"{'--require' if args.dtb_bad else 'MMU oracle'}: ELF {elf} not found")
    boot_image = make_timer_negative_image(
        elf, args.image, args.late_corruption, args.stalled_timer)
    image_extent = resolve_image_extent(elf) if args.phys_alloc else None
    mmu_syms = None if args.dtb_bad else resolve_syms(elf, MMU_LAYOUT_SYMS, "mmu")
    fault_exp = (mmu_fault_expect(args.mmu_fault, elf, args.image, mmu_syms)
                 if args.mmu_fault else None)
    tt_exp = timer_test_expect(elf, boot_image) if args.timer_test else None

    markers = list(BASE_MARKERS)
    markers[3] = exact(b"JeiOS/aarch64: bss clear verified" if args.bss_verified
                       else b"JeiOS/aarch64: bss cleared")
    bad_line = (b"JeiOS/aarch64: FATAL dtb rejected status=BAD_TOKEN off="
                + hx(fx.off_dt_struct))
    if args.dtb_bad:
        markers += [
            COPIED,
            exact(b"JeiOS/aarch64: dtb-bad test: wrote token 0x0000000a at off="
                  + hx(fx.off_dt_struct)),
            exact(bad_line),
        ]
    else:
        markers += dtb_good_markers()
        if args.phys_alloc:
            markers += phys_alloc_markers()
        markers += mmu_markers()
        markers += gic_markers()
        if args.timer_poll:
            markers += timer_poll_markers()
        if args.vector_test:
            markers += vector_test_markers(resolve_brk_site(elf, args.image))
        elif args.mmu_fault:
            markers += mmu_fault_markers(args.mmu_fault, fault_exp)
        elif args.timer_test:
            markers += [exact(IRQ_CANARY_HI)]
            markers += [valued(b"JeiOS/aarch64: irq tick n=" + hx(n) + b" ...",
                               re.escape(b"JeiOS/aarch64: irq tick n=" + hx(n) + b" ")
                               + rb".*")
                        for n in range(1, IRQ_TICKS + 1)]
            markers += [valued(b"JeiOS/aarch64: irq timer PASS ticks=<hex>",
                               IRQ_PASS_RE.pattern),
                        exact(IRQ_BRK_TRIGGER), exact(IRQ_BRK_EXC), exact(TERM_EXC)]
        else:
            markers += [exact(TERM_DEFAULT)]
    # Every mode ends on a fixed (exact) line; that line is the terminal marker.
    final_line = markers[-1][0]
    # Stop early (instead of burning the whole timeout) once the run can no
    # longer PASS: outside --dtb-bad any FATAL means the kernel halted on an
    # error path; in --dtb-bad the forbidden "validated" marker means the
    # injection was not rejected. The checks below then fail with a clear
    # message; this only shortens the wait, it never makes a run PASS.
    early_stop = VALIDATED if args.dtb_bad else b"FATAL"
    if not args.dtb_bad:
        check_dtb_gic_timer(fx)   # static: pinned bases/INTID vs fixture DTB

    # Evidence: image hash + tool versions.
    with open(boot_image, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    print(f"image: {boot_image}  sha256={sha}")
    print(f"qemu:  {tool_version(qemu)}")
    print(f"cc:    {tool_version(TOOLCHAIN_CC)}")
    print(f"fixture: {os.path.normpath(FIXTURE)} crc32=0x{fx.crc32:08x} "
          f"len={fx.length} off_dt_struct=0x{fx.off_dt_struct:x}")

    if args.log:
        serial_path = args.log
        os.makedirs(os.path.dirname(os.path.abspath(serial_path)), exist_ok=True)
        if os.path.exists(serial_path):
            os.unlink(serial_path)  # never assert on a previous run's output
    else:
        with tempfile.NamedTemporaryFile(prefix="jeios-arm-serial-", suffix=".log",
                                         delete=False) as tf:
            serial_path = tf.name

    cmd = [qemu, "-M", MACHINE, "-cpu", CPU, "-smp", "1", "-m", MEMORY,
           "-display", "none", "-serial", f"file:{serial_path}",
           "-no-reboot", "-kernel", boot_image]
    print("cmd:   " + " ".join(cmd))

    try:
        out, early, err = run_qemu(cmd, serial_path, final_line, early_stop,
                                   args.timeout)
    finally:
        if boot_image != args.image:
            try:
                os.unlink(boot_image)
            except OSError:
                pass
        if args.log:
            print(f"log:   {serial_path}")
        else:
            try:
                os.unlink(serial_path)
            except OSError:
                pass

    sys.stdout.write(out.decode("ascii", "replace"))
    sys.stdout.write("\n")

    # Everything below judges the FINAL log (re-read after QEMU was killed)
    # and the final stderr. stderr first: a QEMU-level error voids the run.
    check_stderr(err)
    if args.late_corruption is not None or args.stalled_timer:
        lines, partial = split_lines(out)
        if partial:
            fail("negative timer profile emitted an unterminated serial line")
        if args.stalled_timer:
            if TIMER_STALLED not in lines:
                fail("stalled-timer profile did not emit the internal timer "
                     "stalled marker")
            fail("stalled-timer profile caught the fault with the internal "
                 "timer-stalled marker")
        want_reg = 0 if args.late_corruption == "low" else 29
        reg = str(want_reg).encode("ascii")
        want = (
            b"JeiOS/aarch64: FATAL irq canary mismatch reg=" + reg,
            b"JeiOS/aarch64: FATAL irq canary-hi mismatch reg=" + reg,
        )
        if not any(marker in lines for marker in want):
            fail(f"late-{args.late_corruption} profile did not emit the "
                 f"expected register marker reg={want_reg}")
        fail(f"late-{args.late_corruption} corruption caught at "
             f"reg={want_reg}")
    if args.dtb_bad:
        # Exactly one FATAL, and it must be the expected parser rejection.
        # Checked over the raw log so a FATAL in a partial tail is also seen.
        fatal_lines = [line_of(ln) for ln in out.split(b"\n") if b"FATAL" in ln]
        if fatal_lines != [bad_line]:
            fail(f"--dtb-bad: expected exactly the BAD_TOKEN FATAL line, got "
                 f"{[ln.decode('ascii', 'replace') for ln in fatal_lines]}")
        for forbidden in (b"JeiOS/aarch64: dtb parsed", VALIDATED, TERM_DEFAULT):
            if forbidden in out:
                fail(f"--dtb-bad: forbidden marker present: {forbidden.decode()}")
    elif b"FATAL" in out:
        fail("FATAL marker present")

    check_lines(out, final_line)
    lines, _ = split_lines(out)  # check_lines rejected any partial tail
    check_ordered(lines, markers)
    if not args.dtb_bad:
        pa = [m.group(1) for m in map(VALIDATED_RE.fullmatch, lines) if m]
        if len(pa) != 1:
            fail(f"expected exactly one 'boot context validated pa_bits=N' line, "
                 f"found {len(pa)}")
        if int(pa[0]) != args.expect_pa_bits:
            fail(f"pa_bits={int(pa[0])} != expected {args.expect_pa_bits}")
        print(f"pa:    pa_bits={int(pa[0])} == expected")
    # In every mode the copy precedes the parse; its CRC is over the copy taken
    # BEFORE the dtb-bad injection, so it must match the fixture in all modes.
    check_dtb_copied(lines, fx)
    if args.phys_alloc:
        check_phys_alloc(lines, fx, image_extent)
    if not args.dtb_bad:
        check_mmu(lines, mmu_syms)
        if args.vector_test:
            next_ok = lambda ln: ln == vector_test_markers(0)[0][0]
        elif args.mmu_fault:
            next_ok = lambda ln: MMU_FAULT_TEST_RE.fullmatch(ln) is not None
        elif args.timer_test:
            next_ok = lambda ln: ln == IRQ_CANARY_HI
        else:
            next_ok = lambda ln: ln == TERM_DEFAULT
        check_gic(lines, args.timer_poll, next_ok)
        if args.timer_test:
            check_timer_test(lines, tt_exp)
        elif any(ln.startswith(IRQ_TICK_PREFIX) or DOUBLE_FAULT in ln for ln in lines):
            fail("irq/double-fault lines present without --timer-test (wrong build?)")
    if args.mmu_fault:
        check_mmu_fault(lines, args.mmu_fault, fault_exp, mmu_syms)

    if early:
        # Defensive: an early stop means a FATAL / forbidden marker was seen,
        # which the checks above must already have rejected.
        fail("early-stop marker seen but validation did not fail (harness bug)")
    print("QEMU aarch64 smoke: PASS")


if __name__ == "__main__":
    main()
