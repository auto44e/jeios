**Verdict: FIX FIRST.** There is one blocker. The per-process audit can report PASS while kernel text is EL0-writable. Everything else is a should-fix or a nit. I did all scratch work in `/tmp/mp3a1-mut-*` (tooling in `/tmp/mp3a1-tools`); the worktree is untouched. Baseline `ARM_USER_TEST` smoke reproduces your values.

## Confirmed (your items)
- **L0 slots are disjoint:** the arena is at L0[32]/L1[0]/L2[0] (static-asserted). The kernel (0x40080000), PL011 and GIC are all in L0[0]. The audit enforces both rules: process L0[i] equals kernel L0[i] for every i≠32, and kernel L0[≥32] is invalid.
- **Hierarchical deny:** the decoration is applied to the kernel root itself before it is copied (observed kernel L0 = `0x3000000040001003`). I added a positive control: I forced a kernel leaf to AP[1]=1. With APTable=01, AT S1E0R still faults (`0x81f`) and EL1 S1E1W still succeeds. With APTable cleared, the S1E0R succeeds. So APTable is what denies EL0, and it does not restrict EL1. PXNTable and UXN are encoded correctly, but EL0 execute permission cannot be observed through AT; it rests on the static asserts only.
- **ASID:** a TTBR0 readback gives `0x0001000040106000`. TCR.A1=0 (ASID comes from TTBR0) and AS=0 (8-bit ASIDs). The order MSR, ISB, ASIDE1IS, DSB ISH, ISB has the right scope and order by construction.
- **Frozen paths:** `trapframe.h` and `vectors.S` are untouched, and there is no `eret` or SP_EL0 code. `je_arm_mmu_build` is unchanged, and `je_arm_mmu_audit` only gained braces. It now accepts the USER literals, but its explicit AP[1] check still rejects any EL0 leaf.
- **Table cap:** 134 kernel tables against a cap of 256. Per-process tables come from the allocator and don't count toward the cap. The alias scan does 17.3M reads (about 80 ms under TCG), and all loops are bounded.

## BLOCKER
**B1 — The ARM audit drops two x86 audit rules and gives a false PASS** (`vm_backend.c:547-604`, `vm.c:430`). On aarch64 `je_vm_audit` is replaced entirely by `je_arch_vm_audit`. That replacement checks only L1[0] and L2[0]. It never checks that the other upper-level entries are empty, a rule x86 added to close the stale-table-frame disclosure hole. It also never checks that the guard page is unmapped.
- **Stray block in arena L1[1] or L2[1]:** I injected an EL0-RW block in each. `AT S1E0W` at arena+1 GiB+0x80000 returned `0xff00000040080b80`, meaning kernel text is EL0-writable. Audit PASS, smoke PASS.
- **Stack shifted down one page:** I made `install_pte` place the stack one page lower. The guard page then translates (`0xff0000004010db80`), the top stack page is unmapped, and the audit and smoke still PASS.
- **Fix:** require L1[1..511] and L2[1..511] to be invalid, and the guard L3 entry to be invalid. Also check that each arena table pointer is the process's own table-extent page and lies in the physical window (currently dereferenced unchecked at lines 562-572).

## SHOULD-FIX
1. **The W^X alias is not enforced; it only holds because nothing maps the frame** (`vm_backend.c:280-290`, `paging.c:987-1003`). `arm_demote_alias` writes a private byte that the audit then reads back, so that check proves nothing. The paging.c W^X ledger never learns about user frames. When I called `je_arm_mmu_map_page(ctx, code_pa, code_pa, JE_MAP_RW)` on p1's USER_CODE frame, it returned 0, and S1E1W then gave `0xff00000040109b80`: an EL1-writable alias of an EL0-executable frame. ADR D1 says "the W^X ledger entry is what makes a later RW alias refuse"; that is not implemented. This must be fixed before the scheduler and user processes coexist in MP3a-2.
2. **The oracle can't fail on the kernel probes** (`smoke_arm.py:937-938`, `main.c:922-936`). It checks only PAR.F, not the fault status, and the marker doesn't print the probed VA. When I pointed the "kernel" probes at unmapped VAs (0x1000 and 0x2000), they returned `0x80d` (translation fault) and the smoke PASSed.
   - Print the VA and check it against the ELF symbols.
   - Require FST = permission fault at level 3 (`0x81f`) using the existing `par_expect_fault` helper.
   - Add device probes, which ADR D4 lists. UART and GICD give `0x81f` in my run.
   - Add a TTBR0 readback marker. Removing the ASID from TTBR0 survives today.
3. **The claimed QEMU TLB behaviour is false** (`sched_arm.c:446-456`, `IMPLEMENTATION.md:40`). The comment and docs say VMALLE1IS is kept because of "observed QEMU same-ASID proof behavior". It doesn't reproduce: removing VMALLE1IS still PASSes on the same QEMU 8.2.2, and so does removing both TLBIs. Fix the claim; keep VMALLE1IS only with an honest rationale, or drop it.
4. **No mutants, though ADR D4 MP3a-1 requires them.** `tools/mutants.py` has no ARM user-test entries.
   - Killed: skip-decorate (FATAL audit-p1), wrong root for p2 (FATAL probe-mismatch).
   - Survived: stray L1/L2 entry, guard shift, unmapped kernel probe, no TLBI, no ASID in TTBR0, and skip-decorate combined with an always-pass audit.
5. **The dispatch seam is dead code and has no unbind** (`sched_arm.c:461-474`). `je_arm_sched_bind_user_root` is never called; the proof calls `install_ttbr0` directly. So `IMPLEMENTATION.md:62` ("binds a user root to a slot for proof-harness installation") is wrong. Nothing clears a slot's root when the slot is reused. A stale root would install freed table frames, which could later be reallocated as EL0-writable data.
6. **The backend only works with the MMU off.** `arm_flush` and `arm_unmap` do nothing, and `clear_pte` does no TLBI, so the journal's "clear → flush" contract isn't met. All access goes through `phys_ptr`, but process tables and frames aren't mapped once the MMU is on (S1E1R/W on every p1 frame gave `0x80f`). Exit and reap in MP3a-2 will data-abort. Record this and fail closed.
7. **Cache maintenance is missing for real hardware (reviewed, not observable on QEMU).** Process tables and user frames are written with the MMU off at 0x40100000 and above. That is outside `enable_leaf`'s `dc ivac` ranges (kernel tables [0x40000000, 0x40100000) and the image), which MP2 invalidates for exactly this reason. This isn't documented anywhere.

## NIT
- **`vm_backend.c:48`:** the "independent literals" comment is wrong. `install_pte` and the audit use the same `ARM_USER_*_BITS`. paging.c holds a second copy of the literals that nothing ties to the first, plus USER encoder branches only the hosted test uses.
- **Decoration (`vm_backend.c:214`, `:232`):** `decorate_kernel_root` can stop partway with no error, and `link_tables` carries on (the audit catches it later). Decorating lazily would become a live, unsynchronized table change if a process were built with the MMU on. Decorate once, explicitly, right after the MP2 audit.
- **`vm_backend.c:254`:** the root is truncated to `uint32_t`. Add a `_Static_assert` that `JE_PHYSICAL_LIMIT` is at most 2^32.
- **`vm_backend.c:405`:** `arm_read_upper` decodes leaf AP/XN bits out of table descriptors, which is meaningless, and it is unused on ARM.
- **Docs:** "128 MiB managed RAM window" should be 256 MiB.
- **Asymmetric in-kernel check:** the final check re-probes under p2, so p1's permissions are checked only by the oracle.
- **Small items:**
  - The ASID parameter isn't bounds-checked against 8 bits.
  - The `je_arm_tlbi_hosted_*` counters are now real-build globals but keep the "hosted" name, and their meaning is inconsistent.
  - The MP2 audit's stack frame grew to 6.3 KiB on the 16 KiB unguarded MMU-off stack (fine for now).
  - The alias scan grows quadratically with the RAM window.

## Item 7, definitively
In MP3a-1 as committed, **no**: a user code or rodata frame cannot be reached writable from the kernel. All 11 p1 frames (tables, code, rodata, data and stack) translation-fault at their identity VA for both S1E1R and S1E1W (`0x80f`). Through the arena VA, EL1 gets a permission fault on writes to code and rodata (`0x81f`). That holds only because there is no RAM linear map and the build runs with the MMU off; nothing enforces it. A single `je_arm_mmu_map_page(..., RW)` succeeds and creates the writable alias (should-fix 1).
