# Adversarial review — ADR 0014 lifecycle extraction (Codex-authored; you review)

You are the adversarial reviewer (different lineage). Codex implemented the
settled link-time-port extraction of the process lifecycle out of the
M1/M2/M3/M4-tested kernel/core/sched.c into kernel/core/lifecycle.c +
include/jeios/lifecycle.h. TRY TO BREAK IT. The worktree under review is the
current directory; the change is the single commit after `9c356e8` (`git show HEAD`).

## What the extraction was required to do (the settled design)
- Move ONLY: the registry state (process_pool, registered_vm[], registered_proc[])
  + its writers/getter (je_sched_set_proc_pool/register/unregister/proc_pool, kept
  under those public names); je_lc_release_waiter; je_lc_on_dying (the mark_dying
  side-effect ORDERING prefix: release -> device fault-note on fault deaths ->
  device dying -> je_proc_on_dying, returning status); je_lc_kill_check (the kill
  D8 process-record-only checks); je_lc_reaped (destroy_owned -> on_reaped).
- KEEP in sched.c byte-identical: the 6 mutant anchors (REAPER-SIGN/IF-CHECK/
  CR3-GUARD/RSP0-GUARD/VM-INDEX in reap_dead, F08-ONE-THREAD scan in
  je_thread_create_user_start), the whole reap_dead loop, kill/kill_all state
  gates + the IF (<<9) check + rip/cs snapshot, the kill_all two-pass loop, the
  frame synthesis, abort_scheduler, and all threads[]/CR3/RSP0/int-0x81/uframe code.
- Neutral module: no x86 tokens, must not include sched.h, must not loop threads[].
- Keep je_sched_* public names (wrappers). test_sched_killall.c body byte-identical
  except one added #include line. test_lifecycle_order.c (the pre-extraction
  characterization of mark_dying ordering) must still pass.

## Verified facts (lead ran these on HEAD)
- make check: PASS (test_sched_killall 259, test_lifecycle_order 50 — identical to
  pre-extraction counts).
- make mutants: 31 killed / 1 equivalent / 0 SURVIVED / 0 BLOCKED / 1 skipped; all
  6 sched.c mutants KILLED (so no anchored text moved).
- lifecycle.c: no forbidden x86 tokens; compiles clean with aarch64 freestanding
  flags (+ -DJE_ARCH_AARCH64).

## Attack these specifically
1. **Ordering preservation.** je_lc_on_dying must reproduce the EXACT original
   mark_dying order AND the exact conditions: (a) waiter release ALWAYS (even for a
   legacy gen==0 thread); (b) device fault-note ONLY on DEATH_FAULT (reason==0) AND
   only when proc_id.gen!=0; (c) device dying only when gen!=0; (d) je_proc_on_dying
   only when gen!=0, and its non-OK return must still cause sched.c to
   abort_scheduler. Does the new code match the original on EVERY branch —
   especially the legacy (gen==0) path (release only, skip all three hooks) and the
   fault vs non-fault reason split? The device hooks are under
   #ifndef JE_SCHED_HOSTED_TEST in BOTH files — is the guard correct so the hosted
   test_sched_killall (defines it) still compiles them out and test_lifecycle_order
   (does not) still sees them?
2. **Dual-ownership / stale state.** lifecycle.c is now the sole owner of the
   registry; sched.c reads it via je_lc_registered_vm/proc/proc_pool. Is there ANY
   remaining write to the registry in sched.c? Any place sched.c caches a registry
   value that could now be stale? Did je_sched_has_process / current_proc_id /
   find_process get correctly repointed (find_process was converted; has_process
   and current_proc_id read threads[] only — confirm they needed no registry access)?
3. **je_lc_reaped failure semantics.** Original: destroy_owned !=OK ->
   abort_scheduler; on_reaped !=OK -> abort_scheduler. New: je_lc_reaped returns a
   status and sched.c does `if (s!=JE_OK) abort_scheduler();`. But je_lc_reaped
   returns destroy's status on destroy-failure AND on_reaped's status on
   on_reaped-failure — so BOTH map to abort_scheduler. Is that equivalent? Could a
   destroy that returns JE_OK but a non-gen proc change behavior? Check the gen==0
   branch (no on_reaped) still returns JE_OK.
4. **The include mechanism.** The worker used TWO mechanisms: test_sched_killall.c
   got an explicit `#include "../kernel/core/lifecycle.c"`, while test_lifecycle_order
   builds with -DJE_SCHED_TEST_INCLUDE_LIFECYCLE which triggers a NEW
   `#if defined(JE_SCHED_TEST_INCLUDE_LIFECYCLE) #include "lifecycle.c"` at the top
   of PRODUCTION sched.c. Is the conditional include in production sched.c
   acceptable, or should both tests use the same explicit-include-in-test approach
   and sched.c stay free of it? Is there any build where lifecycle.c is BOTH
   separately compiled AND #included (double definition / ODR)? Check the kernel
   SOURCES link (lifecycle.c added) vs the two hosted tests.
5. **kill_all** still loops threads[] and calls je_proc_run_link/status via
   je_lc_proc_pool() — confirm no behavior change vs calling process_pool directly,
   and that the two-pass validate/mutate invariant is intact.
6. Anything else: a moved function whose error path or argument order subtly
   differs from the original; a missing `static`; a header cycle; a symbol now
   exported that should be internal.

## Output
BLOCKER / SHOULD-FIX / NIT, each with file:line and the exact scenario. For the
ordering (item 1) state explicitly whether je_lc_on_dying is branch-for-branch
equivalent to the original mark_dying prefix. End with SAFE TO INTEGRATE / FIX FIRST.
