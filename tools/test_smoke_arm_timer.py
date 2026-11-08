#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Mutation self-test for the tools/smoke_arm.py --timer-test oracle (MP2 PR-2b,
ADR 0012 D8).

The oracle must REJECT doctored serial logs, not merely accept a good one. This
replays a known-good ARM_TIMER_TEST serial log (captured by a real
`make smoke-aarch64 ARM_TIMER_TEST=1` run) through the unmodified smoke_arm.py
main() using a fake QEMU shim (`--qemu`), once unmodified (must PASS) and once
per mutation (each must FAIL with the expected reason substring).

The ELF/Image are the real ARM_TIMER_TEST build, so ELR-window/BRK-site/stack
checks use real Image-derived values. This proves the ORACLE; it does not boot
anything and is no substitute for the QEMU run.

Usage: python3 tools/test_smoke_arm_timer.py [BUILD_DIR]
  BUILD_DIR defaults to build/aarch64-timertest and must contain Image,
  jeios-arm.elf and serial.log from a PASSING smoke run.
Exit 0 iff every check passes. QEMU is NOT needed (the shim answers --version
and replays the log); aarch64-linux-gnu-gcc must be on PATH because
smoke_arm.py --require FAILs without it.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SMOKE = ROOT / "tools" / "smoke_arm.py"

SHIM = r'''#!/usr/bin/env python3
import sys, time
if "--version" in sys.argv:
    print("QEMU emulator version 8.2.2 (fake replay shim)")
    sys.exit(0)
serial = sys.argv[sys.argv.index("-serial") + 1]
assert serial.startswith("file:")
with open(%r, "rb") as src, open(serial[5:], "wb") as dst:
    dst.write(src.read())
time.sleep(60)
'''

def field(log, n, name, value):
    """Replace field `name` on tick line n with 16-digit hex `value`."""
    def sub(m):
        return re.sub(rb" " + name + rb"=0x[0-9a-f]{16}",
                      b" " + name + b"=0x%016x" % value, m.group(1))
    out, k = re.subn(re.compile(rb"^(JeiOS/aarch64: irq tick n=0x0*%d .*)$" % n,
                                re.M), sub, log)
    assert k == 1, (n, name)
    return out


def get(log, n, name):
    m = re.search(rb"^JeiOS/aarch64: irq tick n=0x0*%d .* " % n + name
                  + rb"=0x([0-9a-f]{16})", log, re.M)
    assert m, (n, name)
    return int(m.group(1), 16)


def mutations(log):
    lines = log.split(b"\n")

    def insert_after(prefix, new):
        i = next(k for k, ln in enumerate(lines) if ln.startswith(prefix))
        return b"\n".join(lines[:i + 1] + [new] + lines[i + 1:])

    def drop(prefix):
        return b"\n".join(ln for ln in lines if not ln.startswith(prefix))

    hb2 = get(log, 2, b"hb")
    pct2 = get(log, 2, b"cntpct")
    sp1 = get(log, 1, b"sp")
    spsr1 = get(log, 1, b"spsr")
    yield "frozen heartbeat", field(log, 3, b"hb", hb2), "heartbeat"
    yield "heartbeat decreases", field(log, 3, b"hb", hb2 - 1), "heartbeat"
    yield "vector=4 tick", field(log, 2, b"vector", 4), "vector=4"
    yield "varying sp", field(log, 3, b"sp", sp1 - 16), "sp="
    yield "sp outside stack", field(field(field(field(log, 1, b"sp", 0x1000),
                                                2, b"sp", 0x1000), 3, b"sp", 0x1000),
                                    4, b"sp", 0x1000), "not 16-aligned inside"
    yield "double fault line", insert_after(b"JeiOS/aarch64: irq timer PASS",
                                            b"JeiOS/aarch64: FATAL double fault; halted\r"), \
        "FATAL"
    yield "double fault text (no FATAL)", insert_after(
        b"JeiOS/aarch64: irq timer PASS", b"JeiOS/aarch64: double fault seen\r"), \
        "double-fault"
    yield "elr outside window", field(log, 2, b"elr", 0x40080000), "canary unmasked window"
    yield "elr misaligned", field(log, 2, b"elr", get(log, 2, b"elr") + 2), \
        "canary unmasked window"
    yield "isr I clear", field(log, 1, b"isr", 0), "isr="
    yield "isr extra F", field(log, 1, b"isr", 0xC0), "isr="
    yield "intid 31", field(log, 4, b"intid", 0x1F), "intid="
    yield "spsr EL1t", field(log, 1, b"spsr", (spsr1 & ~0x1F) | 0x4), "EL1h"
    yield "spsr I masked", field(log, 1, b"spsr", spsr1 | 0x80), "I=0"
    yield "spsr A unmasked", field(log, 1, b"spsr", spsr1 & ~0x100), "I=0"
    yield "cntpct not monotonic", field(log, 3, b"cntpct", pct2), "cntpct"
    yield "cntpct delta < interval", field(log, 3, b"cntpct", pct2 + 624999), "delta"
    yield "tick n wrong", field(log, 3, b"n", 7), "missing/out-of-order"
    yield "only 3 ticks", drop(b"JeiOS/aarch64: irq tick n=0x0000000000000004"), \
        "missing/out-of-order"
    yield "PASS ticks=5", log.replace(b"irq timer PASS ticks=0x0000000000000004",
                                      b"irq timer PASS ticks=0x0000000000000005"), \
        "ticks != 4"
    yield "BRK exc vector 5", log.replace(
        b"EXCEPTION vector=0x0000000000000004 ec=0x000000000000003c",
        b"EXCEPTION vector=0x0000000000000005 ec=0x000000000000003c"), \
        "missing/out-of-order"
    yield "BRK elr wrong", re.sub(rb"(esr=0x00000000f200b2b2 elr=0x)[0-9a-f]{16}",
                                  rb"\g<1>0000000040080000", log), "BRK elr"
    yield "BRK spsr I unmasked", re.sub(
        rb"(esr=0x00000000f200b2b2 elr=0x[0-9a-f]{16} spsr=)0x[0-9a-f]{16}",
        rb"\g<1>0x0000000080000345", log), "DAIF all masked"
    yield "irq line in BRK tail", insert_after(
        b"JeiOS/aarch64: triggering deliberate IRQ-path BRK",
        b"JeiOS/aarch64: irq stray\r"), "irq tick lines"
    yield "extra line after terminal", log + b"JeiOS/aarch64: late\r\n", \
        "after terminal"


def run(build, shim, args=()):
    cmd = [sys.executable, str(SMOKE), str(build / "Image"), "--elf",
           str(build / "jeios-arm.elf"), "--qemu", str(shim), "--timeout", "10",
           "--require", *args]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/aarch64-timertest")
    good = (build / "serial.log").read_bytes()
    lines = good.split(b"\n")
    failures = 0
    with tempfile.TemporaryDirectory(prefix="jeios-timer-mut-") as td:
        td = Path(td)
        replay = td / "replay.log"
        shim = td / "qemu-shim"
        shim.write_text(SHIM % str(replay))
        shim.chmod(0o755)

        def case(name, data, expect_pass, needle, args=("--timer-test",)):
            nonlocal failures
            replay.write_bytes(data)
            rc, out = run(build, shim, args)
            last = out.strip().splitlines()[-1] if out.strip() else ""
            ok = (rc == 0 and last.endswith("PASS")) if expect_pass else \
                 (rc != 0 and "FAIL" in last and needle in last)
            print(f"{'ok ' if ok else 'BAD'} {name}: {last}")
            failures += not ok

        case("baseline good log", good, True, "")
        for name, data, needle in mutations(good):
            assert data != good, name
            case(name, data, False, needle)
        stalled = next(k for k, ln in enumerate(lines)
                       if ln.startswith(b"JeiOS/aarch64: timer cntfrq="))
        stalled_log = b"\n".join(
            lines[:stalled + 1] +
            [b"JeiOS/aarch64: FATAL timer stalled"] +
            lines[stalled + 1:])
        case("stalled timer marker", stalled_log, False, "timer-stalled",
             args=("--timer-test", "--stalled-timer"))
        # Mode discipline: the same timer-test log in the default mode, and a
        # default-mode expectation of no irq lines.
        case("timer-test log without --timer-test", good, False, "", args=())
        case("--timer-test with --timer-poll", good, False, "only valid alone",
             args=("--timer-test", "--timer-poll"))
    print(f"timer-test oracle mutations: {'PASS' if not failures else 'FAIL'} "
          f"({failures} failing case(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
