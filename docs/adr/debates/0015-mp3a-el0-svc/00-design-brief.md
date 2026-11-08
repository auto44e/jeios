# Design review — MP3a: ARM64 EL0 entry + SVC ABI + the TTBR split (BEFORE code/ADR)

You are reviewing a DESIGN for JeiOS (freestanding AArch64 kernel), not code.
MP3 = ARM64 userspace parity (the ARM analogue of x86 ADR 0007/0008/0009: EL0
isolation, typed handles, capability IPC). MP3a is the first slice: EL0 entry/return
+ the SVC syscall ABI + the kernel VA relocation that EL0 requires. Three
frozen-contract decisions need to be right before an owner-gated ADR; attack them.

## Verified ground truth (main 0a4762f, post-MP2 + ADR 0014)
- ARM runs EL1-only today. vectors.S: EL1h slots 4-7 live (slot 5 = the MP2
  returning-IRQ path); lower-EL slots 8-11 are bounded-halt stubs; EC_SVC64 (0x15)
  is #defined but never dispatched. An EL0 exception today just halts.
- The frozen `je_arm_trapframe` is 304B, 16-aligned: gpr[31]@0, sp@248, elr@256,
  spsr@264, esr@272, far@280, vector@288, pad@296, with _Static_asserts. CRITICAL:
  the saver stores `sp` = interrupted SP computed as `add sp,#JE_TF_SIZE` — correct
  ONLY for the EL1h group; for a lower-EL trap the interrupted SP is SP_EL0 and the
  saver never reads SP_EL0. So the frozen frame as-saved does NOT capture EL0's SP.
- MMU (ADR 0011 D2): kernel is IDENTITY-mapped in TTBR0_EL1, kernel linked at
  0x40080000, TCR.EPD1=1, TTBR1_EL1=0. A pre-enable audit asserts AP[1]=0 (no EL0)
  on EVERY leaf, byte-for-byte against frozen class literals; no EL0 map class, no
  ASID, no nG, no per-process root. The user arena VA 0x40000000 (JE_VM_ARENA_BASE,
  2 MiB) is INSIDE the TTBR0 range the kernel identity-maps (QEMU RAM base) — it
  collides with the kernel map.
- ADR 0011 D2 EXPLICITLY scheduled the high-half (TTBR1) kernel split for "the
  first PR of MP3", wrote the descriptor builder VA-generic for reuse, and flagged
  three hazards: (1) a high-half kernel must be RELINKED off 0x40080000 (adrp
  survives but literal pools / .quad symbol addresses / function pointers /
  je_arm_dtb_phys consumers do not) — that edits MP1-tested boot.S and its
  load==link check, an owner-escalation-level tested-path change; (2) a linear map
  of RAM re-aliases [_text,__rodata_end) RW, creating a W^X-alias hole identity
  doesn't have; (3) TTBR1_EL1 is UNKNOWN at reset, so enabling TTBR1 without a
  correct base allows speculative garbage-table walks. ADR 0011 D2 "Flex: revisit
  at the MP3 first PR."
- sched_arm.c: EL1h-pinned (SPSR&~NZCV must==0x345, M=EL1h), arm_dispatch_install
  is a NO-OP (no TTBR0/SP_EL1 install). No process/VM/lifecycle linkage yet.
- Reusable arch-neutral cores (confirmed): handle.c, endpoint.c, endpoint_kern.c,
  proc.c, lifecycle.c (je_sched_register_process / je_lc_*), and the PURE
  je_syscall_decode(op, args[6], &status, &result). The x86-coupled glue
  je_syscall_dispatch reads/writes je_trap_frame rax/rdi/rsi/rdx/r10/r8/r9.
- The x86 template: je_thread_create_user_start synthesizes the user frame at
  rsp0_top-sizeof on the process kstack (cs/ss=USER, rip=ARENA_BASE+entry,
  rsp=ARENA top, rflags=0x202); select_and_install_next installs TSS.RSP0 + CR3 for
  a user next-thread; je_uframe_check validates cs/ss + canonical rip/rsp in arena +
  re-sanitizes rflags (force IF=1, clear IOPL/NT/...). int-0x80 is a DPL3 gate.

## DECISION 1 (biggest): kernel VA regime for EL0 — TTBR1 kernel + TTBR0 user
The user arena at VA 0x40000000 collides with the kernel identity map in TTBR0.
ADR 0011 D2 planned to fix this now by relocating the kernel to the high half
(TTBR1_EL1) so EL0's TTBR0 can host the per-process user arena.
- Is moving the kernel to TTBR1 the right call, vs alternatives? Alternatives to
  weigh: (a) keep the kernel identity in TTBR0 and carve the user arena out / move
  the user arena VA elsewhere (avoid the relink but share TTBR0 between kernel+user,
  which breaks the clean per-process-TTBR0 isolation x86 gets from CR3); (b) kernel
  in TTBR0 identity AND a separate TTBR0 per process that still maps the kernel
  low (so the kernel is reachable while EL0 runs) — how is the kernel reached
  during a syscall then? (c) the ADR 0011 D2 plan: kernel → TTBR1 high-half,
  per-process TTBR0 holds only the user arena, TTBR0 switched (with ASID) per
  process; the EL1 syscall/exception path runs on TTBR1 so it's always mapped.
- The relink hazard: ADR 0011 D2 says a high-half kernel must be relinked off
  0x40080000, editing MP1-tested boot.S. Is there a way to get EL0 isolation with
  a SMALLER tested-path blast radius for MP3a specifically? E.g. can MP3a keep the
  kernel identity-mapped (NOT relink) and ONLY add per-process TTBR0 user maps that
  ALSO carry the kernel's low mappings as global/EL1-only entries (so SP_EL1 code is
  reachable), deferring the full high-half relink to a later MP3 slice? What breaks
  if the kernel stays in TTBR0 and each process gets its own TTBR0 that contains
  BOTH the kernel (EL1-only, global) and that process's user arena (EL0)? Is that a
  legitimate smaller first step, or does it bake in a hazard?
- Whichever: how is the kernel reachable when EL0 traps in (SVC/IRQ/fault)? On
  AArch64 the exception is taken to EL1 using VBAR_EL1 and SP_EL1; the translation
  regime doesn't switch on exception entry, so the kernel's mappings must be live in
  whatever TTBR0/TTBR1 config is active at EL0. Pin this precisely.
- TTBR1-UNKNOWN-at-reset and the W^X linear-map-alias hazards (ADR 0011 D2 points
  2-3): if we go high-half, how are they closed in MP3a? If we DON'T relink this
  slice, are they avoided entirely?

## DECISION 2: SVC syscall register ABI (frozen contract, mirror ADR 0007 D8/D10)
x86: RAX=op, RDI/RSI/RDX/R10/R8/R9=args[0..5], RAX=status, RDX=result; int 0x80.
je_syscall_decode is already op+args[6] agnostic. Propose the AArch64 mapping:
sketch is SVC #0, op in x8, args x0-x5, status in x0, result in x1. Is x8 right
(it's the Linux svc NR register; or use x16/x17 which are IP0/IP1 scratch)? Does
returning status in x0 + result in x1 cleanly map onto the decode's {status,result}?
Any AArch64 PCS / syscall-restart (the 4-byte post-SVC ELR already points past the
SVC, vs x86's 2-byte int rewind) wrinkle? Is one SVC immediate (#0) enough or do we
want the op in an immediate? (x86 chose a register op; mirror that.)

## DECISION 3: user trapframe — extend the frozen 304B frame vs a separate EL0 frame
The frozen frame's `sp` field captures SP_EL1 for EL1h, not SP_EL0 for EL0. Options:
(a) extend the frozen 304B frame with an SP_EL0 field (reopens the frozen offsets +
_Static_asserts + any crash-dump/fixup tooling bound to them — risky);
(b) a SEPARATE je_arm_user_trapframe type for the EL0 entry path, leaving the EL1
frame frozen; (c) document that for the lower-EL groups the existing `sp` field
holds SP_EL0 (dual-use, no new field, EL0 saver reads SP_EL0 into it). Which keeps
the frozen EL1 contract intact AND gives the EL0 path a clean SP_EL0? What does the
sched_arm.c frame validator need (it currently requires M=EL1h; EL0 frames are M=0)?

## Output
For each decision: a crisp recommendation with the why, the rejected alternative's
predicted cost, and the tested-path blast radius (what MP1/MP2-tested ARM code each
option touches). Call out explicitly: can MP3a achieve real EL0 isolation WITHOUT
relinking the kernel (smaller first slice), or is the high-half relink unavoidable
for a sound design? End with the recommended MP3a scope boundary (what's in MP3a vs
deferred to MP3b+), and the single biggest risk.
