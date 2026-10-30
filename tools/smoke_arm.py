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
        body = src.split("{", 1)[1]
        hdr = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)[:40])
        if len(hdr) != 40:
            raise ValueError(f"fixture {path}: fewer than 40 header bytes")
        self.off_dt_struct = int.from_bytes(hdr[8:12], "big")


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
    if not os.path.isfile(elf) and (args.vector_test or args.require):
        fail(f"{'--vector-test' if args.vector_test else '--require'}: "
             f"ELF {elf} not found")

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
        if args.vector_test:
            markers += vector_test_markers(resolve_brk_site(elf, args.image))
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

    # Evidence: image hash + tool versions.
    with open(args.image, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    print(f"image: {args.image}  sha256={sha}")
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
           "-no-reboot", "-kernel", args.image]
    print("cmd:   " + " ".join(cmd))

    try:
        out, early, err = run_qemu(cmd, serial_path, final_line, early_stop,
                                   args.timeout)
    finally:
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

    if early:
        # Defensive: an early stop means a FATAL / forbidden marker was seen,
        # which the checks above must already have rejected.
        fail("early-stop marker seen but validation did not fail (harness bug)")
    print("QEMU aarch64 smoke: PASS")


if __name__ == "__main__":
    main()
