# MP3b adversarial design review — verdict: FIX FIRST

The design can't be implemented as written: there are three blockers, and D4/D5 need owner escalation in a different form than drafted. I tested the claims by injection in scratch copies under `/tmp/mp3b-review`, made with `git archive` from 0cfcde0. That was 12 boots on QEMU 8.2.2 / cortex-a72; the worktree was not touched. This is AI-assisted review, not human qualification. I did not post to Slack: the brief says the URL will be replaced, and this is an outside service.

| D | Verdict | Core issue |
|---|---|---|
| D1 | **FIX** | Two routines; LDTRB/STTRB only for the user-side access; no x16–x18 |
| D2 | **FIX** | Require DFSC exactly 0x07 and an exact ISS[10:0]; SPSR is 0x3c5; add a live TTBR0 check; `canonical()` can stay |
| D3 | **OK** (with conditions, N4) | |
| D4 | **ESCALATE** | The glue is compiled out on ARM, pulls in endpoint/device code, and touches tested x86 code |
| D5 | **ESCALATE** (reworked, below) | Option A is unreachable, and its natural return path clears the guard |
| D6 | **FIX** | The oracle logic is inverted, and each check needs an input that only it rejects |

## BLOCKERS

**B1 (D5, Q4) — the fixup can never run.** `je_arm_el0_sync_save` sets `je_arm_in_handler=1` for the whole SVC (`vectors.S:370-375`). `je_arm_vec_common` checks that guard before anything else (`vectors.S:90-93`), so every copy-site abort is treated as a double fault.
- **Injection c1:** an LDTRB from unmapped `ARENA+0x1000` inside `je_arm_el0_svc` prints `FATAL double fault; halted`. No syndrome is printed and no C code runs.

**B2 (D5, Q4) — the obvious return path breaks the double-fault guarantee.** `je_arm_restore_selected` clears the guard to 0 (`vectors.S:246-249`).
- **Prototype c9** (guard bypass on an exact ELR match, then return via `restore_selected`): the fixup works. It returned r=1, and x30=`0x400899ac` is the instruction right after the `bl`, which answers D1's question. But `guard_after=0` for the rest of the SVC.
- **Case c10:** a second fault at a different instruction in the same SVC then goes to C `je_arm_exception`, which uses the stack, instead of the stack-free double-fault halt.
- The c9 smoke still **PASSES**, so today's proof cannot detect this.

**B3 (D1, Q2) — one shared LDTRB/STTRB loop cannot work.** The kernel-side access is also checked with EL0 permissions.
- **c5:** STTRB to a kernel .bss byte gives `esr=0x9600004f` (permission fault, L3, WnR=1).
- **c6:** LDTRB from a kernel byte gives `0x9600000f`.
- So `from_user` would fault on its kernel store, and `to_user` on its kernel load.
- **Fix:** `je_copy_from_user_run` uses LDTRB for the user read and STRB for the kernel write. `je_copy_to_user_run` uses LDRB for the kernel read and STTRB for the user write. That gives one exact user site per direction, so direction is fixed by construction (keep the scope's direction check too).

## SHOULD-FIX

- **S1 (D5) — don't widen the ADR-0015 EL1h validator row.**
  - The nested copy frame's SPSR is `0x800003c5` (c2), not 0x345.
  - `frame_shape_class` validates frames the scheduler resumes. Widening its 0x345 policy to also accept 0x3c5 (scratch w1) lets the tested `spsr-i` mutant get past validation:
    - baseline w0: `FAIL as expected … FATAL sched proof frame validation`;
    - widened w1: the run times out, and a timeout is not evidence.
  - The copy path needs its own exact check and no scheduler involvement.
- **S2 (D2, Q3) — DFSC: require exactly 0x07 (translation fault, level 3), not `0b0001xx`.**
  - Each process's arena L0→L1→L2 chain is built at create time and never changes (`vm_backend.c:300-302`). An in-arena translation fault at L0–L2 therefore means a wrong or corrupt TTBR0, and must be fatal.
  - Match ISS[10:0] exactly: 0x007 for loads, 0x047 for stores, which forces FnV=EA=CM=S1PTW=0. Require IL=1. Ignore ISS[24:11], which depends on ISV (QEMU reports ISV=0).
  - Existing code already does this: `mmu_fault_check` (`main.c:283-288`).
  - Access-flag faults can't occur legitimately, because user leaves set AF (`paging.h:63-65`), so keeping them fatal is right.
- **S3 (D2, Q1) — the owner check doesn't catch a wrong live root.**
  - `arm_read_user_pte` walks the process's own L3 table in software (`vm_backend.c:483-488`), not the live TTBR0.
  - With another process's root installed, a fault in that root's arena (L3 translation fault, FAR inside the span) is quietly turned into INVALID. A page mapped there means a silent cross-process copy.
  - **Fix:** in ARM `copy_core`, check `TTBR0_EL1.BADDR == je_arm_user_root[slot]` (one `mrs`; fatal on mismatch).
- **S4 (D1) — x16–x18 are not preserved across a returning slot-4 exception.**
  - The frame holds the vector index and guard scratch in those slots (`vectors.S:135-139`). In c9 the frame had x16=4, and the 0x1616/0x1717/0x1818 canaries were destroyed.
  - Copy routines must use only x0–x3 plus x30 and stay leaf functions. Enforce this with a `tools/` grep check on the `.S` file.
- **S5 (D4)**
  - ARM compiles `syscall.c` with `-DJE_SYSCALL_HOSTED_TEST` (`mk/aarch64.mk:228`), so the whole glue (`syscall.c:213-1207`) is compiled out on ARM.
  - The typed dispatch pulls in `je_endpoint_kern_close`/`dup`/`is_wired`, `je_device_close_matches`/`revoke_close` and `je_sched_current_proc_id`. None of these are linked on ARM (`aarch64.mk:187-190`).
  - The x86 glue has only QEMU/integration evidence (it isn't hosted-tested), so extracting it is a tested-path change and goes to the owner.
  - **Recommendation:** for MP3b, ARM-local SELF-only glue. Any other type fails closed (WRONGTYPE); on ARM only SELF objects can exist and `vm->teardown` is NULL. x86 stays untouched; converge later with byte-identical x86 `objdump` evidence.
  - **Answer to the D4 question:** no op needs a second output register. Every glue path writes only RAX and RDX (grepped), and the two 2×32-bit packings (endpoint_create, try_recv) fit in x1.
- **S6 (D6) — proof oracles.**
  - (a) "Drop a check, the mutant must FATAL" is backwards. Removing a check makes the injected fault *recover*. The correct build must halt fatally with an exact syndrome line, and the mutant must turn that into a smoke FAIL. Use one build per fatal-expected stage, as `ARM_MMU_FAULT` does.
  - (b) Each negative needs an injected state that *only* its target check rejects (pattern: `je_arm_el0_test_mode_rejection`):
    - ELR = site+4 with everything else valid;
    - FAR = the span's upper bound;
    - owner mismatch;
    - a load-site fault while the scope direction is to-user;
    - STTRB to the read-only code page at `ARENA+0` (c4 gives `0x9600004f`).
  - (c) Assert guard==1 after a recovery, and add a mutant that returns through `restore_selected`. c9 shows the smoke can't see it otherwise.
  - (d) A hosted test matrix for the ARM fault check, mirroring `tests/test_usercopy.c` and flipping one field at a time.
  - (e) Positive copies:
    - use distinct non-zero byte patterns;
    - include a span that crosses a page, and lengths 0, 256 and 257;
    - include the hole at `ARENA+0x1000`, counting fixups so a prevalidation rejection is distinguishable from a recovered fault.

## NITs

- **N1** `canonical()` checks nothing extra on ARM but is harmless (2^44 >> 47 == 0). The arena check is strictly stronger on both arches. Leave it as is (zero x86 diff). Add static asserts that the arena end is ≤ 2^47 and ≤ 2^(64−T0SZ); TBI0 is already 0 (`paging.c:79-83`).
- **N2** `arm_read_user_pte` is not read-only. `identity_page` maps and unmaps the L3 table page in the shared kernel map (`vm_backend.c:145-162`): page-table writes plus a TLBI for every prevalidated page, and `arm_fail_stop` on failure, all inside the SVC. Document it, and exercise two-page spans.
- **N3** `el0_test.c:573-576` reads a user address with a plain load in the SVC hook, which breaks the "single approved site" claim. Convert it to `je_copy_from_user` or exclude test code explicitly.
- **N4** (D3 conditions) The shim must also check `je_arm_rr_ready` (x86 checks `ready`, `sched.c:638`), accept NULL out-pointers as x86 does, and reject `vm==NULL` or `index>=JE_VM_MAX_PROCESSES`, because `current_htable` doesn't check either (`syscall.c:315-320`). Otherwise it's clean: `sched.c` isn't linked on ARM, so there's no duplicate symbol.
- **N5** Record the target-profile assumptions:
  - cortex-a72 has no PAN and no UAO. LDTR is EL0-checked only while UAO=0, and PAN doesn't affect LDTR/STTR, so the design stays correct if PAN is enabled later.
  - `SCTLR.SPAN=1` (`boot.S:111-113`).
  - No hardware access-flag/dirty-bit management (HAFDBS).

## Answers to Q1–Q5

- **Q1 — what guards against a non-copy abort being recovered:**
  1. an asm prefilter that requires an exact ELR match before any stack use;
  2. guard == exactly 1, so the fault is nested exactly once inside an EL0 SVC;
  3. a C check for vector 4, SPSR exactly 0x3c5, EC 0x25, an exact ISS[10:0], site matching direction, FAR within the span, owner, and (per S3) the live TTBR0.

  With S2 and S3 applied, I found no remaining false recovery while kernel state is intact.
- **Q2 — LDTRB/STTRB:** yes, for the user side only. The DFSC for an unmapped page is the same as with plain loads (c2 and c7 both give `0x96000007`). LDTRB also turns a span-check bug into a fatal permission fault instead of a kernel-memory leak.
- **Q3 — DFSC:** translation fault at L3 only (S2).
- **Q4 — double-fault guard:** B1 and B2.
- **Q5 — D4 risk to x86:** S5.

## Reworked D5 escalation (Option A′, recommended)

1. **Entry.** On the guard-hit branch, run a stack-free asm prefilter: `ELR_EL1 ∈ {from_site, to_site}` and guard == 1. Otherwise go to `je_arm_double_fault`, unchanged for every other case. Set guard=2 before building the frame, so a fault inside the check itself becomes a double fault.
2. **Check.** Build the frozen frame and call `je_arm_copy_fault(tf)`. On reject, use the existing fatal diagnostic; on accept, set `elr := fixup`.
3. **Return.** Use a dedicated restore tail:
   - assert x0 == sp, `elr == fixup`, and `(spsr & ~NZCV) == 0x3c5`;
   - set guard := 1 (restore, never clear);
   - restore the GPRs, `add sp, #304`, `eret`.

   It does not use `restore_selected`, does not change `frame_shape_class`, and `je_arm_exception` stays `_Noreturn`.
4. **Owner gates.** The MP1 rule "handlers are non-returning; a nested synchronous fault is always a double fault" gains exactly one exception, and the guard becomes three-state. The other savers test it with `cbnz`, so they are unaffected.

Option B (making `je_arm_exception` return) has a larger blast radius; reject it.

To reproduce: `python3 -I /tmp/mp3b-review/scripts/inject.py <dir> <case 1-10>` patches a scratch copy, then `/tmp/mp3b-review/scripts/run_case.sh <dir>` builds and boots it. Serial logs are in `c*/build/aarch64-el0test/serial.log`.
