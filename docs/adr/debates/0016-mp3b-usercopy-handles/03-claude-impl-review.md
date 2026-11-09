**VERDICT: FIX-FIRST.** The fixes are all test and evidence gaps. The mechanism itself held under every injection, and I found no way to mis-recover an EL1h abort. But six security-relevant claims in the proof can't currently fail, so the gate's "load-bearing" claim is overstated. The worktree is untouched (clean at 5b338b7); all experiments ran in `/tmp/mp3b-rev`.

## SHOULD-FIX

1. **The `copy_core` live-root assert has no test** (`usercopy.c:154-155`).
   - **Injection:** `if (0 && !arm_live_root_matches(...))`. The positive smoke and all 8 stages still PASS (9/9).
   - **Demo:** from process 0's SVC, install process 1's root, then call `je_copy_from_user(data)`.
     - With the check: `FATAL sched copy live root`.
     - Without it: `st=0x0 v=0x2222`, a silent read of process 1's data.
   - The live-root stage and the `ARM-MP3B-LIVE-ROOT` mutant only exercise the predicate's own live-root check, through the inject seam, which bypasses `copy_core`.
2. **The stack-free asm exact-ELR prefilter has no test** (`vectors.S:135-143`).
   - **Injection:** replace the store-site `b.ne je_arm_double_fault` with a nop. The positive smoke and all 8 stages still PASS (9/9).
   - Without the prefilter, a stray in-scope LDR becomes `FATAL copy fault fixup rejected`, taken on the stack-using C path, instead of `FATAL double fault`. It still halts, but D1's stack-free guarantee is no longer tested.
   - The exact-elr stage only changes `tf->elr` inside C, after the prefilter has already run.
3. **The 257-byte probe round proves nothing** (`el0_test.c:93-94`, `:508-512`).
   - dst − src = 0xb00 = 11×256, and the fill pattern repeats every 256 bytes, so the destination already holds the expected bytes before the copy.
   - Making the probe copy nothing when len==257, or dropping the 257th byte, gives smoke PASS. A control that makes `copy_to_user` a no-op for every length is caught by the 256 round (`copy-probe-bytes`).
   - `abi.h:91` documents COPY_PROBE as ≤256, but ARM now accepts 257 (`JE_COPY_PROBE_MAX`, `usercopy.c:310`). That makes the same op differ between architectures, and it isn't documented.
4. **The restore-selected stage can never fail** (`el0_test.c:720-727`, `:808-809`).
   - Replacing the mutant tail with the correct `add sp; eret` still gives `FATAL el0-test copy-guard-cleared` and smoke PASS.
   - The base positive proof is what actually protects this property: a real `restore_selected` tail in the positive build makes smoke FAIL.
5. **Translation-only is tested only for stores** (`usercopy.c:257`).
   - **Injection:** `is_load = (iss & ~0x8) == 0x007`. The positive smoke and all 8 stages PASS (9/9).
   - **Demo:** an LDTRB of kernel VA 0x40080000 at the exact load site gives ISS 0x00f. The correct build rejects it as fatal; the weakened build silently recovers it.
6. **`check_arm_usercopy_boundary.sh` misses most forbidden forms.** I planted 11; it missed 8: `ldr x16`, `sub sp`, `bl`, `blr`, `orr x17`, `ldrh w18`, `mrs x17`, `eor x18`. It only inspects lines whose mnemonic is on its list.

## NIT

- **FAR lower bound untested** (`:276`): removing it survives 9/9.
- **`vectors.S:124-129` comment is false.** A fault on the `saved_gprs` store livelocks instead of double-faulting: 392,260 nested aborts in 6 s with no marker. This needs kernel `.data` to be unmapped already. Fix by moving the ELR compare before the save.
- **3 of 6 mutant killer texts don't match the actual kill line:**
  - TRANSLATION-ONLY actually dies on `copy-negative-recovered`.
  - DIRECTION actually dies on `copy-negative-returned`.
  - GUARD-3-STATE actually mutates the guard store, and the assertion is what kills it. When I weakened the guard from 2 to 1 and removed the assertion, a nested site fault inside the check re-entered C and recovered. So guard state 2 is load-bearing, but no shipped test shows it.
- **ADR wording doesn't match the code:**
  - The ADR says recovery happens only "inside an EL0 SVC", but guard==1 covers any returning handler. An inject from the IRQ hook recovered.
  - The ADR says the predicate compares ISS[10:0], but the code compares ISS[24:0]. That is stricter, so only the doc is wrong.
- **Wrong recorded commit:** `HANDOFF.md:6` and `IMPLEMENTATION.md:8` cite 59cf62c, a dangling pre-amend commit that is not an ancestor of HEAD.
- **Not disclosed:** the hosted test matrix asked for in the design review wasn't built, and the gaps above are missing from the handoff's "not enforced" list.
- **Smaller items:**
  - The EL0-build slot-4 veneer overwrites x0–x2 in the guard==0 fatal frame (diagnostic only).
  - The proof uses `JE_SYSCALL_*` enums as SVC op numbers, with no static assert tying them to `JE_SYS_*`.
  - A TPIDR-poison change to the MP3a-2 proof isn't listed in the handoff.
  - Copy-fault smokes have no early stop, so every kill waits the full 60 s.

## Tried and could not break (each injected)

- **Non-copy faults:** a stray in-scope LDR, a fault at site+4 (kernel STRB) and at site−4 (kernel LDRB), an LDR or BRK after a recovery, and faults inside the C check (guard 2), including one at the exact site, all gave `FATAL double fault`. An exact-site fault with guard==0 gave the ordinary fatal path.
- **Restore tail:** a wrong frame pointer, ELR=fixup+4, SPSR with I unmasked, and SPSR EL1t each double-faulted. A tail that cleared the guard to 0 or left it at 2 made the positive smoke FAIL.
- **Registers:** canaries x4–x29, x30 and SP came through recovered load and store faults intact. The harness detects corruption of x16 and x17.
- **Handles:** non-SELF types 2, 3, 4, 5 and 9 get WRONGTYPE on all four ops and stay live after a failed close. Dup cannot escalate rights (DENIED or INVALID). Stale handles are rejected on all four ops, and quota exhaustion fails closed.
- **User-reachable faults:** 12 malformed `copy_probe` spans all return INVALID, with no faults taken.
- **x86 untouched:** the full `jeios.elf` disassembly is identical to pre-MP3b. The non-EL0 ARM Images (default, timer, sched, user, vector) are byte-identical.
- **Suites:** the 8 stages reproduce, and all 6 shipped mutants are KILLED. The full manifest gave 61 killed, 1 equivalent, 0 survived. `make check` PASS.

Not observable under QEMU TCG: walk-cache and TLB behaviour of the l0 stage on real hardware.

**Effort:** 75 injection scenarios, each a fresh copy, build and QEMU boot (50 also scored by `smoke_arm.py`); 10 baseline boots; the 6 MP3b mutants plus the full manifest; 12 builds for the identity checks; 15 planted forms against the boundary checker. I posted 7 Slack messages (findings 1–6 and the verdict). This is AI-assisted review, not human qualification, and local QEMU only, with no remote CI.
