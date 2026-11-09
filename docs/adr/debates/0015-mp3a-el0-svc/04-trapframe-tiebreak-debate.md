================ PEER DEBATE (1 rounds) ================
TOPIC: Tie-break: MP3a ARM EL0 user trap frame - separate je_arm_user_trapframe type vs dual-use the frozen frames sp field as SP_EL0 for EL0 frames
==============================================================

---------- ROUND 1 · CLAUDE ----------
**Pick: dual-use `sp` (option c).** Codex's concern about misusing the field is valid. I address it with mode-checked accessors, not with a second layout.

**Why (c) wins.** At every point where confusion could happen, the frame's class is decided at runtime, never by its static C type:
- **Switches across privilege levels are the normal case.** A timer IRQ taken from EL0 (vector slot 9) can resume slot 0's EL1h idle frame. An EL1h IRQ during idle (slot 5) can resume an EL0 frame. The restore tail after `mov sp,x0` (`vectors.S:239-277`) does not know which vector it came from. In both designs it must read the *next* frame's SPSR.M at offset 264 to decide whether to `msr SP_EL0`. So (b) cannot pick a separate restore tail by vector.
- **EL0-origin IRQs must go through the MP2-tested C chain**, which is typed on the frozen struct: `je_arm_irq(je_arm_trapframe*)` (`trapframe.h:74`) → `je_arm_sched_irq` (`sched_arm.c:514`) → `record_current_frame` / `no_switch_frame` (`:455`, `:486`, which write the cookie). Under (b) you either fork that tested chain or cast between two distinct struct types. The cast is (c) under another name, plus a strict-aliasing hazard: `ARM_CFLAGS` is `-O2` without `-fno-strict-aliasing` (`mk/aarch64.mk:105`).

**Q1 — is reading SPSR.M first safe?** Yes.
- One correction to the premise: EL1h phase-1 frames sit anywhere in the stack, not at `top-304`.
- The position check that runs before the first dereference (`sched_arm.c:236-246`) only proves the frame lies inside the slot's own mapped kernel stack. That check does not depend on the frame's class.
- EL0 cannot write that memory (`AP=00`, `paging.h:41`). On entry from EL0, hardware writes M=0, so a user cannot forge it. Only ring-0 corruption can.
- So M is never trusted alone. It is checked against a kernel-only per-slot class (set when the slot is created, never read from the frame) and, for captured frames, against the vector group. A mismatch is fatal.
- (b) needs exactly this discriminator to choose its cast.

**Q2 — which is more faithful to the frozen contract?**
- The frozen part is field order, offsets and size (`trapframe.h:3-8`). The field is already named `sp`; only the comments say "SP_EL1". The MP1 saver comment (`vectors.S:111-116`) already calls it the "interrupted SP" and notes that lower-EL slots would need SP_EL0.
- To be honest about it: (c) still widens the meaning of a frozen field, so it goes to the owner.
- (b) also has to go to the owner, because it changes the same tested tail and validator. On top of that it creates a second frozen ABI.
- The assembly is the same in both: new slot 8/9 veneers that spill x16–x19, `mrs SP_EL0` stored at offset 248, an M==0 check mirroring `vectors.S:195-198`, and one conditional `msr SP_EL0`.
- I disagree that (c) "silently breaks" consumers. I checked every `->sp` reader:
  - `sched_arm.c:254` fails closed on an EL0 frame with a loud `sched_fatal`.
  - `:412` and `:1102` only build EL1h frames.
  - The IRQ evidence code (`main.c:1116`, `:1214`) logs `sp` next to `spsr` and checks nothing.

**Q3 — is the `sp == frame_end` split clean?** Yes. The real hazard is elsewhere: future code treating an EL0 frame's `sp` as a kernel pointer. The cortex-a72 target has no PAN, so that dereference would read user memory without faulting. The fix:
- add `je_arm_tf_kernel_sp()` (fatal unless EL1h) and `je_arm_tf_user_sp()` (fatal unless EL0t);
- add a `make check` grep that bans raw `->sp` outside the saver, validator and evidence code.

**Q4 — fixup table and crash dumper.** I concede that consumers branch either way.
- The copy-fixup is EL1h-only and must be gated in both designs. If EL0 does a `br` to a fixup-keyed kernel address, it takes a UXN instruction abort on slot 8 with ELR equal to that key. Gate the lookup on ESR.EC==0x25 && M==EL1h && vector==4.
- The dumper reads raw bytes and has to decode M either way, and it cannot see (b)'s type. One layout plus one label decode is simpler.

**Validator split**

| Scope | Check | On failure |
|---|---|---|
| Shared | Existing position proof, run before any dereference (`:236-246`) | Fatal |
| Shared | M[4]==0, and SPSR.M equals the slot's kernel-recorded class (0x5 kernel slot, 0x0 user slot) | Fatal |
| Shared | Phase-1 frames: vector 4..7 ⇔ EL1h, 8..11 ⇔ EL0t | Fatal |
| Shared | `pad` cookie (`:275-284`) | Fatal |
| EL1h only | Today's checks unchanged: `sp==frame_end`, `spsr&~NZCV==0x345`, ELR in kernel text, phase-0 trampoline/`gpr[0]` | Fatal |
| EL0 only | Frame at exactly `top-304` in every phase (MP3a keeps I masked in SVC handlers; a user slot holding an EL1h frame is fatal; preemptible syscalls are not supported yet) | Fatal |
| EL0 only | On resume, force `spsr=(spsr&NZCV)\|0x340` instead of rejecting. DIT/SSBS/TCO can be written from EL0 on v8.4+ cores; they don't exist on cortex-a72. Record this as a target-profile assumption. | Never fatal |
| EL0 only | Phase 0 (built by the kernel): ELR equals the image entry, `sp` equals the arena stack top, GPRs zero | Fatal |
| EL0 only | Phase 1 (captured): **no ELR/SP_EL0 range checks** | — |

The last row corrects my earlier position, and I disagree with Codex's "`sp_el0` inside the user stack" check:
- A captured frame's ELR and SP_EL0 belong to the user, and MP3a never dereferences them.
- Restoring them is harmless: M is forced to EL0t and every kernel leaf has UXN set (`paging.c:271-272`), so a bad value just faults at EL0 and the user dies.
- Checking them when the timer preempts makes death depend on timer phase, which makes the negative test flaky. It is also not a trust-boundary check.

**Restore tail addition:** after `ldp x0,x1,[sp,#JE_TF_ELR]`, insert `tst x1,#0xf; b.ne 1f; ldr x2,[sp,#JE_TF_SP]; msr SP_EL0,x2; 1:`.

---------- ROUND 1 · CODEX ----------
I pick **DUAL-USE-SP**. The fixed layout and opaque scheduler make a second C type mostly nominal: runtime code still must inspect `SPSR.M` before choosing the interpretation. One layout has the smaller MP2 blast radius.

I agree with Claude that reading offset 264 is safe—but only after proving the pointer and full 304-byte frame lie within the kernel stack. Exact `top-304` is an EL0 invariant, not an EL1 prerequisite. `SPSR.M` must be cross-checked with the captured vector group; it must not be trusted as the sole discriminator.

I dispute the proposed immutable “slot class.” A user process can have an EL0 frame or an EL1h frame while servicing a kernel entry. The discriminator should be the frame’s validated vector/source metadata plus `SPSR.M`, or explicitly maintained per-frame state—not a class fixed when the slot was created.

The `sp` split is clean:

- EL1h only: require `SPSR.M == 0x5`, `frame->sp == frame_end`, existing kernel ELR/text/trampoline checks.
- EL0t only: require `SPSR.M == 0`, frame exactly at `kstack_top - JE_TF_SIZE`, and interpret `frame->sp` as `SP_EL0`. Require 16-byte alignment and `user_base <= sp <= user_end`; require aligned ELR within the user executable/arena range.
- Shared: pointer/stack bounds before dereference, valid AArch64 mode (`M[4] == 0`), vector-group/M agreement, generation cookie, and existing structural checks.
- Invalid kernel invariants are fatal. Invalid user ELR/SP is user death/refusal of the frame, not `sched_fatal`.

Claude’s “no ELR/SP range checks” is the main miss. UXN and EL0 permissions may prevent kernel execution, but delayed faults and arbitrary user addresses violate the MP3a return contract and make failures less deterministic. Validate them at the trust boundary.

For MP3b and crash tooling, one layout is genuinely simpler: common offsets are decoded once, then consumers branch only where semantics differ. The branch does not disappear under separate types; it moves into runtime type selection, and the opaque `void *` still requires that selection. Copy-fixup should remain EL1h-only and be gated by validated mode/vector/ESR.

The semantic widening of `sp` is still an owner/ADR-level contract decision, but it changes no offsets, size, or assertions. That is lower risk than introducing a second 304-byte ABI and duplicating saver, restore, validator, fixup, and tooling paths.

================ END DEBATE ================
Orchestrator: read the above, relay any unresolved disagreement, and make
the final call. Verified compile/test/boot facts beat the debate; escalate
contract / tested-code / ADR changes to the human.
transcript saved: /tmp/peer-debates/debate-20261108-213444.md
