# MP3b design brief — ARM64 usercopy (ELR-keyed fault fixup) + typed-handle syscalls

JeiOS freestanding kernel, repo main HEAD 0cfcde0 (MP3a complete). MP3b = the aarch64
analogue of x86 "M3b": (A) safe copy_from/to_user with an EXACT-ELR-keyed fault-fixup net,
and (B) wiring the already-arch-neutral typed-handle syscalls (handle.c / je_syscall_decode)
through the ARM SVC path so EL0 processes can mint/close/query/dup/rights handles and run
the copy_probe op. This is a DESIGN REVIEW before any code is written. Attack the design.

## Frozen/known-good reference (x86, do not change)
- `kernel/core/usercopy.c`: prevalidates EVERY page of a user span (present + perm via
  `vm->ops->read_user_pte`) BEFORE touching it, sets a single global copy scope
  {active,dir,user_lo,user_hi,owner_vm,owner_index}, runs the ONE approved asm site
  `je_copy_user_run`, clears the scope. A recovered fault => JE_ABI_INVALID. The fixup
  predicate `je_usercopy_try_fixup(frame,cr2,pfec)` recovers ONLY IF: vector14+cs8,
  scope active, PFEC is not-present (P=0, not RSVD/FETCH), faulting RIP EXACTLY == the load
  or store label (never a range), direction matches (from_user=load+read, to_user=store+write),
  CR2 in [user_lo,user_hi), and scope owner == current user. Else fatal.
- `kernel/arch/x86_64/usercopy.S`: leaf fn, no frame; `je_copy_user_load_rip: movb (%rsi),%cl`
  / `je_copy_user_store_rip: movb %cl,(%rdi)`; `je_copy_user_fixup: movl $1,%eax; ret`.
  Because it's a leaf, the handler just rewrites RIP->fixup and the `ret` unwinds to the C caller.
- `include/jeios/handle.h` + `kernel/core/handle.c`: typed handle table (16b index/16b gen,
  gen starts at 1 so handle0 invalid; states FREE/LIVE/RETIRED; alloc/lookup/dup_self/close/
  resolve_any). ALREADY arch-neutral and ALREADY linked into the ARM build.
- `kernel/core/syscall.c`: pure `je_syscall_decode(op,args[6],&status,&result)` already decodes
  all ops (COPY_PROBE, HANDLE_*, ENDPOINT_*, PROC_*, DEV_*). BUT the handle/copy GLUE
  (`je_syscall_dispatch`, lines ~751+) is x86-`je_trap_frame`-typed (reads rax/rdi/.. and calls
  je_copy_*). ARM currently maps only abi_query/yield/exit and returns UNSUPPORTED for the rest.
- ARM SVC path `kernel/arch/aarch64/sched_arm.c je_arm_el0_svc` (~1054): validates frame, checks
  ESR EC=0x15/IL=1/imm=0, copies x0-x5 -> args, x8 -> op, calls je_syscall_decode, then only acts
  on RETURN/YIELD/EXIT, else UNSUPPORTED. x0=status, x1=result.
- ARM arena base 0x0000_1000_0000_0000 (2^44); `vm->ops->read_user_pte` = arm_read_user_pte
  (vm_backend.c:471). Per-process htable lives in je_vm_process (vm.h). ARM current-user query is
  `je_arm_sched_current_user(vm,index,slot)` — a DIFFERENT signature from core
  `je_sched_current_user(vm,index)` that usercopy.c + current_htable() call.

## The hard part: ARM fault handler must become RETURNABLE
A usercopy fault is a KERNEL-mode (EL1h) data abort: ESR.EC==0x25, vector==4 (current EL, SP_ELx,
sync). Today slot-4 sync -> `je_arm_vec_common` -> `bl je_arm_exception` which is `_Noreturn`
and halts (`for(;;) wfe`); vectors.S also halts if it ever returns (~lines 147-150). ADR-0015's
EL1h validator row (SPSR (spsr&~NZCV)==0x345, ELR in kernel text) was built assuming EL1h-sync
never returns. MP3b must make EXACTLY the copy-site case return via the restore seam, everything
else stays fatal. The reconciled design (debates/05:125-126) says: copy-fixup is EL1h-ONLY,
gated on ESR.EC==0x25 && M==EL1h && vector==4; debates/01:152 suggests LDTR/STTR (unprivileged
load/store) so the copy is checked with EL0 permissions.

## Proposed MP3b design (critique every point)

### D1 — ARM copy-site asm (new kernel/arch/aarch64/usercopy_arm.S)
`int je_copy_user_run(void *dst, const void *src, uint64_t len)` byte loop, leaf, no frame.
Use `LDTRB`/`STTRB` (unprivileged) so the access is checked at EL0 permission even though we
run at EL1 (defense-in-depth; compensates for no PAN). Exact labels
`je_copy_user_load_site`/`je_copy_user_store_site`/`je_copy_user_fixup` (fixup sets x0=1; ret).
QUESTION: leaf-with-ret works on x86 because the handler rewrites RIP and ret unwinds. On ARM
the fault handler returns through the vectors restore seam (ERET to the rewritten ELR). Does
rewriting tf->elr = fixup label + ERET correctly resume a leaf with x30 intact? (x30 untouched
by the copy loop, so ret after fixup returns to the C caller — confirm.)

### D2 — portable scope core, arch hook for the fault predicate
Keep usercopy.c's scope + prevalidation PORTABLE. Factor the arch-specific bits behind a small
hook: the x86 keeps its PFEC/CR2/RIP predicate; ARM adds a DFSC/WnR/FAR/ELR predicate
`je_usercopy_try_fixup_arm(je_arm_trapframe*)` that recovers ONLY IF: vector==4, (spsr&0x1f)==0x5
(EL1h), EC==0x25, DFSC is a TRANSLATION fault only (0b0001xx — the P=0 analogue; permission/
access-flag/external stay fatal), WnR matches direction, ELR EXACTLY == load/store site, FAR in
[user_lo,user_hi), scope owner == current. `canonical()` in usercopy.c is x86 47-bit — must be
made arch-neutral or arch-provided (ARM arena is at 2^44, canonical differently). PROPOSE:
replace canonical() with the existing span-in-arena check (already arch-neutral via
JE_VM_ARENA_BASE/SIZE) and drop the x86-canonical assumption, OR arch-hook it. Which?
QUESTION: LDTR/STTR fault with WnR per the actual access; does an unprivileged store to an
RO user page raise DFSC permission (fatal) or does prevalidation already exclude it (span_pages_ok
requires RW for writes)? Confirm the translation-only recovery can't mask a perm bug.

### D3 — current-user shim
usercopy.c + current_htable() call core `je_sched_current_user(vm,index)`. ARM provides
`je_arm_sched_current_user(vm,index,slot)`. PROPOSE: add an arch-neutral `je_sched_current_user`
impl for ARM (thin shim dropping slot, or a shared hook) so the SAME usercopy.c/syscall-glue
runs on both. QUESTION: is there a reason x86 and ARM current-user must stay separate, or is a
shared signature clean?

### D4 — factor the handle/copy syscall glue out of x86 frame
Extract the handle-op glue in syscall.c (~803-970: HANDLE_SELF/CLOSE/QUERY/RIGHTS/DUP, COPY_PROBE)
into frame-NEUTRAL helpers taking (args[6], &status, &result) — NOT je_trap_frame. x86
je_syscall_dispatch and ARM je_arm_el0_svc both call them. This is the ADR-0015-D2 "thin glue ->
pure decode" pattern extended to handles. The 2-handle-packing for endpoint_create (x86 packs
two 32b handles into RDX) maps to x1 on ARM (x0=status, x1=result 64b) — same packing, fine.
MP3b scope = handle ops + copy_probe; ENDPOINT/IPC is MP3c, leave UNSUPPORTED on ARM for now.
QUESTION: any op whose result doesn't fit the single x1 result register, or needs a second
output the x86 path delivered via a different register? Audit HANDLE_DUP/QUERY/RIGHTS returns.

### D5 — the owner-gated escalation (frozen/tested path)
Making EL1h-sync returnable + extending the ADR-0015 EL1h validator to accept a returning
copy-fixup path is a FROZEN-CONTRACT + TESTED-PATH change => must be escalated to the owner as
options BEFORE writing, per the workflow. Draft the escalation: Option A = a dedicated minimal
returnable branch in je_arm_vec_common that calls the fixup predicate and ERETs on recovery,
leaving je_arm_exception _Noreturn for all other cases (smallest blast radius, the validator row
gains exactly one accepted returning shape). Option B = generalize je_arm_exception to return an
int and let vectors decide (larger change to a tested path). Recommend A.

### D6 — the proof (ARM analogue of the x86 copy_probe + dev inject)
An ARM EL0 test (extend el0_test.c or a sibling) that: a user process does copy_probe round-trips
(src->kernel->dst), proves copy_from/to_user move exact bytes across the EL0/EL1 boundary; a
not-present arena page triggers the fixup and recovers to INVALID (the dev-inject analogue, guard
JE_DEV_COPY_INJECT); a handle round-trip (self/dup/rights/query/close with a stale-handle reject);
and NEGATIVE mutants: drop the DFSC-translation-only guard (a permission fault must then be
mis-recovered => the mutant must FATAL), drop the exact-ELR match (range match must be caught),
drop the owner-match, drop the direction-match. Each negative stage MUST be proven to FATAL by
injection (MP3a-2 lesson: a negative stage you never made fail is not a proof).

## Your task (adversarial design review)
For EACH of D1-D6: is it correct, minimal, and does it preserve the frozen EL1h path? Find the
hole. Specifically: (1) Is making slot-4 EL1h-sync returnable safe — can a NON-copy EL1h data
abort ever be mis-recovered? What exactly guards that? (2) Is LDTRB/STTRB the right choice vs
plain LDRB/STRB — does it actually add EL0-permission checking and does it change the DFSC we
must match? (3) Is translation-fault-only (DFSC 0b0001xx) the correct P=0 analogue, and does it
leave any recoverable-but-should-be-fatal case? (4) the double-fault guard (je_arm_in_handler):
does a returning copy-fixup interact badly with it? (5) anything in D4's frame-neutral extraction
that breaks the x86 tested path? Give a VERDICT per decision: OK / FIX / ESCALATE, with the
exact concern. Be concrete and adversarial; cite file:line where you can.

## Slack
You may post your own design notes / concerns directly to the agent group channel via:
curl -s -X POST "SLACK_WEBHOOK_URL_INJECTED_AT_DISPATCH" -d '{"Text":"[codex|claude] <your note>"}'
(the lead will inject the real URL). Keep it to substantive design findings.
