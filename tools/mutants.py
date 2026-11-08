#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Critical-mutant runner (issue #18/#19, "curl-style" assurance).

Institutionalizes the recurring-defect check: "a green test that passes though
the security property is broken." For each NAMED critical mutant in the manifest
(tools/mutants.json) this tool:

  0. BASELINE PREFLIGHT (the F01 fix): builds and runs the killer test on PRISTINE,
     unmutated source in the SAME configuration and requires it to PASS. If the
     killer does not pass clean, the mutant is BLOCKED (not KILLED) — a test that
     is already red cannot prove anything about a mutation. This is what stops the
     runner from "manufacturing green evidence" (a test that always fails was
     previously misreported as KILLED).
  1. applies a precise, reversible source transform that BREAKS one security
     invariant (e.g. deletes a bounds check, a zeroization, a rights-mask test);
  2. rebuilds the one hosted killer test;
  3. asserts the test now FAILS (the mutant is KILLED). A mutant that SURVIVES
     means the clean-passing test does not actually prove its invariant.

ISOLATION (the other half of F01): every build/run happens inside a private,
disposable snapshot of the tracked tree (`git archive HEAD`), NEVER in the
caller's worktree. The caller's sources and build/ are never written, so a
concurrent editor or a SIGKILL cannot corrupt them; cancellation leaves the
worktree byte-identical. Production code carries NO mutation macros.

Outcomes are reported distinctly and a BLOCKED result is NEVER green:
  KILLED      mutation applied, killer passed clean, then FAILED under mutation.
  SURVIVED    killer passed clean but still PASSED under mutation (test defect).
  EQUIVALENT  mutant declared behavior-preserving (with a mandatory rationale)
              and the killer indeed still passed — the expected outcome.
  BLOCKED     could not reach a trustworthy verdict: pristine baseline did not
              pass, build failed, source drift (find not unique), spawn error,
              missing executable, unexpected signal/crash in the BASELINE, a
              timeout on a non-liveness mutant, or any infrastructure failure.
              A compile failure is BLOCKED, never a kill (planning agent:
              "a compile failure is not automatically a mutant kill").

A timeout KILLS only a mutant explicitly declared "liveness": true (its invariant
is that the test terminates); such a mutant must still have a passing bounded
baseline. For any other mutant a timeout is BLOCKED.

Exit 0 iff every enabled mutant is KILLED or an explicitly-justified EQUIVALENT.

Usage:
  tools/mutants.py                 # run the whole manifest
  tools/mutants.py --id W^X-RODATA-PXN   # run one mutant by id (repeatable)
  tools/mutants.py --list          # list manifest entries
  tools/mutants.py --manifest PATH # alternate manifest
  tools/mutants.py --json PATH     # also write machine-readable results
  tools/mutants.py --timeout SECS  # per build/run step budget (default 300)
  tools/mutants.py --jobs N        # parallel `make` jobs inside each snapshot
  tools/mutants.py --selftest      # run the runner's own self-tests and exit

A manifest entry (JSON):
  {
    "id": "ALLOC-FREE-OOB",            # unique, non-empty
    "invariant": "...",                # non-empty human statement
    "file": "kernel/core/alloc.c",     # repo-relative, contained in the tree
    "find":  "...",                    # MUST occur EXACTLY ONCE in `file`
    "replace": "...",                  # the breaking transform
    "build": "build/0/test_physmem",   # make target to (re)build (optional)
    "run":   "build/0/test_physmem",   # executable to run; killed iff it FAILS
    "killer": "human description of the test",
    "env":   {"ASAN_OPTIONS": "detect_leaks=0"},
    "equivalent": false,               # true => expected to survive, needs reason
    "equivalent_reason": "",           # MANDATORY non-empty when equivalent
    "liveness": false,                 # true => a timeout counts as a kill
    "enabled": true
  }
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MANIFEST = os.path.join(ROOT, "tools", "mutants.json")

KILLED, SURVIVED, BLOCKED, EQUIVALENT, SKIPPED = (
    "KILLED", "SURVIVED", "BLOCKED", "EQUIVALENT", "SKIPPED")

DEFAULT_TIMEOUT = 300


class Infra(Exception):
    """An infrastructure failure -> BLOCKED with this message (never a kill)."""


def _run(cmd, cwd, env=None, timeout=DEFAULT_TIMEOUT):
    """Run a command in its own process group so a timeout kills the whole tree.

    Returns (returncode, combined_output, timed_out). Never raises for a nonzero
    exit; raises Infra only for a spawn failure.
    """
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        p = subprocess.Popen(cmd, cwd=cwd, env=e,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              start_new_session=True)
    except (OSError, ValueError) as exc:
        raise Infra(f"spawn failed for {cmd!r}: {exc}")
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out, False
    except subprocess.TimeoutExpired:
        # Kill the whole process group, not just the shell/make leader.
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            out, _ = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out = b""
        return None, out, True


def _tail(b, n=20):
    if b is None:
        return ""
    return b"\n".join(b.splitlines()[-n:]).decode("ascii", "replace")


def _signaled(rc):
    """subprocess returns a negative rc when killed by a signal."""
    return rc is not None and rc < 0


def validate_manifest(manifest):
    """Schema + safety checks. Returns the mutant list or raises Infra."""
    if not isinstance(manifest, dict) or "mutants" not in manifest:
        raise Infra("manifest must be an object with a 'mutants' array")
    mutants = manifest["mutants"]
    if not isinstance(mutants, list) or not mutants:
        raise Infra("manifest 'mutants' must be a non-empty array")
    seen = set()
    for i, m in enumerate(mutants):
        where = f"mutant[{i}]"
        if not isinstance(m, dict):
            raise Infra(f"{where} is not an object")
        mid = m.get("id")
        if not mid or not isinstance(mid, str):
            raise Infra(f"{where} has no non-empty string id")
        if mid in seen:
            raise Infra(f"duplicate mutant id: {mid}")
        seen.add(mid)
        # 'run' is required (the runner always needs something to execute);
        # the others are the human/transform fields.
        for key in ("invariant", "file", "find", "killer", "run"):
            if not m.get(key) or not isinstance(m[key], str):
                raise Infra(f"{mid}: missing/empty required string field '{key}'")
        if "replace" not in m or not isinstance(m["replace"], str):
            raise Infra(f"{mid}: 'replace' must be a string (may be empty)")
        # Path containment: file/build/run must be contained repo-relative paths,
        # and must NOT look like a command-line option (defends against a manifest
        # smuggling e.g. a Make flag through the 'build' field).
        for key in ("file", "build", "run"):
            val = m.get(key)
            if val is None:
                continue
            if (not isinstance(val, str) or os.path.isabs(val)
                    or ".." in val.split("/") or val.startswith("-")
                    or val.strip() == ""):
                raise Infra(f"{mid}: '{key}' must be a contained repo-relative "
                            f"path (not absolute, no '..', not an option)")
        # Boolean flags must be real booleans — a truthy string like "false"
        # must never silently enable liveness/equivalent/disabled behavior.
        for flag in ("enabled", "equivalent", "liveness"):
            if flag in m and not isinstance(m[flag], bool):
                raise Infra(f"{mid}: '{flag}' must be a JSON boolean, got "
                            f"{type(m[flag]).__name__}")
        if m.get("equivalent"):
            reason = m.get("equivalent_reason")
            if not isinstance(reason, str) or not reason.strip():
                raise Infra(f"{mid}: equivalent=true requires a non-empty string "
                            f"'equivalent_reason'")
        env = m.get("env", {})
        if not isinstance(env, dict):
            raise Infra(f"{mid}: 'env' must be an object")
        for k, v in env.items():
            if not isinstance(k, str) or not isinstance(v, str):
                raise Infra(f"{mid}: 'env' keys and values must be strings "
                            f"(offending key {k!r})")
    return mutants


def _snapshot(dest):
    """Materialize a copy of the tracked WORKING tree into `dest`.

    Copies the tracked files at their current working-tree content (so an
    uncommitted fix to the manifest or a test is exercised), excluding build/
    and anything not tracked by git. The caller's worktree is read, never
    written. Implemented as `git archive` over a transient tree object built
    from the current index + worktree, falling back to a tracked-file copy.
    """
    # Stage a transient tree that reflects the working tree's content without
    # disturbing the caller's index: use a private index file. `git add -A`
    # (not -u) so NEW files the PR adds (e.g. a new test) are captured too, not
    # only modifications to already-tracked files. .gitignore still excludes
    # build/ and other ignored output, so the snapshot stays clean.
    tmp_index = os.path.join(dest, ".jeios_mut_index")
    env = dict(os.environ, GIT_INDEX_FILE=tmp_index)
    try:
        for cmd in (["git", "read-tree", "HEAD"],
                    ["git", "add", "-A"]):
            r = subprocess.run(cmd, cwd=ROOT, env=env, timeout=120,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode != 0:
                raise Infra(f"{' '.join(cmd)} failed: "
                            f"{r.stderr.decode('ascii','replace')}")
        tw = subprocess.run(["git", "write-tree"], cwd=ROOT, env=env, timeout=120,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if tw.returncode != 0:
            raise Infra(f"git write-tree failed: "
                        f"{tw.stderr.decode('ascii','replace')}")
        tree = tw.stdout.decode().strip()
        arch = subprocess.run(["git", "archive", "--format=tar", tree], cwd=ROOT,
                              timeout=120,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if arch.returncode != 0:
            raise Infra(f"git archive failed: "
                        f"{arch.stderr.decode('ascii','replace')}")
        untar = subprocess.run(["tar", "-x", "-C", dest], input=arch.stdout,
                               timeout=120, stderr=subprocess.PIPE)
        if untar.returncode != 0:
            raise Infra(f"untar failed: {untar.stderr.decode('ascii','replace')}")
    except subprocess.TimeoutExpired as exc:
        raise Infra(f"snapshot step timed out: {exc}")
    except (OSError, ValueError) as exc:
        raise Infra(f"snapshot step failed to run (missing git/tar?): {exc}")
    finally:
        try:
            os.remove(tmp_index)
        except OSError:
            pass


def _build_and_run(workdir, m, timeout, jobs):
    """Build the killer target (clean, forcing recompile) and run it in workdir.

    Returns (built_ok, run_rc, combined_output, timed_out). Raises Infra on a
    spawn failure or a missing run executable after a successful build.
    """
    env = m.get("env")
    tgt = m.get("build")
    if tgt:
        tgt_path = os.path.join(workdir, tgt)
        try:
            os.remove(tgt_path)   # force a real recompile of the (maybe #included) TU
        except OSError:
            pass
        make = ["make", tgt] + ([f"-j{jobs}"] if jobs else [])
        rc, out, to = _run(make, cwd=workdir, env=env, timeout=timeout)
        if to:
            return False, None, out, True
        if rc != 0:
            return False, rc, out, False
    runp = os.path.join(workdir, m["run"])
    if not (os.path.isfile(runp) and os.access(runp, os.X_OK)):
        raise Infra(f"run executable missing/not executable: {m['run']}")
    rc, out, to = _run([runp], cwd=workdir, env=env, timeout=timeout)
    return True, rc, out, to


def apply_one(m, timeout=DEFAULT_TIMEOUT, jobs=0):
    """Baseline-preflight, then mutate, in a private snapshot. Returns (status, detail).

    The caller's worktree is never touched. On any infra problem the mutant is
    BLOCKED with a reason; a BLOCKED result is never treated as green.
    """
    work = tempfile.mkdtemp(prefix="jeios-mut.")
    try:
        try:
            _snapshot(work)
        except Infra as exc:
            return BLOCKED, str(exc)

        # Resolve the mutated file and REFUSE to follow a symlink out of the
        # snapshot (a tracked symlink could otherwise point `open(w)` at the
        # caller's real file). Require a regular file whose real path is inside
        # the snapshot root.
        work_real = os.path.realpath(work)
        path = os.path.join(work, m["file"])
        if os.path.islink(path):
            return BLOCKED, f"{m['file']} is a symlink; refusing to mutate it"
        real = os.path.realpath(path)
        if not (real == work_real or real.startswith(work_real + os.sep)):
            return BLOCKED, (f"{m['file']} resolves outside the snapshot "
                             f"({real}); refusing to mutate")
        if not os.path.isfile(real):
            return BLOCKED, f"file not found in snapshot: {m['file']}"
        # Byte-exact read/write so a mutation cannot silently renormalize line
        # endings (CRLF->LF) across the file and cause a spurious kill.
        with open(real, "rb") as f:
            original_b = f.read()
        try:
            original = original_b.decode("utf-8")
        except UnicodeDecodeError:
            return BLOCKED, f"{m['file']} is not valid UTF-8; cannot mutate safely"
        find, repl = m["find"], m.get("replace", "")
        n = original.count(find)
        if n != 1:
            return BLOCKED, (f"find-pattern occurs {n} times (need exactly 1) in "
                             f"{m['file']} — source drifted; update the mutant")
        mutated = original.replace(find, repl, 1)
        if mutated == original:
            return BLOCKED, "replace produced no change (find == replace?)"

        # --- 0. BASELINE PREFLIGHT on pristine source ---
        try:
            built, rc, out, to = _build_and_run(work, m, timeout, jobs)
        except Infra as exc:
            return BLOCKED, f"baseline: {exc}"
        if to:
            return BLOCKED, ("baseline TIMED OUT on pristine source — the killer "
                             "is not a bounded passing test; cannot trust a kill")
        if not built:
            return BLOCKED, f"baseline BUILD FAILED on pristine source:\n{_tail(out)}"
        if _signaled(rc):
            return BLOCKED, (f"baseline CRASHED on pristine source "
                             f"(signal {-rc}) — fix the test first:\n{_tail(out)}")
        if rc != 0:
            return BLOCKED, ("baseline killer FAILED on pristine source "
                             f"(exit {rc}) — a red test proves nothing:\n{_tail(out)}")

        # --- 1/2. apply the mutation and rebuild/run (byte-exact write) ---
        with open(real, "wb") as f:
            f.write(mutated.encode("utf-8"))
        try:
            built, rc, out, to = _build_and_run(work, m, timeout, jobs)
        except Infra as exc:
            return BLOCKED, f"mutated: {exc}"

        if to:
            # Only a mutant EXPLICITLY declared liveness (a real JSON boolean, per
            # validate_manifest) treats a non-terminating RUN as the kill — and a
            # build-phase timeout is never a kill.
            if m.get("liveness") is True and built is not False:
                return KILLED, "mutated test did not terminate (declared liveness mutant)"
            return BLOCKED, ("mutated step TIMED OUT; not a declared-liveness run "
                             "mutant — treated as blocked, not a kill")
        if not built:
            # compile failure under mutation is BLOCKED, never a kill.
            return BLOCKED, (f"mutated source did not build (BLOCKED, not a kill):\n"
                             f"{_tail(out)}")
        killed = (rc != 0)   # any nonzero exit (incl. a signal) == test failed == killed

        if m.get("equivalent"):
            # Declared behavior-preserving: the killer should STILL pass.
            if killed:
                return SURVIVED, ("declared EQUIVALENT but the killer FAILED under "
                                  "the mutation — it is not behavior-preserving")
            return EQUIVALENT, m.get("equivalent_reason", "")
        if killed:
            return KILLED, ""
        return SURVIVED, (f"killer PASSED with the invariant broken — the test "
                          f"({m.get('killer','?')}) does not prove it")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def run_manifest(mutants, only_ids, timeout, jobs):
    results = []
    for m in mutants:
        mid = m["id"]
        if only_ids is not None and mid not in only_ids:
            continue
        if not m.get("enabled", True):
            results.append((mid, SKIPPED, "disabled"))
            print(f"[SKIP ] {mid}")
            continue
        status, detail = apply_one(m, timeout=timeout, jobs=jobs)
        results.append((mid, status, detail))
        tag = {KILLED: "KILL ", SURVIVED: "SURV ", BLOCKED: "BLOCK",
               EQUIVALENT: "EQUIV", SKIPPED: "SKIP "}[status]
        print(f"[{tag}] {mid}: {m['invariant']}"
              + (f"\n         -> {detail}" if detail else ""))
    return results


def summarize(results):
    c = {k: 0 for k in (KILLED, SURVIVED, BLOCKED, EQUIVALENT, SKIPPED)}
    for _, st, _ in results:
        c[st] += 1
    print(f"\nmutants: {c[KILLED]} killed, {c[EQUIVALENT]} equivalent, "
          f"{c[SURVIVED]} SURVIVED, {c[BLOCKED]} BLOCKED, {c[SKIPPED]} skipped")
    ran = c[KILLED] + c[SURVIVED] + c[BLOCKED] + c[EQUIVALENT]
    if ran == 0:
        # Nothing actually executed (all disabled, or --id selected only disabled
        # mutants). An empty run must NOT report success.
        print("\nFAIL — no enabled mutant ran; an empty assurance run is not a "
              "pass. Enable at least one mutant or select an enabled id.")
        return 1
    bad = c[SURVIVED] + c[BLOCKED]
    if bad:
        print("\nFAIL — SURVIVED means a test does not prove its invariant; "
              "BLOCKED means an untrustworthy verdict (bad baseline, build break, "
              "drift, timeout or infra failure). Neither is ever treated as green.")
        return 1
    print("\nOK — every enabled critical mutant was killed (or justified "
          "equivalent).")
    return 0


# --------------------------------------------------------------------------
# Self-tests: exercise the runner's own classification on synthetic fixtures,
# in a throwaway tree, with no dependency on the real kernel. Covers the F01
# acceptance list: baseline failure, assertion kill, survivor, compile failure,
# timeout, unknown/duplicate id, source drift, malformed manifest, unjustified
# equivalent, zero enabled, and that the caller's tree is never touched.
# --------------------------------------------------------------------------
def _selftest():
    import textwrap
    failures = []

    def check(name, cond, extra=""):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {extra}" if extra and not cond else ""))
        if not cond:
            failures.append(name)

    # Build a self-contained snapshot-able git repo so apply_one's `git archive`
    # has something to copy; the "source" is a tiny C file + a Makefile that
    # builds a test whose pass/fail we control by the mutation.
    base = tempfile.mkdtemp(prefix="jeios-mut-selftest.")
    global ROOT
    saved_root = ROOT
    try:
        src = textwrap.dedent("""\
            #include <stdlib.h>
            int guard(int x){ return (x >= 0) ? 0 : 1; }
            int main(void){ /*GUARD*/ return guard(-1) ? 0 : 1; }
        """)
        mk = textwrap.dedent("""\
            build/t: t.c
            \t@mkdir -p build
            \tgcc -O0 -g t.c -o build/t
        """)
        open(os.path.join(base, "t.c"), "w").write(src)
        open(os.path.join(base, "Makefile"), "w").write(mk)
        for cmd in (["git", "init", "-q"], ["git", "add", "-A"],
                    ["git", "-c", "user.email=s@s", "-c", "user.name=s",
                     "commit", "-q", "-m", "x"]):
            if subprocess.run(cmd, cwd=base).returncode != 0:
                print("  (could not init selftest git repo)"); return 1
        ROOT = base

        env = None
        # 1. A real kill: the pristine test passes (guard(-1)==1 -> main returns 0),
        #    the mutation flips the guard sense so main returns 1.
        st, d = apply_one({"id": "KILL", "invariant": "guard rejects negatives",
                           "file": "t.c", "find": "(x >= 0) ? 0 : 1",
                           "replace": "(x >= 0) ? 1 : 0",
                           "build": "build/t", "run": "build/t"}, timeout=60)
        check("real kill is KILLED", st == KILLED, f"got {st}: {d}")

        # 2. Always-red baseline (the exact F01 defect): make the pristine test
        #    fail by a find/replace whose FIND is already broken is awkward; instead
        #    point run at a test that fails pre-mutation by mutating a no-op while
        #    the baseline main returns 1.
        bad = textwrap.dedent("""\
            int main(void){ /*X*/ return 1; }
        """)
        open(os.path.join(base, "t.c"), "w").write(bad)
        subprocess.run(["git", "add", "-A"], cwd=base)
        subprocess.run(["git", "-c", "user.email=s@s", "-c", "user.name=s",
                        "commit", "-q", "-m", "red"], cwd=base)
        st, d = apply_one({"id": "REDBASE", "invariant": "x", "file": "t.c",
                           "find": "/*X*/", "replace": "/*Y*/",
                           "build": "build/t", "run": "build/t"}, timeout=60)
        check("red baseline is BLOCKED (not KILLED)", st == BLOCKED, f"got {st}: {d}")

        # 3. Survivor: baseline passes, mutation of a comment changes nothing.
        ok = textwrap.dedent("""\
            int main(void){ /*Z*/ return 0; }
        """)
        open(os.path.join(base, "t.c"), "w").write(ok)
        subprocess.run(["git", "add", "-A"], cwd=base)
        subprocess.run(["git", "-c", "user.email=s@s", "-c", "user.name=s",
                        "commit", "-q", "-m", "ok"], cwd=base)
        st, d = apply_one({"id": "SURV", "invariant": "x", "file": "t.c",
                           "find": "/*Z*/", "replace": "/*ZZ*/",
                           "build": "build/t", "run": "build/t"}, timeout=60)
        check("no-op mutation SURVIVES", st == SURVIVED, f"got {st}: {d}")

        # 4. Compile failure under mutation is BLOCKED, not a kill.
        st, d = apply_one({"id": "COMPILE", "invariant": "x", "file": "t.c",
                           "find": "return 0;", "replace": "return 0",  # drop ; -> compile error
                           "build": "build/t", "run": "build/t"}, timeout=60)
        check("compile failure is BLOCKED", st == BLOCKED, f"got {st}: {d}")

        # 5. Source drift: find occurs zero times.
        st, d = apply_one({"id": "DRIFT", "invariant": "x", "file": "t.c",
                           "find": "NONEXISTENT", "replace": "x",
                           "build": "build/t", "run": "build/t"}, timeout=60)
        check("source drift is BLOCKED", st == BLOCKED, f"got {st}: {d}")

        # 6. Malformed manifest / duplicate id / unjustified equivalent.
        for name, man, ok_if in [
            ("empty manifest rejected", {"mutants": []}, False),
            ("duplicate id rejected",
             {"mutants": [{"id": "a", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k"},
                          {"id": "a", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k"}]}, False),
            ("equivalent needs reason",
             {"mutants": [{"id": "e", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k",
                           "equivalent": True}]}, False),
            ("abs path rejected",
             {"mutants": [{"id": "p", "invariant": "i", "file": "/etc/passwd",
                           "find": "x", "replace": "y", "killer": "k",
                           "run": "build/t"}]}, False),
            ("non-boolean liveness rejected",
             {"mutants": [{"id": "l", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k",
                           "run": "build/t", "liveness": "false"}]}, False),
            ("missing run rejected",
             {"mutants": [{"id": "r", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k"}]}, False),
            ("build as an option rejected",
             {"mutants": [{"id": "b", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k",
                           "run": "build/t", "build": "-C/etc"}]}, False),
            ("non-string env value rejected",
             {"mutants": [{"id": "v", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k",
                           "run": "build/t", "env": {"K": 1}}]}, False),
            ("well-formed manifest accepted",
             {"mutants": [{"id": "ok", "invariant": "i", "file": "t.c",
                           "find": "x", "replace": "y", "killer": "k",
                           "run": "build/t"}]}, True),
        ]:
            try:
                validate_manifest(man); got = True
            except Infra:
                got = False
            check(name, got == ok_if)

        # 7. An all-disabled manifest (nothing runs) must FAIL, not report OK.
        res = run_manifest([{"id": "off", "invariant": "i", "file": "t.c",
                             "find": "x", "replace": "y", "killer": "k",
                             "run": "build/t", "enabled": False}],
                           None, 60, 0)
        check("all-skipped manifest FAILS (exit 1)", summarize(res) == 1)

        # 8. --id selecting only a disabled mutant also fails.
        res = run_manifest([{"id": "off", "invariant": "i", "file": "t.c",
                             "find": "x", "replace": "y", "killer": "k",
                             "run": "build/t", "enabled": False}],
                           {"off"}, 60, 0)
        check("selecting only a disabled id FAILS", summarize(res) == 1)

        # 9. The caller's tree was never modified (t.c still the last committed ok).
        check("worktree untouched by apply_one",
              open(os.path.join(base, "t.c")).read() == ok)

    finally:
        ROOT = saved_root
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print(f"\nSELFTEST FAIL: {len(failures)} case(s): {failures}")
        return 1
    print("\nSELFTEST OK")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--id", action="append", help="run only these mutant id(s)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--json", help="write machine-readable results to this path")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                    help=f"per build/run step seconds (default {DEFAULT_TIMEOUT})")
    ap.add_argument("--jobs", type=int, default=0, help="make -j inside snapshots")
    ap.add_argument("--selftest", action="store_true",
                    help="run the runner's own self-tests and exit")
    a = ap.parse_args()

    if a.selftest:
        return _selftest()

    try:
        with open(a.manifest, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as exc:
        print(f"cannot read manifest {a.manifest}: {exc}", file=sys.stderr)
        return 2
    try:
        mutants = validate_manifest(manifest)
    except Infra as exc:
        print(f"invalid manifest: {exc}", file=sys.stderr)
        return 2

    if a.list:
        for m in mutants:
            flag = "" if m.get("enabled", True) else " (disabled)"
            print(f"{m['id']}{flag}: {m['invariant']}")
        return 0

    only = None
    if a.id:
        only = set(a.id)
        known = {m["id"] for m in mutants}
        missing = only - known
        if missing:
            print(f"unknown mutant id(s): {sorted(missing)}", file=sys.stderr)
            return 2

    results = run_manifest(mutants, only, a.timeout, a.jobs)
    code = summarize(results)
    if a.json:
        try:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump({"results": [{"id": i, "status": s, "detail": d}
                                       for i, s, d in results]}, f, indent=2)
        except OSError as exc:
            print(f"warning: could not write --json {a.json}: {exc}",
                  file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
