# M4 PR-C reviews (2026-10-31, AI-assisted; not human qualification)

Worker mode: Codex wrote the implementation and Claude reviewed its core code. Claude wrote the tests from headers only and Codex reviewed them with mutants. Lead: Kiro. Reviewer outputs are verbatim.

## Round 1 — Claude reviewing the Codex core code (spawn/image/handle/sched/syscall; fixture excluded)

**P1: one confirmed defect. Any failure at journal step 5 halts the kernel instead of rolling back.** Below that: two P2s, then P3 hygiene. This is a read-only static review by an AI model, not human qualification. I did not run `make`.

### Findings

**P1-1. Rollback after the thread is created always halts.** `kernel/core/spawn.c:77-87` with `kernel/core/proc.c:75-82`.
- `je_thread_create_user_start` calls `je_proc_link` (`sched.c:~215`), which sets `rec->run_linked = true`.
- `spawn_undo` then calls `je_sched_drop_user` and `je_vm_destroy`, but nothing clears `run_linked`. `je_proc_abandon` rejects `run_linked` with `JE_ERR_INVALID`, so the undo reaches `je_arch_halt()`.
- Two paths reach this:
  - failpoint step 5, at `spawn.c:196`;
  - any `je_process_handle_mint` failure at `spawn.c:204`. One concrete case is `je_endpoint_kern_is_wired()` being false. Another is a broken preflight.
- Because a halt looks like a hang, this is a plausible cause of the r1 "hangs" note, but I haven't confirmed that.
- **Fix:** add `je_proc_unlink(pool, id)` to `proc.c`. It should require LIVE and `run_linked`, clear `run_linked`, `thread_slot` and `vm_index`, and be called in `spawn_undo` right after `drop_user` succeeds. Add a hosted proc test for link→unlink→abandon, and make the step-5 failpoint test assert that the pools are back to baseline.

**P2-1. A child handle quota below the grant count is caught late, with an ambiguous status.** `spawn.c:119-121` against `:173-177`.
- With `handle_quota = 1` and `n = 4`, the code builds a full VM, mints, then fails with `H_QUOTA` and unwinds, all with IF=0.
- `H_QUOTA` is also the status for the caller-table preflight at step 5, so the caller can't tell its own quota from the child's.
- ADR D5 says inputs are validated before state changes.
- **Fix:** in step 3, return `INVALID` when `n > handle_quota` (after the default is applied). Add this to spawn.h's precedence list.

**P2-2. A step-3 failpoint can stay armed and break a later real spawn.** `spawn.c:26-33`.
- If `grant_index >= n` (or `n = 0`), the step-3 failpoint never fires and never disarms. The next real spawn with more grants then fails spuriously with `H_NOSLOTS`.
- The hook is not reachable from ring 3: no syscall path calls `je_spawn_failpoint_set`. However, it is compiled into the production kernel.
- **Fix:** disarm at the end of every `je_proc_spawn` call, or reject `grant_index >= JE_ABI_SPAWN_GRANTS_MAX` in `je_spawn_failpoint_set`. Also put the hook behind a test-only build flag.

**P3 findings:**
1. `syscall.c:31-32,140-141`: the hard-coded `39u` and `0u` duplicate `JE_VM_HANDLE_QUOTA_MAX` and `JE_EP_CHILD_QUOTA_MAX`, so they can drift. Use the macros, or `_Static_assert` that they are equal. `handle_quota != 0u && handle_quota < 1u` is dead code; remove it.
2. `spawn.c:155-157`: when VM build fails, the `je_proc_abandon` result is ignored with `(void)`. Every other undo path halts on failure. Use `spawn_undo` here for one consistent journal.
3. `spawn.c:56,69`: unexpected statuses get softened instead of stopping the kernel.
   - `JE_EP_CORRUPT` maps to `UNSUPPORTED`.
   - Unknown internal statuses map to `INVALID`.
   - Pool corruption should go to `syscall_invariant`, not be returned to ring 3 as a normal status.
4. `syscall.c:327-328`: `je_process_handle_mint` turns handle `QUOTA` into `JE_ERR_DENIED`, which spawn then reports as `H_DENIED`. The preflight makes this unreachable today, but if it ever triggers it reports the wrong status (`H_DENIED` instead of `H_QUOTA`), and after P1-1 it also halts. Map `QUOTA` directly.
5. `syscall.c:760-766`: the grants are copied in before the launcher is checked. A caller with no authority and a bad pointer gets `INVALID`, where `BADINDEX`/`DENIED` would be expected. send-caps does the reverse: it preflights the main handle first (`syscall.c:793`). The ADR allows either order, but spawn.h's comment implies the launcher is checked first. Either resolve the launcher before the copy, or document the copy-first order.
6. No `je_vm_audit` runs before the spawned child becomes runnable, whereas the M3 fixtures audit after build (`main.c:104,197`). ADR 0010:148 notes the audit costs O(N²·pages), so with IF=0 this is a deliberate trade-off. Record it as a recorded deviation, or add the audit to debug builds.

### Confirmed sound
- **Authority:** grants go only through `resolve_send_cap`. It requires ENDPOINT type plus TRANSFER and checks rights are a subset, so SELF, PROCESS and LAUNCHER handles return `WRONGTYPE`. The child can never receive the LAUNCHER or its own PROCESS handle, and the caller gets exactly INSPECT|KILL. LAUNCHER has DUP set to DENIED, and its only known right is SPAWN.
- **TOCTOU:** the grants are copied once into a kernel array, and only the kernel copy is used afterwards. Between resolve and mint the code runs with IF=0 on one CPU, and nothing in between touches the caller's table, so the `can_alloc` preflight holds.
- **Rollback, steps 1-4:**
  - The record is freed.
  - The VM is destroyed, and the release walk drops each minted grant ref. The caller still holds a ref, so no end transitions and no spurious PEER_CLOSED wake fires.
  - Registration is undone.
  - A failed thread create restores the guard page and clears the thread slot.
  - The grant mint itself undoes its retain if the alloc fails.
- **Startup registers:** the frame is zeroed first, then RDI, RSI and RDX–R9 are set from zero-extended u32 handles. RFLAGS is 0x202, and `je_uframe_check` still sanitizes on every dispatch. No kernel value leaks.
- **Decode and glue:** wide RDI/RSI, R9≠0, quotas [63:32], count > 4 and an RDX/count mismatch all return `INVALID`. A copy fault returns `INVALID`, matching the other ops. `CLOCK_TICKS` requires all argument registers zero and reads the tick counter, which is a single 64-bit read and atomic on one CPU.
- **`je_handle_can_alloc`:** shares the same preflight code as `alloc`, so QUOTA still takes precedence over NOSLOTS. It is read-only.
- **NOMEM/BUSY → `H_NOSLOTS`:** this follows spawn.h.

## Round 1 — Codex reviewing the Claude tests (with mutants)

AI review, not human. No P1 findings.

1. **P2 — invalid-frame output contract is untested.**
   `uframe.h` requires `*out_rflags` remain untouched on failure, but [test_uframe.c:74-77](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/tests/test_uframe.c:74) only checks the boolean. A mutant that writes `*out_rflags = 0` before rejecting `size == 0` survives.
   Fix: initialize a sentinel and assert it remains unchanged for every invalid case.

2. **P2 — startup ABI tests validate only the validator, not construction.**
   [test_uframe.c:79-121](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/tests/test_uframe.c:79) manually builds the expected frame. A wrong `sched.c`/spawn register layout, failure to zero registers beyond `n`, or wrong handle order survives. The author also records no hosted coverage for spawn/rollback.
   Fix: add a constructor seam/fake or kernel fixture test covering exact registers, zero-fill, grant order, and rollback baselines.

3. **P3 — semantic SPAWN “OK” cases rely on an unstated decoder/glue boundary.**
   [test_syscall.c:126-132](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/tests/test_syscall.c:126) requires `RDI=0`, unknown `image_id`, and an all-ones grants pointer to decode successfully. This is consistent with ADR D5’s glue/copy-fault wording, but `abi.h` does not explicitly say the listed decode rejects are exhaustive.
   Fix: clarify in `abi.h` that launcher/image validity and pointer validity are glue-level checks, or avoid pinning these values as decode-OK.

Mutation results:

- Handle mutants: quota precedence, quota off-by-one, RETIRED-as-FREE, and mutating `can_alloc` were killed.
- Syscall mutants: all 12 requested examples were killed.
- A wrapped-range guard removal in `uframe.c` survived, but is behaviorally redundant because later bounds comparisons still reject the wrapped interval.

Confirmed sound: baseline hosted binaries passed; `git diff --check` passed; current commit is `dc5bf9f`.
## Lead disposition

- **Round-1 fixture hang.** Codex worker r2 found the root cause: the parent's release scratch word was never written, so the parent waited forever. The deeper defect is that the wait was unbounded and gave no diagnostic. All M4c waits are now bounded and fail loudly.
- **"D3 RIGHTS ambiguity" was the lead's brief error, not the ADR's.** The brief asked the child to call RIGHTS on handles with no INSPECT. The fix: grants carry INSPECT, and attenuation is also proven behaviourally (RECV on the SEND-only end gives DENIED).
- **Claude P1-1 (rollback after the thread exists halted the kernel): accepted.** The Codex worker found and fixed the same bug independently in round 2, by tearing down through `on_dying(KILLED)` then `on_reaped`, the existing pinned kernel-forced teardown rule. That rule is now pinned in spawn.h as well. Rejected: Claude's alternative, a new `je_proc_unlink` API, because it widens the set of record mutators for no gain.
- **Claude P2-1 (child quota smaller than n caught late) and P2-2 (failpoint could stay armed): accepted and pinned in spawn.h.** All Claude P3s were also accepted.
- **Codex test review:**
  - P2-1 (uframe sentinel) accepted.
  - P2-2 (construction unproven on the host) accepted. The proof is the QEMU fixture, where the child asserts its own startup registers: zero-fill beyond n, and grant order checked by RIGHTS.
  - P3 accepted: abi.h now says the decode reject list is exhaustive.
