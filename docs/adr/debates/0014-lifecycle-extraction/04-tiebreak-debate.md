================ PEER DEBATE (1 rounds) ================
TOPIC: Tie-break: RUNTIME-TABLE vs LINK-TIME-PORT for the ADR 0014 lifecycle extraction seam, given the white-box test include and the 6 mutant text anchors in sched.c
==============================================================

---------- ROUND 1 · CLAUDE ----------
**Position: link-time port, with the scope corrected below.** A runtime ops table can't keep 0 BLOCKED mutants and a byte-identical test body if it really moves the authority. I checked everything below against the tree at a88c585.

**Q1: does Codex's runtime table move the anchors or the test's reach? Yes, at least 4 of the 6 mutants.**
- **F08-ONE-THREAD and REAPER-VM-INDEX** both read `threads[]`. If a lifecycle table becomes the authority, these lines have to read that table instead, so their text changes.
- **REAPER-SIGN and REAPER-IF-CHECK** are in the reap loop, which moves into the module under an ops design. `abort_scheduler` is static to sched.c, so the IF-check line can't move unchanged.
- **CR3 and RSP0 guards** survive only if `reap_prepare` is hand-written to reproduce the exact lines.
- **The test** is stuck either way:
  - If the slot fields are removed, about 56 lines of direct internal access stop compiling.
  - If they're kept as a cache, the unchanged test compiles and quietly checks the copy that is no longer the authority. That's worse than BLOCKED.
  - The only ways out are not moving storage (which is the link-time port) or `#define` aliasing (breaks JCS-07).

**Corrections to the shared facts, including against my own side:**
1. **Fact 2 is overstated.** BLOCKED is not green: `mutants.py` exits 1 on any BLOCKED (mutants.py:402), and the CI `mutants` job runs it (ci.yml:24-37). It is invisible to `make check` (Makefile:107 is `check: all test`). The real risk is different: CI goes red, and the same author then re-anchors `mutants.json`, rewrites the killer test and moves the code in one PR. That makes the author certify their own test oracle, which the AGENTS.md different-author default warns against.
2. **My side's "registry plus 5 writer functions" is wrong.** `je_sched_has_process` and `je_sched_current_proc_id` read `threads[]` and `current`, not the registry, so they stay in sched.c. The registry has 3 writers plus a getter.
3. **Fact 1 hits my side too.** The test touches `registered_vm`, `registered_proc` and `process_pool` directly (lines 192-196, 395-404, 490, 530, 554, 563, 609). Fix: add one `#include "../kernel/core/lifecycle.c"` under the existing `#define static` and leave the body untouched. `git diff -U0` can verify that.
4. **The device hooks are compiled out of the hosted test** (sched.c:264, `#ifndef JE_SCHED_HOSTED_TEST`). So no hosted test checks where the device calls sit in the dying sequence, under either option. A call-order characterization test with device fakes has to land first, run against the unmodified sched.c.

**Why not the runtime table, even setting the anchors aside:**
- Each binary has exactly one run queue: x86 `SOURCES` links sched.c, `ARM_OBJS` links sched_rr.o. Link-time binding is enough.
- The tickets and atomic ops guard against interleaving that ADR 0014 D1 rules out (no SMP) and that REAPER-IF-CHECK already enforces (interrupts off, slot 0 only).
- Its second implementation, the ARM user path, doesn't exist until MP3, so the interface can't be verified now.
- **Concessions:** `je_rr_hooks` shows a validated runtime table is allowed here (JCS-07 permits it), so it's permitted, just unjustified. And Codex's per-slot table does cover legacy threads with `proc_id` NONE uniformly. But those threads have no PROCESS record, so the slot is their only copy and there's no dual ownership to fix.

**Single authority: satisfied.** ADR 0014 D2's own list of what the module owns is the registry, the kstack rule and the release rule. D2 says the IF/liveness guards must not be embedded in neutral code, so keeping them in sched.c complies.

For the `{vm, vm_index, proc_id}` triple, there is one writer: thread creation. `je_sched_kill` and `kill_all` already check the slot against `je_proc_run_link`; the reaper doesn't. Either add that check as a new line after the VM-INDEX anchor, in its own commit with its own mutant (owner-gated), or record it as NOT enforced.

ADR 0014 D2 needs two wording fixes before acceptance, both owner-gated:
- "sole waiter release" → "one release function, two idempotent call sites (dying and drop)" (fact 3).
- "arch seams the module calls" → "the per-arch run-queue caller invokes the module; the module never reads run-queue or arch state."

| MOVE to lifecycle.c (extern functions, no function pointers) | STAY in sched.c |
|---|---|
| Registry state and set_pool / register / unregister / lookup / getter; `je_sched_*` names kept as wrappers | `threads[]`, `current`, `ready`, death-record writes, `abort_scheduler` |
| `je_lc_release_waiter(slot)`, called from both dying and drop | `je_sched_has_process`, `je_sched_current_proc_id` |
| `je_lc_on_dying`: waiter release → device fault note (fault deaths only) → device dying → `je_proc_on_dying`; returns a status | The whole `reap_dead` loop, so all 5 reap anchors stay unchanged |
| `je_lc_kill_check`: the D8 checks that use only the PROCESS record; caller id passed in | Kill's IF check, slot check and rip/cs snapshot; both passes of `kill_all` |
| `je_lc_reaped`: `destroy_owned` → `on_reaped`; returns a status | `drop_user`'s slot scan, guard restore and clear; the F08 scan; frame build |

**Fact 4 (ARM kstack double-free):** the contract is right, but the mechanism should be an ownership tag set when the thread is created (kernel pool vs held by the VM), not a token. That mirrors x86 `je_sched_reap`, which only clears a user slot. Make it an MP3 entry criterion, not part of this PR: there's no ARM user path yet, and changing sched_rr.c (tested in #101) needs the owner. Pass/fail: a hosted `test_sched_rr` case reaps a VM-held context and sees 0 `stack_free` calls and exactly 1 VM destroy, and a mutant that deletes the skip is killed.

**Pass/fail for this PR:**
- The 6 sched.c entries in `mutants.json` are unchanged.
- `make mutants` reports 6/6 killed and 0 BLOCKED. It has to be run explicitly, because `make check` won't show it.
- The `test_sched_killall` diff is exactly one added include line.
- New lifecycle.c mutants are added, never re-anchored from sched.c.

---------- ROUND 1 · CODEX ----------
Claude is right on the decisive point: the proposed runtime per-slot authority cannot preserve both the six sched.c text anchors and the existing white-box reach. Moving `vm/vm_index` authority changes `REAPER-VM-INDEX` and `F08`; moving `reap_dead` changes the sign/IRQ anchors. Retaining dead compatibility text or mirrored state would preserve mutant counts without preserving semantic coverage—worse than a blocked mutant.

One refinement: a runtime table is not inherently incompatible if it is limited to neutral callbacks while the reap loop and all anchors remain in sched.c. But that is effectively the link-time-port boundary plus unnecessary indirection, not the proposed extraction.

I recommend `LINK-TIME-PORT`, with this boundary:

- Move: registry storage and operations; process-record-only kill checks; waiter release; ordered dying side effects; destroy/`on_reaped`.
- Stay: `threads[]`, `current`, `ready`, slot-cache handling, death-record writes, reap/kill loops, IRQ/CR3/RSP0/frame guards, `abort_scheduler`, F08 scan, and arch register logic.
- Treat `{vm, vm_index, proc_id}` in slots as a synchronized cache, never an independent lifecycle authority. Production writers must be explicit; direct white-box writes remain test setup only.
- Preserve all six mutant anchors unchanged and add `lifecycle.c` to the hosted test without changing its test body. Run `make mutants` explicitly; `make check` is insufficient.

That satisfies “single ownership boundary” if ADR 0014 means single ownership of neutral lifecycle state and side effects. It does not require lifecycle code to own decisions requiring run-queue or architectural state. If ADR wording demands all reap/kill decisions move, that wording conflicts with D2 and the tested-path constraints and needs owner escalation.

For ARM, record now—but implement in MP3—the contract that the run queue never frees a lifecycle-owned/user stack; lifecycle destruction uses an explicit architecture stack-ownership descriptor/token. MP3 must prove zero `stack_free` calls for a VM-owned stack and exactly one destroy, including a mutant that removes the skip.

================ END DEBATE ================
Orchestrator: read the above, relay any unresolved disagreement, and make
the final call. Verified compile/test/boot facts beat the debate; escalate
contract / tested-code / ADR changes to the human.
transcript saved: /tmp/peer-debates/debate-20261108-172758.md
