**Verdict: FIX FIRST.** I found no defect in the code itself; it behaves exactly like the original. What has to be fixed before merge is a false statement in the handoff and a test gate that ADR 0014 requires. All my runs used `git archive` exports under `/tmp/rv14`. I made no edits to your worktree. It has uncommitted `Makefile` and `tools/check_lifecycle_boundary.sh` changes, which are not part of HEAD and which I left alone.

## Item 1: ordering
**Yes, `je_lc_on_dying` matches the original `mark_dying` prefix on every branch:**
- **Waiter release:** always runs, including for legacy threads with `gen==0`.
- **Legacy path (`gen==0`):** returns `JE_OK`, then `mark_dying` writes the death record. This is the original "release only" behaviour.
- **Fault note:** only when `reason==0` and `gen!=0` and not a hosted build. `DEATH_FAULT` is 0.
- **Device dying, then `je_proc_on_dying`:** only when `gen!=0`. A failed `je_proc_on_dying` makes `mark_dying` call `abort_scheduler()` before any slot write, as before.
- **One change:** `proc_id` is now read once, before the release, instead of re-read after it. This is equivalent because neither the release (`endpoint_kern.c:380-400`) nor the device hooks (`device.c:166-228`, which only call `je_sched_ticks()`) write `threads[]`.
- **Hosted guard:** correct in both tests. `test_sched_killall` gets `-DJE_SCHED_HOSTED_TEST` on the command line (`Makefile:261`), so it also applies to the included `lifecycle.c`. `test_lifecycle_order` doesn't define it, so the hooks are present and my mutants that flip or drop them are caught.

**Items 2–6 are all clean:**
- **Item 2:** sched.c no longer writes the registry directly. The only write is the owner API call `je_sched_unregister_process` in `drop_user`. There are no new cached values. `has_process` and `current_proc_id` only read `threads[]`, and `find_process` was converted correctly.
- **Item 3:** `je_lc_reaped` fails in the same two cases as before, adds no new abort reasons, and `gen==0` returns `JE_OK` after destroy.
- **Item 5:** `kill_all` only swaps three `process_pool` reads for `je_lc_proc_pool()`. Both passes are intact.
- **Item 6:** argument orders are correct, the moved registry functions are verbatim, and storage is `static`. There is no header cycle, and `lifecycle.h` compiles on its own for x86 and aarch64.

## BLOCKER
None.

## SHOULD-FIX
1. **`docs/HANDOFF.md:88`: the reason given for skipping smoke is false.** It says there is "no boot image… change". In fact `lifecycle.c` is a new kernel source (`Makefile:21`) and the kernel's dying, kill and reap code changed. ADR 0014 part 3 requires `make smoke`, `make integration` and every `JE_TEST_MODE` before merge. I ran them on HEAD (QEMU 8.2.2, gcc 13.3) and **all passed**:
   - `make check`: 259 and 50 checks.
   - `make smoke`, plus the three CI variants: no-NX, no-NX with 16M, and 1536M with NX.
   - `make integration`: modes 1–15.

   Record this evidence and remove the false reason.
2. **The ADR part 3 neutral conformance suite is still missing, and hosted tests leave moved behaviour unpinned.** I ran a quick mutation probe on `lifecycle.c`:
   - **Not caught by any hosted test:**
     - **M05:** a failed `je_proc_on_dying` is swallowed. Smoke can't reach this either, because it only fails with a corrupted slot id.
     - **M07:** `je_proc_on_dying` moved before the device hooks. `test_lifecycle_order`'s header says it checks this, but its fakes never record the PROCESS state when they're called.
     - The kill checks, all registry writers, the exit code and the fault vector.
   - The same M05 and M07 mutations also survive on HEAD~1, so these are existing gaps in the tests, not regressions.
   - QEMU smoke does catch the supervisor, self-kill, already-dying and exit-code mutants. They are invisible to `make check` and `make mutants`.
   - Use this list as the spec for `test_proc_lifecycle`. Separately, add to `test_lifecycle_order` an assertion that the record is LIVE inside the device fakes, and a stale-id case that must fail-stop.
3. **`kernel/core/sched.c:12-14`: a test-only `#include "lifecycle.c"` sits in production code.** No current build compiles `lifecycle.c` twice; a wrongly set macro would give a loud link error. A cleaner option works: a two-step compile with `-include kernel/core/lifecycle.c` applied only to the test file. I tried it: 50 checks pass, sched.c stays clean, and the test file is unchanged. A one-command `-include` fails, because it also prepends to `proc.c` and duplicates the definitions.
4. **`lifecycle.c` and `lifecycle.h` contain no comments, and the M3c A6-CLARIFY-2 contract comment was deleted from `mark_dying` without being moved.** AGENTS.md rule 4 and coding-standard rules JCS-03/06/09 require these contracts:
   - The module is the sole owner of the registry.
   - Callers run with interrupts off on a single CPU.
   - Callers must fail-stop when `je_lc_on_dying` returns an error.
   - The release is keyed on the thread slot (which is the waiter id) and is idempotent.
   - `je_lc_reaped` may only be called after the drop; the kernel stack is freed by destroy, never by reap.

## NIT
- **`lifecycle.c:75`:** `reason == 0u` is a magic number tied to sched.c's private enum. Use `JE_ABI_DEATH_FAULT`, which is already visible through `device.h` → `abi.h`, or a constant pinned with `_Static_assert`, following the `JE_PROC_REASON_MAX` pattern in `proc.h:44-47`. Today's tests do catch a flipped value.
- **`sched.h:7,19-23`:**
  - The four registry prototypes are now declared twice.
  - Because `sched.h` includes `lifecycle.h`, every `je_lc_*` function becomes visible to 8 other files. That includes `je_lc_on_dying` and `je_lc_reaped`, which do only half a transition.
  - Include `lifecycle.h` from `sched.c` only, or drop the duplicate prototypes.
- **`sched.c:684-692`:** `je_sched_kill` now looks up the caller before the victim's status. Both lookups have no side effects and both return `JE_ERR_INVALID`, so nothing observable changes. Say so in the PR narrative.
- **`lifecycle.c:57`:** `je_lc_proc_pool` duplicates `je_sched_proc_pool` instead of one forwarding to the other.
- **`sched.c:199-200`:** the registered id is read twice; read it once into a local.
- **`sched.c:202`:** `je_proc_link` still writes PROCESS link state directly from sched.c. ADR part 1 counts the thread↔VM link as lifecycle, so the ARM scheduler in MP3 will need a `je_lc_link` wrapper.
- **HANDOFF wording:**
  - It says "all five reaper/F08 mutant anchors"; there are six. I confirmed each is present exactly once in both versions.
  - The record cites 9c356e8 plus worktree changes instead of e5c4ead.
  - The new section sits above an older "## Current status — 13:56" heading.
- **`tests/test_sched_killall.c:12`:** the comment saying `je_sched_unregister_process` is sched.c code is now wrong. It can't change under the byte-identical rule, so fix it in a follow-up.
- **Uncommitted boundary script (not in HEAD):** its ARM compile has no `-Werror`, and its token list leaves out `<<9`. An allowlist check on `lifecycle.o`'s undefined symbols would be stronger than grepping for tokens. `lifecycle.c` itself compiles clean on ARM with `-Werror -Wconversion -Wshadow`.

I did not rerun `make mutants`; your result stands as reported. Logs are in `/tmp/rv14/*.log`.
