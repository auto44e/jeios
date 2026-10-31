# WORKER_NOTES — M4 PR-B test writer (#53, ADR 0010 D3/D4/D-DEV)

Observed 2026-10-31 (UTC date, from the session; no wall-clock time recorded).
Worktree `wt-m4b-tests`, branch `wip/m4b-tests`, base commit `1623f1e` (pre-wiring).
Role: TEST WRITER. Author: one AI agent (Claude). Nothing here is human review.
Uncommitted. I did not commit, push or touch git config. No network.

## State
- **Implemented:** the tests listed below.
- **Tested:** they compile warning-free under the Makefile flags. Against the stubs
  they fail as expected (mapped below). Against my own throwaway reference
  implementations they pass, and the suites killed every planted mutant except one,
  which turned out to be equivalent (see Verification).
- **Not verified:** the real proc.c, resolve_any and create_by written by the other
  worker (not read, by brief); QEMU/smoke (not run, tests-only change); remote CI
  (none observed).

## Files changed (tests/*.c only)
| File | Change |
|---|---|
| `tests/test_proc.c` | Placeholder replaced by the full proc.h suite (11 sections, 628 lines). |
| `tests/test_proc_retire.c` | **NEW.** Generation-retirement suite. Needs `-DJE_PROC_GEN_MAX=3u` on BOTH this file and `kernel/core/proc.c`. `#error` if the define is missing; `_Static_assert` that it is 3. |
| `tests/test_handle.c` | `test_resolve_any()` added, run in both the 16/16 and the reduced-width (30/2) builds. `#include <string.h>` added. |
| `tests/test_endpoint.c` | `test_ddev()` added: 13 D-DEV cases plus helpers, called before the final printf. Existing sections untouched. |
| `tests/test_syscall.c` | KILL decode, op-15 decode, ops 13/14/16-20 UNSUPPORTED, and an op-15 64-bit alias check. The old `op 13` line is folded into a loop. |

## Makefile rule the lead must wire (I did not edit the Makefile)
```make
$(BUILD)/test_proc_retire: tests/test_proc_retire.c kernel/core/proc.c $(wildcard include/jeios/*.h)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DJE_PROC_GEN_MAX=3u -std=c11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_proc_retire.c kernel/core/proc.c -o $@
```
Also add `$(BUILD)/test_proc_retire` to the `test:` prerequisites, and `./$(BUILD)/test_proc_retire` after `test_proc` in the run list.

## Commands run and results
1. `ASAN_OPTIONS=detect_leaks=0 make test`. **rc=2.** All 11 Makefile test binaries
   compiled with zero warnings or errors. The run stopped at `test_syscall`, which
   fails as expected. Log: `/tmp/m4b/tests_final_make.log`.
2. I ran each binary individually against the in-tree stubs, because `make` stops at
   the first failure. test_proc_retire was built by hand with the rule above.

| Binary | Result vs stubs | Stub it hits |
|---|---|---|
| test_core, test_boot, test_alloc, test_vm, test_uframe, test_fdt | PASS | none (unchanged) |
| test_syscall | FAIL `test_syscall.c:146` `st==JE_ABI_OK && res==0` | `je_syscall_decode` has no `case JE_SYS_PROC_STATUS`, so op 15 returns UNSUPPORTED. The KILL-decode lines before it **pass** already, because abi.h RIGHTS_KNOWN includes KILL. |
| test_handle (16/16) | FAIL `test_handle.c:346` (first resolve_any OK) | `je_handle_resolve_any` stub fails closed. |
| test_handle_rw (30/2) | FAIL `test_handle.c:346`, same | same stub |
| test_endpoint | FAIL `test_endpoint.c:101` `proc_new` alloc OK | `je_proc_alloc` stub (proc.c), reached from `test_ddev`. Every pre-existing endpoint section (1–24) ran first and passed. |
| test_proc | FAIL `test_proc.c:78` `used_count == 0` after init | `je_proc_pool_init` stub does not initialise the pool. |
| test_proc_retire | FAIL `test_proc_retire.c:39` alloc OK | `je_proc_alloc` stub |

3. `git diff --check`: clean.
4. **Self-check against throwaway references.** These live in `/tmp/m4b-ref/`, outside
   the worktree, and are NOT for merging. I wrote them from the headers myself, so
   this proves the tests are self-consistent. It is not independent evidence.
   - `proc_ref.c` (proc.h): test_proc PASS, test_proc_retire PASS.
   - `resolve_any_ref.c`, linked via `-Dje_handle_resolve_any=ref_resolve_any` on the
     test TU only: test_handle PASS at 16/16 and at 30/2.
   - `endpoint_ref.c` is a copy of endpoint.c. It does three things:
     1. Replaces the create_by stub with a reference version. I never read the stub
        body; the python script cut it out without printing it.
     2. Makes `channel_create` delegate to `create_by(NONE)`.
     3. Uncharges the creator and resets it in `maybe_free_record`.

     Result: test_endpoint PASS.
   - `syscall_ref.c`: a decoder copy with `case 15` returning a new action value.
     Result: test_syscall PASS, so every assertion after line 146 is consistent.
5. **Mutation check** (against the references, same flags with `-w`):
   - proc: 17 mutants, all killed. Targets: reason overwrite, release underflow,
     uncharge without the identity check, reap freeing with refs, alloc not zeroing,
     REF_MAX off-by-one, charge-limit off-by-one, NONE charging rec 0, abandon
     ignoring refs or charge, link in any state, retire wrapping (killed only by
     test_proc_retire), release freeing a LIVE record, NOMEM not writing NONE, reap
     keeping the run link, used_count not decremented, status accepting NULL.
   - create_by: 11 mutants; 10 killed. Targets: charge before NOSLOTS, no uncharge on
     mint failure, no uncharge on free, double uncharge, quota not enforced, invalid
     creator allowed, creator not stored, outs not zeroed, procs==NULL still
     charging, uncharge keyed on the slot instead of the identity. The survivor,
     "stale creator kept on free", is equivalent in my reference, because every
     successful create rewrites `creator`. The non-equivalent version (creator kept on
     free AND written only when charged) is **killed** by cases 10 and 13.

## Decisions (why + rejected alternative)
- **D1. Retirement lives in a separate TU** (`test_proc_retire.c`), as the brief directs.
  - Why: `JE_PROC_GEN_MAX` must also apply to proc.c.
  - Rejected: `#define` before the include in test_proc.c. That would leave proc.c
    at 2^32−1, so the test would be wrong.
  - Rejected: looping 2^32 times. Infeasible.
- **D2. "No change" means memcmp of the whole pool**, snapshotted with memcpy, not a
  check of a few fields.
  - Why: it catches stray writes to fields the test does not name.
  - Risk: padding bytes. Mitigated because snapshot and compare are both byte-level
    on the same object.
  - Rejected: per-field checks only. They miss unexpected writes.
- **D3. PROC_STATUS decode is pinned as "OK, not RETURN, distinct from every existing
  action"** rather than by an action name.
  - Why: `syscall.h` has no `JE_SYSCALL_PROC_STATUS` action, and that header is frozen
    for me.
  - Rejected: guessing the value 13 or 15. That would resolve a header gap by fiat.
  - **Lead:** tighten the assertion once the enum gains the action (see A1).
- **D4. A stale or never-issued, non-NONE creator is asserted fail-closed**:
  `create_by != OK`, no state change (cases 3 and 9). The exact status is NOT pinned.
  - Why: AGENTS fail-closed discipline. Accepting it would let a reaped identity
    create channels.
  - Rejected: pinning `JE_EP_INVALID`/`BADINDEX`. The header does not say.
  - This is the one place I lean on a reading the header does not state (A2). The
    lead may drop those two assertions if the majority reads it otherwise.
- **D5. On a full pool, an invalid creator gets NOSLOTS** (case 3). This is a literal
  reading of the order "1. find record → NOSLOTS; nothing charged; 2. charge".
- **D6. Transfer is modelled with the real cap-send path**: `try_send_caps` →
  `recv_peek_caps` → `capdesc_retain_handle` + `je_handle_alloc` into a second table
  → `recv_commit_caps`. It has an in-flight variant.
  - Why: this is how a handle actually moves between tables, so the charge is tested
    against the real refcounts.
  - Rejected: `je_handle_alloc`-ing a raw second handle without a ref. That breaks
    the endpoint refcount invariant.
- **D7. Case 12 compares `create` with `create_by(NONE)` on twin pools**, both
  memset-zeroed before init. It checks identical statuses, handles and `rec[]` bytes,
  and that the proc pool is untouched.
  - Found while testing: `je_endpoint_pool_init` does not scrub queue payload bytes,
    so a reused pool is not byte-equal to a fresh one. This was a test bug, fixed by
    memset; it is not an implementation defect.
- **D8. Added case 13 (not in the brief):** a freed record reused by plain create, or
  by another creator, must not carry the old creator, and must not decrement it.
  - Why: the brief's list does not cover the realistic bug where `creator` is not
    reset on free or on an uncharged create.
- **D9. Retirement timing is not pinned.** I assert only that after the next alloc the
  slot is retired, FREE and at gen==MAX. I do not check whether `retired` is set at
  free time or at alloc time. The header says "at alloc"; both are safe.
- **D10. resolve_any "upper bits" BADINDEX** is tested as high *index-field* bits, e.g.
  `encode(INDEX_MASK,1)` and `encode(1<<(INDEX_BITS-1),1)`. With INDEX+GEN = 32 bits,
  a `je_handle` has no bits above the fields; the wide-RDI case belongs to the decoder
  and is covered in test_syscall.
- **D11. Retain on a DEAD record is asserted legal** (DUP of a PROCESS handle to a dead
  child). This is the literal "valid id" reading: valid = non-FREE.

## Ambiguities found in the headers (flagged, not resolved)
- **A1. syscall.h has no `JE_SYSCALL_PROC_STATUS` action.** abi.h defines op 15, but
  `je_syscall_action` cannot express it. The impl worker or the lead must add it,
  which is a header change.
- **A2. `create_by` with a non-NONE creator that is invalid or stale:** the status is
  unspecified. The steps list only QUOTA (DENIED). What `je_proc_charge` returning
  INVALID maps to is open. My tests require only `!= OK` plus no change (D4).
- **A3. JE_EP_QUOTA is overloaded.** It means both "creator at creation quota" and
  "table live-slot quota" (mint failure). The caller cannot tell them apart. This is
  ABI-visible once ring-3 create is charged (H_QUOTA for both).
- **A4. `create_by` with `procs == NULL` and a non-NONE creator:** the header does not
  say whether `rec->creator` stores the creator or NONE. Not asserted.
- **A5. `je_proc_abandon` "unpublished"** is not a field. The tests only cover
  refs/charge/state. Abandon of a LIVE record with `run_linked` set is not asserted
  either way.
- **A6. `je_proc_on_reaped` "run link cleared":** the tests assert `run_linked ==
  false` only. They do not check whether `thread_slot`/`vm_index` are zeroed.
- **A7. NULL `pool` for every proc op except alloc** is unspecified. Not tested.
- **A8. `je_proc_charge` on DYING/DEAD** passes as a "valid id" under the literal
  reading, but a dead process creating channels makes no sense. Not asserted.
- **A9. The alloc NOMEM promise "nothing changes"** can conflict with "marked retired
  … the search continues". If the final search retires the last slot and then finds
  nothing, it both changes state and returns NOMEM. The tests only assert
  no-change on a *subsequent* NOMEM, once every slot is already retired.
- **A10. A retired slot's state** is the `FREE` value plus `retired=true`, and
  `used_count` is "records not FREE". I assert that retired slots are not counted in
  `used_count`. That is the literal reading; flagged in case "not FREE" was meant to
  include retired.
- **A11. Comment placement in handle.h:** the `resolve_any` comment sits between
  `je_handle_live_at`'s comment and its declaration, so `live_at`'s comment now
  floats above the wrong prototype. Cosmetic, but it is in a frozen header.
- **A12. `on_dying` does not validate `reason`** (e.g. > 3). Not tested.

## Not enforced / not covered
- No ring-3 or QEMU coverage. M4b-s1 belongs to the lead/impl.
- Typed dispatch (QUERY/RIGHTS/DUP/CLOSE through the type table) is not testable from
  these headers. It lives in the syscall glue (QEMU fixture).
- The `kern_*` paths (endpoint_kern.c) and the reaper ordering are not hostable here.
