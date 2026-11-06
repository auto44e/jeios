#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Critical-mutant runner (issue #18/#19, "curl-style" assurance).

Institutionalizes the recurring-defect check: "a green test that passes though
the security property is broken." For each NAMED critical mutant in the manifest
(tools/mutants.json) this tool:
  1. applies a precise, reversible source transform that BREAKS one security
     invariant (e.g. deletes a bounds check, a zeroization, a rights-mask test);
  2. rebuilds the one hosted test that is supposed to PROVE that invariant;
  3. asserts the test now FAILS (the mutant is KILLED). A mutant that SURVIVES
     means the test does not actually prove its invariant — a defect in the test.
  4. restores the source unconditionally (even on error / SIGINT).

Production code carries NO mutation macros; mutants live entirely here + in the
manifest, so the kernel source stays clean. Outcomes are reported distinctly
(killed / SURVIVED / blocked / equivalent) per the planning-agent requirement; a
toolchain/build problem is BLOCKED, never silently green. Exit 0 iff every
enabled mutant is KILLED (or an explicitly-justified EQUIVALENT).

Usage:
  tools/mutants.py                 # run the whole manifest
  tools/mutants.py --id W^X-RODATA-PXN   # run one mutant by id
  tools/mutants.py --list          # list manifest entries
  tools/mutants.py --manifest PATH # alternate manifest

A manifest entry (JSON):
  {
    "id": "ALLOC-FREE-OOB",
    "invariant": "je_pages_free rejects out-of-window addresses before indexing",
    "file": "kernel/core/alloc.c",
    "find": " || (a-JE_PHYSICAL_BASE)>=JE_PHYSICAL_SPAN)",
    "replace": ")",
    "build": "build/0/test_physmem_arm",     # make target to (re)build
    "run":   "build/0/test_physmem_arm",      # binary to run; killed iff nonzero
    "killer": "test_physmem_arm (out-of-window free rejection)",
    "env": {"ASAN_OPTIONS": "detect_leaks=0"},
    "equivalent": false,            # true => expected to survive, with a reason
    "equivalent_reason": "",
    "enabled": true
  }
`find` MUST occur EXACTLY ONCE in `file` (else the mutant is BLOCKED — the source
drifted and the mutant needs updating; this is the drift guard).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MANIFEST = os.path.join(ROOT, "tools", "mutants.json")

KILLED, SURVIVED, BLOCKED, EQUIVALENT = "KILLED", "SURVIVED", "BLOCKED", "EQUIVALENT"


def run(cmd, env=None, cwd=ROOT):
    e = dict(os.environ)
    if env:
        e.update(env)
    return subprocess.run(cmd, cwd=cwd, env=e, shell=isinstance(cmd, str),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def apply_one(m):
    """Apply, build, run, classify one mutant. Always restores the file."""
    path = os.path.join(ROOT, m["file"])
    if not os.path.isfile(path):
        return BLOCKED, f"file not found: {m['file']}"
    original = open(path, "r", encoding="utf-8").read()
    find, repl = m["find"], m.get("replace", "")
    n = original.count(find)
    if n != 1:
        return BLOCKED, (f"find-pattern occurs {n} times (need exactly 1) in "
                         f"{m['file']} — source drifted; update the mutant")
    mutated = original.replace(find, repl, 1)
    if mutated == original:
        return BLOCKED, "replace produced no change"
    # Clean the built target so the rebuild actually recompiles the mutated TU.
    tgt = m.get("build")
    if tgt:
        try:
            os.remove(os.path.join(ROOT, tgt))
        except OSError:
            pass
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(mutated)
        if tgt:
            b = run(["make", tgt], env=m.get("env"))
            if b.returncode != 0:
                # A build break can legitimately "kill" a mutant (a deleted check
                # that was load-bearing for compilation). Treat a BUILD FAILURE as
                # killed ONLY if the manifest says so; by default it is BLOCKED,
                # because a compile error is not a runtime invariant test (planning
                # agent: "a compile failure is not automatically a mutant kill").
                if m.get("build_fail_is_kill", False):
                    return KILLED, "build failed (manifest: build-fail counts as kill)"
                return BLOCKED, f"build failed:\n{tail(b.stdout)}"
        r = run([os.path.join(ROOT, m["run"])], env=m.get("env"))
        killed = r.returncode != 0
    finally:
        with open(path, "w", encoding="utf-8") as f:
            f.write(original)
        if tgt:
            try:
                os.remove(os.path.join(ROOT, tgt))  # drop the mutated binary
            except OSError:
                pass
    if m.get("equivalent", False):
        # Expected to SURVIVE (no behavioral change). Survival is the pass here.
        return (EQUIVALENT if not killed else SURVIVED), m.get("equivalent_reason", "")
    return (KILLED if killed else SURVIVED), ("" if killed else
            f"test PASSED with the invariant broken — the killer test "
            f"({m.get('killer','?')}) does not prove it")


def tail(b, n=20):
    return b"\n".join(b.splitlines()[-n:]).decode("ascii", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--id", action="append", help="run only these mutant id(s)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    with open(a.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    mutants = manifest["mutants"]
    if a.list:
        for m in mutants:
            flag = "" if m.get("enabled", True) else " (disabled)"
            print(f"{m['id']}{flag}: {m['invariant']}")
        return 0
    if a.id:
        want = set(a.id)
        mutants = [m for m in mutants if m["id"] in want]
        missing = want - {m["id"] for m in mutants}
        if missing:
            print(f"unknown mutant id(s): {sorted(missing)}", file=sys.stderr)
            return 2
    results = []
    for m in mutants:
        if not m.get("enabled", True):
            results.append((m["id"], "SKIPPED", "disabled"))
            print(f"[SKIP ] {m['id']}")
            continue
        status, detail = apply_one(m)
        results.append((m["id"], status, detail))
        tag = {KILLED: "KILL ", SURVIVED: "SURV ", BLOCKED: "BLOCK",
               EQUIVALENT: "EQUIV"}[status]
        print(f"[{tag}] {m['id']}: {m['invariant']}"
              + (f"\n         -> {detail}" if detail else ""))
    bad = [r for r in results if r[1] in (SURVIVED, BLOCKED)]
    killed = sum(1 for r in results if r[1] == KILLED)
    equiv = sum(1 for r in results if r[1] == EQUIVALENT)
    print(f"\nmutants: {killed} killed, {equiv} equivalent, "
          f"{sum(1 for r in results if r[1]==SURVIVED)} SURVIVED, "
          f"{sum(1 for r in results if r[1]==BLOCKED)} BLOCKED, "
          f"{sum(1 for r in results if r[1]=='SKIPPED')} skipped")
    if bad:
        print("\nFAIL — a surviving mutant means a test does not prove its "
              "invariant; a blocked mutant means the source drifted or the build "
              "broke. Both must be resolved (never treated as green).")
        return 1
    print("\nOK — every enabled critical mutant was killed (or justified "
          "equivalent).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
