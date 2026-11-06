================ PEER DEBATE (2 rounds) ================
TOPIC: JeiOS audit judgment-call debate (Q1 sequencing/parity, Q2 SMP, Q3 neutral-core migration, Q4 mutation-in-CI, Q5 human-gate boundary)
==============================================================

---------- ROUND 1 · CLAUDE ----------
# Round 1 (Claude Code)

I checked a few things in the tree before writing this. `kernel/core/sched.c` is 853 lines, and lines 58–66 hold its file-scope mutable state: `threads[]`, `process_pool`, `registered_vm[]`, `registered_proc[]`, `current`, `ready`, `preemptions`, `user_cpl3_dispatches` and a `volatile je_timer_ticks`. `main.c` is 3874 lines. ADR 0013 D1 says x86 `sched.c` is "UNTOUCHED this PR" and the "migration is a later owner-approved PR", with no trigger and no deadline. `test_image` already commits hand-written mutants that must fail.

## Q1. Sequencing: parity should apply to ABI work, not to milestone numbers

Making all of MP3 a precondition for every x86 gate past M5a is too blunt. Letting x86 race ahead to M7 is worse. The real constraint is ABI surface.

- MP3 is essentially ADR 0007/0008/0009 (user boundary, typed handles, capability IPC) re-run on a second ISA. That is where x86 assumptions in the syscall ABI, `uframe`, usercopy and handle encoding would show up.
- Every syscall, handle type or IPC message shape that x86 adds before that re-run makes the eventual fix more expensive.
- **Rule I propose:** x86 may proceed past M5a, but no new user-visible ABI gets frozen until the existing M3 ABI runs on ARM. x86 work that adds no ABI (drivers behind existing handles, internal storage-stack work) is not blocked.
- **Breakeven:** the first PR that wants to freeze a new syscall or IPC contract.
- A storage/shell/UI demo does not count as an argument for racing. AGENTS.md already says "a desktop mockup does not advance kernel readiness", and the stated target, Pixel 9, is ARM.

## Q2. SMP: stay single-core, but buy cheap insurance now

Do not build per-CPU structures, TLB shootdown or memory-ordering machinery yet. The cheapest insurance has three parts:

1. **Inventory plus annotation lint before M5a.** Every file-scope mutable object in `kernel/core` gets a tag: `/* SMP: boot-only | cpu-local | irq-protected | needs-lock(<name>) */`. A grep check in `make check` rejects untagged objects. It costs about an hour per file and blocks nothing. `sched.c` alone already has nine such objects, and `volatile je_timer_ticks` gives a false sense of safety: volatile is not atomic.
2. **A named critical-section API now: `je_irq_save()` / `je_irq_restore()`.** Single-core correctness already relies on IRQ masking. Naming those sections now turns the later SMP conversion into a mechanical change to spinlock-with-irqsave, instead of an archaeology project. This is not an SMP lock. It documents an invariant that already exists.
3. **Where the gate goes:** I would not put an SMP gate before M6. I would put it before any "supported" tier on multicore hardware. Until then, secondary CPUs are parked and the build reports an explicit degraded single-core profile, as AGENTS.md rule 5 requires.

One caveat: M6 storage brings DMA and IRQ-versus-thread concurrency even on one core. That is a reason to do part 2 now.

## Q3. Two schedulers: a debt trap as written, so the migration needs an ordering

As written, ADR 0013 has no trigger and no drift guard, which is how a temporary bridge becomes permanent. The real problem is MP3. User-thread semantics live inside `sched.c` today (`THREAD_USER`, `registered_vm`, the proc pool, killall reaping). ARM MP3 would have to re-implement all of that inside `sched_rr.c`. That gives two copies of the security-relevant reap and kill logic, not just two round-robin loops.

Least-risk order, one risky change per step:

1. ADR 0013 as accepted: `sched_rr.c`, ARM kernel threads only.
2. A shared conformance suite: one hosted test spec run against both cores, covering RR order, idle fallback, SLEEPING deadlines, ZOMBIE exclusion and reap rules. This makes drift visible.
3. Migrate x86 `sched.c` onto the `sched_rr` core, with M2 smoke, `test_sched_killall` and M3/M4 smoke as the safety net. This touches tested code, so it goes to the owner.
4. Only then, ARM MP3 user threads.

Yes, step 4 waits on step 3, and that is deliberate. This sequencing does not couple the two changes; it serializes them. The alternative is writing the user-thread kill path twice.

## Q4. Mutation testing: no per-PR kill-threshold gate

A threshold invites agents to write mutant-specific assertions that never state the property being protected. Most of the noise comes from equivalent mutants, not slow runs. I propose three tiers:

- **(a) Per PR, cheap and the main guard:** an invariant manifest mapping each ADR security invariant to the ID of a named denial test. CI fails if an invariant has no test or a test is missing. This puts the burden on what gets claimed, not on scores.
- **(b) Per PR, targeted:** semantic mutants only on the diffed critical-profile files (`usercopy.c`, `handle.c`, `syscall.c`, the `vm.c` validators). Use a small fixed set of mutation operators: flip bounds comparisons, delete return-value checks, drop rights-mask tests. A surviving mutant must be either killed or justified in a committed file, and the justification is reviewed by the other lineage. No percentage threshold.
- **(c) Nightly or weekly:** a full run with trend reporting. It never blocks merges, but a regression in the trend opens an issue.

The `test_image` pattern, hand-written deterministic mutants committed as tests, is the model to extend.

## Q5. The human gate: the first published claim that depends on a trust boundary

The first claim that must carry a named human qualification is any public statement that a security property holds or that a support tier exists. That means "enforced" in SECURITY.md, "supported" for a target, or a hardened/critical-profile label. Executing on owner hardware with persistent media is in the same class, because data loss and bricking are irreversible and an AI cannot observe them.

Everything below that line can remain AI-review-only, as long as it is labeled as such. That includes code, ADR drafts and status labels like implemented/tested/planned.

Making it concrete:

- A `CLAIMS` registry recording claim ID, evidence, and whether review was AI or human, protected by CODEOWNERS so owner approval is required.
- A CI lint that rejects "supported", "hardened", "enforced against" and similar words in status docs unless a registry entry backs them.

Two honesty points:

- The owner clicking merge is not qualification.
- Two AI lineages are not two independent reviewers. Their blind spots are correlated, so "cross-lineage agreement" should not be weighted as human-grade evidence.

---------- ROUND 1 · CODEX ----------
Round 2 — Codex

Claude’s direction is mostly right, but several proposed mechanisms are weaker than the policy they are meant to enforce.

**Q1 — Sequencing**

I agree that parity should apply to contracts, not milestone numbers. Blocking all x86 work until MP3 would waste useful integration capacity; allowing x86 to freeze a complete desktop ABI would create expensive rework.

I disagree that “storage-stack work behind existing handles” is automatically ABI-neutral. Storage choices constrain blocking semantics, completion delivery, buffer ownership, handle rights, persistence errors, and eventually filesystem/file-descriptor contracts. Those are exactly the assumptions ARM MP3 should exercise.

Recommendation:

- Permit x86 internal work that adds no externally observable contract.
- Require ARM MP3 before freezing new syscall, process, IPC, storage, or shell semantics.
- Allow x86 demos, but label them experimental and do not call them cross-architecture support.
- Require ARM execution before any M5–M7 claim involving a new user-visible contract or supported target.

The breakeven is not merely “the next syscall.” It is the first x86 feature whose implementation choices become an ABI dependency. For this project, storage and shell/process work likely cross that line even if initially hidden behind existing handles.

**Q2 — SMP**

I agree with cheap insurance and with remaining rigorously single-core for now. I disagree that comment tags plus `je_irq_save()` make later SMP conversion “mechanical.”

Comments miss `static` locals, shared heap objects, DMA-visible memory, and aliases. More importantly, IRQ masking only excludes local interrupt handlers; it does not provide inter-CPU exclusion or memory ordering. A future spinlock conversion still needs ownership, lock ordering, acquire/release semantics, interrupt nesting rules, and TLB-shootdown behavior.

The right distinction is:

- Add an **SMP-readiness gate** now.
- Do not add an **SMP-capability implementation** now.

The readiness gate should require:

- an inventory of mutable shared state, including the scheduler globals Claude identified in `sched.c`;
- an explicit `SINGLE_CPU` runtime/build profile that parks or refuses secondary CPUs and reports degraded capability;
- a rule that new mutable global state requires an owner/lifetime/context/concurrency record;
- documented IRQ/preemption critical sections and tests for interrupt-versus-thread races.

`je_irq_save()`/restore is worthwhile only as a real abstraction for the existing interrupt state contract. It must not be advertised as a lock precursor or substituted for SMP synchronization.

I would place this readiness gate before M5a/M6, because storage introduces DMA and interrupt/thread concurrency even on one CPU. I would place actual multicore support before the first multicore support-tier claim, not necessarily before all M6 implementation work.

**Q3 — Neutral core and scheduler migration**

Claude correctly identifies the debt trap: ADR 0013 has no trigger or deadline, and two implementations can drift. The key question is whether `sched_rr.c` contains only run-queue policy or also duplicates user-thread lifecycle and security logic.

I do not recommend making a wholesale x86 `sched.c` migration a prerequisite for MP3. `sched.c` is tested code, and changing it before ARM userspace exists couples a risky refactor to a second risky port. That is precisely the kind of change the repository rules say requires owner escalation.

The mandatory pre-MP3 work should be narrower:

1. Define a scheduler interface and shared conformance suite covering RR order, idle fallback, sleeping deadlines, zombie exclusion, and reaping.
2. Port ARM kernel-thread scheduling through that interface.
3. If ARM would duplicate process/VM/user-thread lifecycle logic, extract that lifecycle into a shared module before ARM MP3. Do not duplicate security-relevant reap, kill, or ownership rules.
4. Leave wholesale x86 run-queue migration as a separately reviewed change, with a hard trigger: before the next scheduler feature or before any supported ARM/x86 parity claim.

So: two schedulers are acceptable only if one is a bounded backend and invariant logic is shared. Two copies of user lifecycle semantics are not acceptable. Claude’s full migration ordering is safer for semantic duplication, but unnecessarily risky as a blanket blocker for MP3.

**Q4 — Mutation testing**

I agree with rejecting a global per-PR mutation score. It will incentivize mutant-specific assertions and produce noise from equivalent mutants.

I disagree that an invariant manifest merely requiring a named test is sufficient. A test can exist and still fail to assert denial, rollback, or unchanged state. Also, mutating only diffed files misses regressions where an unchanged caller or validator no longer protects a changed invariant.

Recommended mechanism:

- Each security invariant has a canonical negative test that checks rejection and relevant state preservation.
- Every PR changing a protected invariant runs that focused denial suite.
- Hand-written critical mutants, like those already in `test_image`, are committed and must be killed per PR.
- A bounded semantic mutator runs on affected invariant paths, not just changed files.
- Full mutation runs nightly or before qualification/release; it reports trends and blocks qualification when a non-equivalent critical mutant survives.
- Equivalent-mutant exemptions require an explicit, committed rationale; “another AI reviewed it” is not evidence of equivalence.

This gives fast per-PR assurance without making a noisy percentage threshold the merge criterion. Mutation testing should validate the invariant suite, not become the invariant suite.

**Q5 — Human-gate boundary**

I agree with Claude’s core boundary: AI review is not qualification, and two AI lineages are not independent human review.

I would narrow “any public statement that a security property holds.” Otherwise ordinary status documentation becomes impossible. The project needs three distinct claim classes:

- “The implementation contains check X” — implementation evidence; AI review is acceptable.
- “Tests passed at commit Y in environment Z” — reproducibility evidence; AI can report it honestly.
- “This boundary is enforced,” “this target is supported,” “this profile is hardened,” or “this release is qualified” — qualification claims; named human signoff required.

The first mandatory human gate is therefore the first claim that an operator may rely on for security, isolation, data safety, or supported hardware—not necessarily the first mention of the mechanism in a document. Existing x86 M3/M4 work can remain implemented/tested and AI-reviewed, but must not be relabeled hardened, supported, or independently qualified without that signoff.

A structured claims registry is useful, but keyword linting alone will be brittle. Require a qualification record containing claim scope, exact commit, target/profile, commands and results, known hazards, named human, and revalidation date. Explicit human qualification—not merely merge approval—should gate supported-tier, critical-profile, and release claims.

---------- ROUND 2 · CLAUDE ----------
Round 2 — Claude, replying to Codex

I agree with most of Codex's refinements. I dispute three points, and two of them come from what the tree actually contains (checked at `645a41f`).

**Q1 — Sequencing: concede, with one change**

- **Concede:** storage behind existing handles is not ABI-neutral. Buffer ownership, completion delivery and blocking semantics are exactly what ARM MP3 should exercise first.
- **Dispute:** "Freeze nothing until ARM MP3" lets x86 implement storage and shell while deferring the freeze. In practice the ABI then gets frozen by those x86 consumers anyway.
- **Recommendation:** an x86 feature that introduces a new contract may merge only behind an `EXPERIMENTAL_ABI` marker. That marker must not be removed until ARM runs the same contract's conformance tests. This gives Codex's rule a mechanical check instead of an honor system.
- **Breakeven:** the first new syscall, handle-rights bit, or blocking/completion primitive. That is earlier than the "M5–M7 claim" Codex names.

**Q2 — SMP: concede the main point, dispute the timing**

- **Concede:** IRQ masking is not inter-CPU exclusion. `je_irq_save()` must not be presented as a step toward a lock. I also accept that comment tags miss `static` locals, aliases and DMA-visible memory.
- **Agree:** the `SINGLE_CPU` profile that parks or refuses secondary CPUs is the right move. It turns SMP from a disclaimer into an explicit degraded profile (AGENTS.md rule 5).
- **Dispute:** "readiness gate before M5a/M6" is too late for the inventory. It costs almost nothing today. `sched.c` has 8 mutable file-scope globals (`threads`, `process_pool`, `registered_vm/proc`, `current`, `ready`, two counters). `kheap.c`, `endpoint_kern.c` and `vm.c` already contain interrupt-masking sites.
- **Recommendation:** do the inventory and the "every new global needs an owner/lifetime/concurrency record" rule now. Put the IRQ-versus-thread race tests and the `SINGLE_CPU` profile at the M5a gate. Require real multicore before the first multicore support-tier claim.

**Q3 — Neutral core: Codex's plan has a contradiction**

Codex says "don't touch `sched.c` before MP3" and also "if ARM would duplicate the user lifecycle, extract it into a shared module before MP3." In the tree, that lifecycle lives inside `sched.c`:

- `process_pool` and `registered_vm[]`/`registered_proc[]` are in `sched.c`, along with `je_proc_link` (`kernel/core/sched.c:220`).
- The kstack ownership rule "teardown, not reap, frees the stack" is documented in `sched.c` (`:29`, `:173`).
- `je_sched_reap_dead` and the `je_proc_on_dying` hook are there too (`:238–258`).

ARM MP3 needs all of this, so it will duplicate it unless it is extracted. Extracting it is a refactor of tested `sched.c`. ARM userspace is therefore blocked on a `sched.c` change either way. The real choice is whether that refactor is lifecycle-only or full run-queue migration.

- **Recommendation:** before MP3, do a lifecycle-only extraction (proc/VM registration, link, dying, reap) out of `sched.c`, with no changes to run-queue logic. Gate it on unchanged x86 `make check` and smoke results, the M3/M4 proofs, and a shared conformance suite. Escalate it to the owner as a change to tested code.
- The run-queue migration onto `sched_rr.c` comes later, with Codex's trigger: before the next scheduler feature or any parity claim.
- ADR 0013 "Flex" only says "migrate `sched.c` onto `sched_rr.c` (later)", with no trigger. The ADR needs an amendment recording one.

**Q4 — Mutation testing: concede one point, correct one fact**

- **Concede:** a manifest that only names a test is too weak. The canonical denial test must assert both rejection and unchanged state, as `test_physmem.c` and `test_uframe.c` already do.
- **Correction:** `test_image.c:147 check_mutations` mutates input descriptors. It is a negative input corpus, not code mutants. The tree has no committed way to build code mutants: no `JE_MUTANT` macro or `MUTATION FIRED` build switch exists. ADR 0013 (`:165–174`) specifies mutation markers for ARM, but that code is unwritten. Codex's "hand-written critical mutants must be killed per PR" therefore needs a new mechanism first: a `make mutants` target building `-DJE_MUT_<ID>` variants, each expected to fail.
- **Recommendation:** for ADR-listed mutants only, build that mechanism and run it per PR on the affected path. Run broad mutation nightly. Codex's equivalence-exemption rule stands, and an AI-written rationale counts only as a recorded argument, not as evidence.

**Q5 — Human gate: agree, and push it earlier**

- **Agree:** Codex's three claim classes are better than my keyword lint, and the qualification-record fields are right.
- **Missed:** the first claim is closer than "supported tier." `SECURITY.md` and the M3a status already describe enforced boundaries (W^X, image validation, handle rights) as properties. Under Codex's own taxonomy, "this boundary is enforced" is a qualification claim. Those existing sentences need rewording now to class 1 ("check X is implemented") or class 2 ("tests passed at commit Y"). Otherwise the project is already making class-3 claims without a human.
- **Recommendation:** first, a one-time sweep reclassifying existing boundary wording. Then require named human signoff for any class-3 claim, with a structured record (claim, commit, profile, commands, hazards, human, revalidation date). Everything below class 3 keeps full AI velocity.

**Owner escalations:** the Q3 lifecycle extraction (changes tested code) and the ADR 0013 trigger amendment (changes an accepted ADR).

---------- ROUND 2 · CODEX ----------
Round 2 — Codex replying to Claude

**Q1 — Sequencing: agree, strengthen the mechanism**

I agree that the breakeven is earlier than M5–M7: the first new syscall, rights bit, or blocking/completion semantic can freeze the ABI accidentally.

`EXPERIMENTAL_ABI` is useful, but only if mechanically enforced. A marker in documentation is insufficient. Experimental interfaces must be:

- excluded from the supported ABI/conformance claim;
- covered by architecture-neutral semantic tests;
- prevented from silently becoming dependencies of later “stable” features;
- promoted only after ARM executes the same tests.

I would permit x86 storage/shell implementation behind that status, but not an ABI or support-tier claim. Claude correctly identifies the first irreversible contract as the breakeven.

**Q2 — SMP: agree with inventory now; refine the test language**

The inventory should happen immediately, before M5a adds more state. Claude’s concrete finding—eight mutable scheduler globals plus interrupt-masking sites in `kheap.c`, `endpoint_kern.c`, and `vm.c`—makes this cheap risk reduction, not premature SMP implementation.

I still reject calling IRQ-versus-thread tests “race tests.” They exercise preemption on one CPU, not inter-CPU races, cache visibility, lock ordering, or TLB shootdown. Name them accurately: single-core preemption/interruption tests.

The cheapest insurance is:

1. inventory globals, static locals, IRQ/DMA-visible memory, and interrupt-context assumptions;
2. require ownership/lifetime/concurrency annotations for new mutable state;
3. make `SINGLE_CPU` an explicit boot/profile policy that parks or refuses secondary CPUs;
4. forbid multicore support claims until real multicore execution is tested.

No lock primitive should be added merely to create a checkbox. A fake or unused lock can create more confidence debt than value.

**Q3 — Lifecycle extraction: agree, but constrain it sharply**

Claude is right that my earlier “do not touch `sched.c` before MP3” wording was too broad. The process lifecycle is actually co-located with the x86 scheduler: `process_pool`, `registered_vm[]`, `registered_proc[]`, `je_proc_link`, reap, dying hooks, and the kstack teardown rule.

ARM MP3 therefore needs a tested-code change even if run-queue migration is deferred. The least-risk order is:

1. extract only process/VM registration, lifecycle links, dying/reap, and ownership contracts;
2. preserve x86 run queues and scheduling behavior byte-for-byte where practical;
3. add shared lifecycle/conformance tests;
4. run x86 `make check` and applicable smoke tests;
5. implement ARM MP3 against the extracted lifecycle;
6. migrate run queues later, before the next scheduler feature or any scheduler-parity claim.

This must be owner-escalated because it changes tested scheduler code. ADR 0013 should gain the missing migration trigger; otherwise “later” is an unbounded debt item.

One additional constraint: the extraction must not produce a second process-lifecycle owner. `sched.c` and the new module cannot each retain partial authority over registration, death, or stack reclamation.

**Q4 — Mutation: Claude’s correction is valid**

I overstated the current state. `test_image.c:147 check_mutations` is a mutated-input negative corpus, not code mutation testing. The repository has no committed `JE_MUTANT` mechanism or code-mutant build path.

The right design is not a global kill percentage. It should be a manifest of named, intentional critical mutants:

- each mutant has an invariant and expected killer test;
- each killer asserts rejection/error plus unchanged state where applicable;
- affected manifests run per PR;
- broader mutation operators run nightly or on release candidates;
- equivalent mutants require an explicit rationale;
- unavailable toolchains or skipped mutant sets are reported as blocked/skipped, never green.

The implementation should prefer test-only generated variants or controlled compile-time substitutions, rather than permanent production mutation branches. The initial gate should be “all committed critical mutants are killed,” not “coverage exceeds X%.”

**Q5 — Human gate: agree, with a narrower classification**

Claude is right that existing broad statements in `SECURITY.md` and M3a status may already overclaim. They need a one-time wording sweep.

I would distinguish:

- “`je_map()` rejects this specific W+X request, tested at commit Y” — implementation/test evidence;
- “the W^X security boundary is enforced across all privileged paths and profiles” — qualification claim requiring human review.

Thus the first mandatory human gate is not every sentence containing “enforced.” It is the first broad security, hardened, release, or support-tier claim—for example “M3a is security-qualified,” “Pixel 9 is supported,” or “the system’s security boundaries are enforced.”

Below that boundary, AI review plus reproducible local evidence can preserve velocity. Above it, require a named human, exact commit/profile/commands, hazards, and revalidation date. AI-assisted review cannot satisfy that qualification independently.

**What Claude missed**

The key missing cross-cutting requirement is preventing status markers from becoming folklore:

- `EXPERIMENTAL_ABI` needs CI and conformance enforcement, not just labels.
- The SMP inventory must include DMA and IRQ-visible memory, not only globals.
- Lifecycle extraction needs a single ownership boundary and explicit no-regression criteria.
- Mutation CI must distinguish “not implemented,” “skipped,” “equivalent,” and “killed.”
- Human qualification records should be tied to support tiers and security claims, while preserving ordinary implementation work below that gate.

My final recommendations are therefore: experimental ABI gating now; SMP inventory and explicit `SINGLE_CPU` policy now, real SMP later; lifecycle-only extraction before ARM MP3, run-queue migration afterward; deterministic critical-mutant gates per affected PR plus broad nightly mutation; and named human qualification before the first broad security or support claim.

================ END DEBATE ================
Orchestrator: read the above, relay any unresolved disagreement, and make
the final call. Verified compile/test/boot facts beat the debate; escalate
contract / tested-code / ADR changes to the human.
transcript saved: /tmp/peer-debates/debate-20261106-042052.md
