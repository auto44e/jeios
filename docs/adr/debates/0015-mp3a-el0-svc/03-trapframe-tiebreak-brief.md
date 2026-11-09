# Tie-break — MP3a user trapframe: separate type (b) vs dual-use `sp` (c)

Two design reviews of JeiOS MP3a (ARM64 EL0 entry) AGREE on almost everything
(shared per-process TTBR0 mapping the kernel EL1-only + user arena EL0; defer the
TTBR1 relink; SVC #0 / x8=op / x0-x5=args / x0=status / x1=result). They DISAGREE on
one thing: the EL0 user trap frame. Settle it. Prefer the lower-risk option for the
frozen 304B je_arm_trapframe and the MP2-tested saver/restore/validator.

## Ground truth
- `je_arm_trapframe` is FROZEN 304B: gpr[31]@0, sp@248, elr@256, spsr@264, esr@272,
  far@280, vector@288, pad@296, with _Static_asserts. `pad`@296 ALREADY holds the
  ADR 0013 generation cookie (sched_arm.c). The MP2 saver computes `sp` = interrupted
  SP via `add sp,#JE_TF_SIZE` (correct for EL1h); the saver comment already notes the
  lower-EL value would be SP_EL0. The sched_arm.c validator (EL1h-only today)
  requires `frame->sp == frame_end` (treats sp as the post-frame kernel stack addr).
- The round-robin core holds the frame as an OPAQUE `void *`; the ARM seam's
  validator reads it. The ELR-keyed fault-fixup table (MP3b) and crash tooling bind
  to the frame layout.
- x86 uses ONE je_trap_frame for both CPL0 and CPL3 frames.

## Position (b) — Codex: a SEPARATE je_arm_user_trapframe type
Same 304B layout/offsets but a distinct type with a named `sp_el0` field, its own
saver, validator, restore tail, asserts. Rationale: don't semantically overload the
frozen EL1 type; EL0's field must be the USER sp, not the kernel-stack addr.

## Position (c) — Claude: DUAL-USE the existing `sp` field
`tf->sp` = whichever SP the interrupted PSTATE used (SP_EL1 for EL1h frames, SP_EL0
for EL0t frames), selected by SPSR.M. No new type, no new offsets, one saver/restore/
validator that branches on SPSR.M (cross-checked against the vector group; mismatch
fatal). Rationale against (b): two layouts behind the scheduler's opaque void* frame
pointer means the validator must know the type BEFORE it can read SPSR → type
confusion; (b) needs two savers, two restore tails, two fixup-table layouts, two
crash-tool layouts; x86 proves one frame for both privilege levels works. The EL0
validator in (c): require SPSR.M==0 (EL0t), re-sanitize SPSR to (spsr&NZCV)|0x340,
require M[4]=0 (no AArch32), frame at exactly kstack_top-304 (= SP_EL1 while EL0
runs), ELR 4-aligned in [base,end), SP_EL0 in (base,end]; wrong position/mode/cookie
is fatal, bad ELR/SP is user-death.

## Question for each (the OTHER's position is above; one short round)
1. Does (c)'s "the validator branches on SPSR.M, cross-checked against the vector
   group" actually avoid the type-confusion Codex worries about in (b)? I.e. is
   reading SPSR.M from a fixed offset (264) BEFORE trusting anything else safe,
   given the frame is at a validated position (kstack_top-304) for BOTH classes?
2. (b) says a separate type avoids overloading the frozen EL1 contract. But (c)
   keeps offsets/size/asserts byte-identical and only changes a COMMENT + adds an
   SPSR.M branch — is that actually MORE faithful to "don't reopen the frozen
   contract" than inventing a second 304B type? Which has the smaller MP2-tested
   blast radius (saver at vectors.S, restore tail, the sched_arm validator, the
   fixup table, crash tooling)?
3. Codex's concrete objection to (c): the EL1h validator requires `sp == frame_end`;
   an EL0 frame's sp is the user SP, not frame_end. Does (c) handle this by making
   the `sp==frame_end` check EL1h-ONLY and adding the EL0 sp-in-arena check for EL0
   frames? Is that a clean split or a landmine?
4. For MP3b's ELR-keyed copy-fixup and the crash dumper: is ONE layout (c) genuinely
   simpler for them than two (b), or does the EL0-vs-EL1 branch just move into those
   consumers either way?

Give a crisp final pick: SEPARATE-TYPE or DUAL-USE-SP, the decisive reason, and the
exact validator split (which checks are EL1h-only, which EL0-only, which shared).
Keep it short — tie-break, not re-review.
