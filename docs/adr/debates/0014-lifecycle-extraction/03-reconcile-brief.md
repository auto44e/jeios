# Reconcile one conflict — ADR 0014 extraction seam shape (link-time port vs runtime ops table)

Two independent design reviews of the JeiOS ADR 0014 process-lifecycle extraction
agree on almost everything (keep the x86 run queue in sched.c; keep the je_sched_*
names as wrappers; land characterization tests first). They CONFLICT on the seam shape.
Settle it. Prefer the option that is VERIFIABLY lower-risk for a tested-path refactor.

## Verified facts (I checked these in the tree, core main a88c585)
1. `tests/test_sched_killall.c` does `#define static` + `#include "../kernel/core/sched.c"`
   and reads/writes `threads[]`, `registered_vm/proc`, `process_pool`, `current`, `ready`
   DIRECTLY. It is a white-box test of sched.c internals.
2. The six sched.c mutants in tools/mutants.json match EXACT TEXT inside reap_dead /
   je_sched_reap_dead / je_thread_create_user_start:
   - REAPER-SIGN: `if (s!=JE_OK) return -(int)s;`
   - REAPER-IF-CHECK: `if (je_arch_irqs_enabled()) abort_scheduler();`
   - REAPER-CR3-GUARD: `if (threads[i].root==active_cr3) return -(int)JE_ERR_BUSY;`
   - REAPER-RSP0-GUARD: `if (threads[i].rsp0_top==active_rsp0) return -(int)JE_ERR_BUSY;`
   - REAPER-VM-INDEX: `je_vm *vm=threads[i].vm; uint32_t idx=threads[i].vm_index;`
   - F08-ONE-THREAD: the one-thread-per-vm scan in je_thread_create_user_start.
   If that text moves out of sched.c, the mutant goes BLOCKED, which mutants.py treats
   as "never a kill" — silent coverage loss with every gate still green.
3. `je_sched_drop_user` (sched.c:~582) ALSO calls the waiter-release hook, so
   "mark_dying is the SOLE release owner" is literally false; kill-then-reap releases
   twice (2nd is a no-op). drop is the only release when a live/BLOCKED_IPC thread is
   torn down by spawn/harness.
4. The ARM run queue `je_rr_reap` (sched_rr.c:~224) ALWAYS calls stack_free, and ARM
   stack_free unmaps+frees — so a naive ARM reuse re-introduces the exact user-kstack
   double-free the "reap never frees a user kstack" rule prevents.

## The conflict
- **Codex** wants state ownership MOVED into a lifecycle-owned per-slot table + a
  runtime seam of atomic ops (snapshot_slot, reap_prepare(ticket), capture_death,
  make_unschedulable, clear_wait, restore_guard, clear_slot). Goal: no dual ownership
  of lifecycle state; the module is the single authority.
- **Claude** says DON'T build a runtime ops table: it would move the mutant-targeted
  text and the white-box test's reach out of sched.c (fact 1+2). Instead do a minimal
  LINK-TIME port: move ONLY (a) the registry state + its 5 writer functions, (b) the
  je_lc_on_dying side-effect ORDERING prefix (waiter-release -> device hooks ->
  je_proc_on_dying), (c) decisions made from the PROCESS RECORD alone (kill D8 steps
  1-5, drop ordering, destroy->on_reaped). Everything that reads run-queue state / the
  frame / arch registers / loops over threads[] STAYS in sched.c. Neutral funcs return
  a status; sched.c keeps calling abort_scheduler. The seam is a link-time port of ~3
  functions, swappable by the hosted tests exactly as sched.c already is.

Both agree the `threads[].{vm,vm_index,proc_id}` triple must have ONE authority
(the registry/proc-record), with the slot copy demoted to a run-queue cache.

## Question for each of you (one short round; the OTHER model's position is above)
Given facts 1-2 specifically: does Codex's runtime per-slot-table + atomic-ops seam
move the mutant-targeted text or the white-box test's reach out of sched.c? If yes,
how do you keep 0 BLOCKED mutants and a byte-identical test body while still achieving
single-authority? If the link-time-port scope achieves single-authority for the
REGISTRY (the only truly neutral state) while leaving reap/kill GUARDS in sched.c behind
the mutants, is that sufficient for ADR 0014's "single ownership boundary", or does
leaving reap/kill decision+guard in sched.c violate it?

Also: for the ARM reuse (fact 4), is the right contract "the run queue must NOT free a
user-thread stack; the lifecycle module drives destroy via an arch stack-ownership
token" — and does that belong in THIS PR or as a recorded MP3 entry-criterion (ARM has
no user path yet)?

Give a crisp final recommendation: RUNTIME-TABLE or LINK-TIME-PORT (or a named hybrid),
the exact set of things that MOVE vs STAY, and whether single-authority is satisfied.
Keep it short — this is a tie-break, not a re-review.
