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

## Final round — Claude (core-code verification + first review of the QEMU fixture)

**Round-1 status:** all six P1/P2 items are fixed, and P3-6 is only partly fixed. **Fixture:** the startup-v1 and baseline claims are only partly proven. There are no P1s, three P2s and a few P3s. This is an AI review, read-only; I did not run `make`.

### Round-1 findings
1. **P1-1: FIXED.** `spawn.c:85-96` runs `drop_user` (which also unregisters, `sched.c:553`), then `vm_destroy`, then `on_dying(KILLED)`, then `on_reaped`. `on_reaped` clears `run_linked` and frees the record because `handle_refs==0` (`proc.c:111-120`). The failed mint has already released its retain (`syscall.c:331`). The ordering matches `je_sched_reap_dead` (`sched.c:585-591`).
2. **P2-1: FIXED.** The check is at `spawn.c:141`, after the default is applied and before any allocation. The precedence is pinned in `spawn.h:10-12`.
3. **P2-2: FIXED.** The hook is disarmed on every return via `spawn_finish` (`spawn.c:28-32`), and out-of-range arming is ignored (`:45`). It stays compiled into the normal kernel, which is a recorded deviation (`spawn.h:48-51`).
4. **P3-1: FIXED.** `syscall.c:137-138` now uses the macros, and the dead clause is gone.
5. **P3-2: FIXED.** `spawn.c:183` goes through `spawn_undo`, so the abandon result is checked.
6. **P3-3: FIXED.** The unexpected-status paths now call `syscall_invariant` (`spawn.c:64-66,76-77,131,160`). I checked that the resource paths (thread table full, VM slots, pages) return NOMEM/BUSY, not INVALID, so ring 3 cannot use these paths to halt the kernel.
7. **P3-4: FIXED.** `spawn.c:228-235` maps QUOTA to `H_QUOTA`. `je_process_handle_mint` still turns QUOTA into `JE_ERR_DENIED` internally (`syscall.c:331`), but spawn no longer depends on that.
8. **P3-5: FIXED.** The launcher is looked up before the grant copy (`syscall.c:759-768`). `je_proc_spawn` repeats the lookup; that is redundant but consistent.
9. **P3-6: PARTLY FIXED.** The fixture audits the child (`main.c:1374`), but only after the child became runnable and possibly ran. The spawn path itself still has no audit. Record this as a deviation.

### Fixture findings
**P2-A. The quota-invalid baseline is taken at an unsynchronised point (TOCTOU).**
- `main.c:1419-1425` takes the baseline with IF=1, before the kernel has seen phase 9. The parent is still running phases 5–8, including dup→close pairs.
- If the parent is preempted between a dup and its close, `parent_live` is off by one and the run fails spuriously.
- The baseline also covers the phase 6/7 negative spawns or not, depending on timing.
- The comment at `:1429` says the baseline is taken with IF=0, which the code does not do.
- **Fix:** take every baseline inside the `quota_phase == 9u` IRQ-save block, then write the ack.

**P2-B. The fail-Nth matrix runs while the parent is still active.**
- The kernel acks with `0x1b20=2` at `:1489`, then enters the matrix (`:1500`) without waiting for the parent to reach its hold loop. The parent hold loop is `0x1b00=1`, phase 8 (`user_demo.S:566`).
- In that window the parent creates a SELF handle and closes it (`user_demo.S:520-538`), so `parent_live` can change during the matrix and cause a spurious failure.
- The block comment at `:1342` says the parent stays at progress 1 during the matrix; nothing enforces that.
- **Fix:** wait (bounded) for `0x1b00==1 && 0x1b10==8`, then re-take the baseline under IF=0 before step 1.

**P2-C. The marker overclaims the startup registers and negative baselines.**
- **Zero-fill:**
  - The child checks only R8 and R9 (`user_demo.S:604-607`).
  - The parent checks only RDI, RSI and RDX.
  - Nothing checks RAX, RBX, RBP or R10–R15, or the parent's RCX, R8 and R9. `abi.h:232` promises "every other GPR zero".
  - **Fix:** at each entry, OR the other GPRs together and assert the result is zero.
- **Order and attenuation: proven.** The child sees rights 3 on RDX and 7 on RCX.
- **Negative spawns:**
  - Only the quota-invalid call checks `RDX==0` and has a baseline.
  - The other six have status-only checks: bad image, no TRANSFER, superset, count 5, endpoint quota 1, SELF launcher.
  - There are no BADINDEX/STALE/DENIED launcher cases, and no caller-table `H_QUOTA` case.
  - **Fix:** check `RDX==0` on every negative, and bracket the phase 6–7 calls with a synchronised baseline. Otherwise narrow the marker.

**P3 findings:**
1. **Child ack race.** The child zeroes `0x1b08` at entry (`:600`). The kernel can ack (`main.c:1380`) before the child's first instruction runs, so the ack is erased. The failure is bounded by the 200-tick reap timeout, so this is a flake, not a false pass. Also, the `0x1830==99` check at audit time is mostly vacuous; the real proof is the parent seeing exit code 5. **Fix:** drop the child's `movq $0,0x1b08` (data is already zero), or ack only after the child reports `0x1b00==1`.
2. **Quota-invalid probe uses a stale grant.** It reuses `0x1900`, which now holds a closed handle (`:504`). Status 2 still tells the outcomes apart, but the "no VM built" path is not exercised. **Fix:** restore valid grants first.
3. **"spawn ticks" is always about 0.** The whole measured spawn runs with IF=0 (`main.c:1543-1547`), so ticks cannot advance. **Fix:** label it as a diagnostic or measure it with TSC. Separately, the unconditional `sti` at `:1540` overrides the saved IRQ state.
4. **Measured close accepts STALE.** `:1585` tolerates STALE, which would hide a premature free, and then releases twice. **Fix:** require OK and release once.
5. **Fail-Nth does not prove the failpoint fired.** A real NOSLOTS before the step would pass. **Fix:** add a fired counter.
6. **Per-endpoint refs are not in the matrix baseline.** A leak is caught only by the final `allocated==0` check (`:1602`), so it isn't attributed to a step. **Fix:** add the endpoint ref counts to the matrix baseline.
7. **CLOCK_TICKS:** monotonicity is shown by two samples with an unsigned `jb` (non-decreasing). That is acceptable, but ring 3 never tests the rejection of non-zero arguments.
8. **Scratch overlap:** the status output at `0x1a10` sits inside the 64-byte receive buffer at `0x1a00`. It is harmless because the two are used one after the other. The parent and child scratch areas are separate VMs, so they do not overlap.

### Confirmed sound
- **Bounded waits:** every wait is bounded. Kernel loops time out at 200 ticks with a phase dump; ring-3 spins use `ecx` counters, and the status loop is capped at 0x01000000.
- **Progress reads:** the progress, death and record scans are each read under a single IRQ-save (the PR-B fix still holds).
- **LAUNCHER:**
  - The startup RDX is non-zero and its QUERY returns type 4.
  - DUP and RIGHTS on it return DENIED.
  - A SELF handle passed as the launcher returns WRONGTYPE.
- **Delivery and attenuation:**
  - The child's byte (0x5a) arrives through grant 0.
  - The rights are 3 and 7, with TRANSFER stripped.
  - Using a right the child lacks returns DENIED.
  - The exit is code 5.
- **Fail-Nth coverage:** steps 1–5, plus step 3 at grant 0 and grant 1, run under IF=0. Each checks the status and `out==0` and compares pages, endpoints, records, the parent's handle count, threads and VM count.
- **Final check:** after the parent is reaped, the pool is back to zero.

## Final round — Codex (test verification + spawn mutants reasoned against the fixture)

1. **P2 — FIXED** — `tests/test_uframe.c:25-29, 41-88`. Sentinel checks now cover every invalid-frame path; `out_rflags` preservation is asserted.

2. **P2 — FIXED** — `kernel/arch/x86_64/user_demo.S:591-612`; `kernel/core/main.c:1375-1382`. The child validates startup version/count, zero-filled R8/R9, handle order, and attenuated rights; the kernel audits before release.

3. **P3 — FIXED** — `include/jeios/abi.h:154-169`; `tests/test_syscall.c:126-142`. The decode reject list is explicitly exhaustive and the tests distinguish decode from glue/spawn validation.

Mutant audit, reasoned from `main.c` and `tools/smoke.py` (no QEMU run):

4. **P1 — TRANSFER gate removed** — gate is helper-owned at `kernel/core/endpoint_kern.c:199-217`, not `spawn.c`. The fixture would **FAIL**: `user_demo.S:411-438` and `454-466` expect `DENIED` for a no-TRANSFER source. Fix: retain the helper gate and add a direct negative regression if refactored.

5. **P1 — minted rights become source/all rights** (`/tmp/.../spawn.c.source-rights`) — **FAIL**. Child `user_demo.S:614-668` checks exact rights `SEND` and `SEND|RECV|INSPECT`; source/all rights violate attenuation. Fix: preserve descriptor requested rights at `spawn.c:148-152`.

6. **P2 — `spawn_finish()` no longer disarms failpoints** — **NOT CATCHED**. Every fixture arming fires, `spawn_failpoint_hit()` self-clears, and `main.c:1541` explicitly disarms before the measured spawn. Fix: arm step 3 with `grant_index >= n`, then perform a real spawn and assert success.

7. **P1 — skip `je_vm_destroy` on step-3 failure** — **FAIL**. `main.c:1520-1537` checks page count and VM-slot baseline after both grant failures. Fix: retain `spawn_undo()`’s VM destruction at `spawn.c:101-103`.

8. **P2 — published PROCESS handle lacks KILL** — **NOT CATCHED**. `main.c:1549-1586` checks handle existence/lifetime, then kernel-reaps directly; it never queries returned rights. Fix: query the measured handle and require `INSPECT|KILL`.

9. **P2 — move `n > quota` after VM construction** — **NOT CATCHED** if rollback remains correct. The fixture checks only post-call baselines, not that validation preceded allocation. Fix: arm an early failpoint during the quota-invalid probe or add an allocation-order observation.

**confirmed sound**

No repository edits or `make`/QEMU execution performed. AI review, not human qualification.
## Lead disposition

- **All of Claude's core round-1 items are FIXED.** P3-6 (no audit inside spawn) is recorded as a deliberate deviation: spawn runs at IF=0 and the audit is O(N²·pages), so the fixture audits the child afterwards.
- **Fixture findings (Claude P2-A/B/C and P3s), plus Codex's three uncaught mutants (unfired-failpoint disarm, published handle rights, validation order): all accepted and sent to Codex round 4.** The flake risks are fixed by synchronised IF=0 baselines. The marker is narrowed to exactly what is proven.

## Verification round — Claude (fixture after round 4)

**Verdict:** 12 of 14 items are fixed. P3-8 is unchanged (it was harmless), and Codex #6 is still not caught. I found one new P2: a race in the phase-6 handshake that can fail a correct run but never pass a broken one. No new P1s. This is an AI review, read-only; I did not run `make` or QEMU.

### My previous findings
- **P2-A: FIXED.** Each baseline is now taken with IF=0 at the exact phase point:
  - `main.c:1575-1580` and `:1599-1606` bracket the phase 6/7 negative spawns.
  - `main.c:1631-1637` takes the phase-9 baseline before the ack.
- **P2-B: FIXED.** The kernel waits (bounded) for `phase==8 && hold==1` under IF=0 (`main.c:1683-1690`). IF stays 0 through the matrix, both unfired-failpoint probes and the measured spawn, and is only restored at `:1769`.
- **P2-C: FIXED.**
  - Zero-fill: the full GPR OR-check is at `user_demo.S:305-318` for the parent and `:705-718` for the child. R11 is included, so all non-argument GPRs are covered.
  - Negatives: every negative spawn now checks `RDX==0`. STALE (`:579-592`) and BADINDEX (`:594-604`) cases were added, and SELF moved inside the bracket.
  - Still missing: a caller-table `H_QUOTA` case and a launcher that lacks SPAWN (unreachable from ring 3). The marker doesn't claim either.
  - The "stale launcher" is really a closed endpoint DUP. It tests STALE-first precedence, not a stale LAUNCHER object.
- **P3-1: FIXED.** The child's `0x1b08` zeroing is gone. The `0x1830==99` audit check is still mostly vacuous: the audit runs once `1b18!=0` (`main.c:1534`), so the real proof is still the parent seeing exit code 5.
- **P3-2: FIXED.** Valid grants are restored before the probe (`user_demo.S:611-616`).
- **P3-3: FIXED.** The measurement now uses `lfence; rdtsc` (`main.c:374-391`), and the unconditional `sti` was replaced by `irq_restore`.
- **P3-4: FIXED.** The close must return OK and the record is released once (`main.c:420-423`).
- **P3-5: FIXED.** Each matrix case checks `fired == 1` (`main.c:1707,1718`).
- **P3-6: FIXED.** The endpoint `handle_refs` and `inflight_refs` are part of the baseline (`main.c:297-298,310-331`).
- **P3-7: FIXED.** A non-zero CLOCK_TICKS argument must return INVALID with `RDX==0` (`user_demo.S:378-389`).
- **P3-8: NOT FIXED.** The `0x1a10`/`0x1a00` overlap is unchanged; it is harmless. Scratch reuse of `0x1898`, `0x18b0`, `0x18c0` and `0x18d0` is likewise sequential and harmless.

### Codex's uncaught mutants
- **#6 (`spawn_finish` no longer disarms): NOT FIXED.** The fix only proves the failpoint did not fire. It never checks that the failpoint is disarmed afterwards:
  - The `(3,2)` arm at `main.c:1724` can never fire when n=2.
  - Every later spawn is preceded by a `set()` (`:1736`, `:1747`, matrix), which disarms or re-arms anyway.
  - The quota probe's leftover `(1,0)` arm is likewise overwritten by the matrix.
  - **Fix:** after the phase-10 quota probe, or after an unfired arm, do one direct spawn *without* calling `set()` and require OK with `fired==0`.
- **#8 (PROCESS handle rights): FIXED.** `main.c:1756-1762` requires exactly `INSPECT|KILL`. `je_proc_spawn` mints the handle internally, so the syscall path is covered too.
- **#9 (validation order): FIXED.** Step 1 is armed before the INVALID probe (`main.c:1634`), and `main.c:1654-1662` requires INVALID, `fired==0` and an unchanged baseline. If the check moved after VM construction, step 1 (`spawn.c:174`) would fire and return NOSLOTS, so the mutant is caught.

### New finding
**P2: lost-ack race at phase 6 (TOCTOU).**
- `user_demo.S:438-439` stores `1b10=6` and then `1b20=0`.
- If the timer preempts the parent between those two stores, the kernel sees phase 6, takes the baseline and acks `1b20=1` (`main.c:1578`).
- The parent then resumes and overwrites the ack with 0.
- The run then fails spuriously: the parent's ring-3 spin (bounded at `0x7fffffff`) gives up, or the kernel's phase-7 wait times out after 200 ticks (`main.c:1622`).
- It cannot produce a false pass. The store is redundant, because `1b20` is already zeroed at `:324`.
- **Fix:** delete `:439`, or move it before the phase store.

### Checked and sound
- Every new wait is bounded: kernel waits use 200 ticks, ring-3 waits use `ecx` counters.
- The phase-8 path writes `1b20=0` before `phase=8` / `hold=1`, so that handshake has no race.
- Each part of the marker (`smoke.py:26`) is backed by a check, except the unchecked disarm (#6), which it doesn't name.

## Lead disposition (lead-authored fixes, `53ab125`)

- **Phase-6 lost-ack race (P2): fixed by the lead.** The redundant store that cleared 1b20 after phase 6 was published is removed, because a timer tick between the two stores could overwrite the kernel's ack.
- **Codex mutant #6 (end-of-call disarm removed) was still uncaught: fixed by the lead.** The new probe arms grant index 1, spawns with 1 grant (so it cannot fire), then spawns with 2 grants without re-arming.
- **Mutant actually run.** The lead built the mutant in a scratch worktree. The first attempt booted a **stale ISO**: `make all` does not rebuild `build/0/jeios.iso`, so the mutant appeared to survive. Re-run through `make smoke`, which rebuilds it, the mutant fails as intended with `JeiOS fatal: M4c failpoint not disarmed after spawn`. Lesson recorded: mutant and profile runs must go through `make smoke` (or rebuild the ISO) before calling `tools/smoke.py` directly.
- **Left as is:** P3-8 (sequential scratch reuse, harmless). Two cases are unreachable from ring 3 and the marker does not claim them: a launcher without SPAWN, and a caller-table H_QUOTA. The "stale launcher" negative is a closed endpoint handle, so it tests STALE-first precedence, not a stale LAUNCHER object.
- **Not peer-reviewed:** these last two lead edits. They were verified by the full sweep plus the mutant run.
