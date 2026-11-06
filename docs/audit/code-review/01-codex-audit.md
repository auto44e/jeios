Reviewed actual source at:

- Branch: `main`
- Commit: `c6914ca9176b360ded82ed70c0884fafddbaf0e6`
- Worktree: one untracked `tools/mutants.py`; not treated as committed code.
- I did not rerun tests in this review. `docs/HANDOFF.md` records prior `ASAN_OPTIONS=detect_leaks=0 make check` success, while plain LeakSanitizer was environment-blocked.

Overall: the FDT parser, VM transaction ordering, allocator bounds checks, handle generation logic, ARM page-table audit, and GIC fake model are unusually strong. The largest risks are real-hardware assurance gaps and a few safety contracts that are distributed instead of enforced at one boundary.

## (A) Correctness and safety issues

### A1. Nonzero usercopy with a NULL kernel buffer can fault the kernel

`kernel/core/usercopy.c:99-106`

`copy_core()` validates the user span and pages but never rejects `kptr == NULL` when `len != 0`. The assembly then dereferences kernel address zero.

Why it matters: this violates the public “return `JE_ABI_INVALID`” boundary and can turn malformed internal/caller input into a fatal kernel page fault.

Fix:

```c
if (len == 0) return JE_ABI_OK;
if (kptr == NULL) return JE_ABI_INVALID;
```

Add direct tests for `je_copy_from_user(NULL, valid_user, 1)` and `je_copy_to_user(valid_user, NULL, 1)`. This is an owner-gated proposal because usercopy is tested M3/M4 code.

### A2. Multiboot metadata itself is not proven to reside in RAM

`kernel/core/boot.c:21-33`

The parser reserves `[physical, physical + total)` but never verifies that the multiboot block lies in type-1 RAM. It validates the kernel image later, but not the source block.

Why it matters: the contract says loader data is reserved and only RAM is usable. A valid result can therefore claim trusted loader metadata from MMIO/reserved space.

Fix: after parsing the memory map, validate every page containing the boot block with a checked “range fully inside one type-1 RAM entry” helper. Add a test with `physical` located in a non-RAM map entry.

### A3. AArch64 page-table audit can recurse forever on a table cycle

`kernel/arch/aarch64/paging.c:450-480` and `:482-513`

`audit_alias_scan()` and `audit_table()` recursively follow table descriptors. `table_frame()` membership is checked, but there is no visited-table set. A tampered table can point back to the root or another ancestor.

Why it matters: the audit is intended to fail closed. A self-cycle instead causes unbounded recursion/stack exhaustion or a hang.

Fix: use a `JE_ARM_MAX_TABLES` visited bitmap, or replace recursion with a bounded explicit work stack. Reject repeated table frames. Add a hosted test that makes `root[0]` point back to `root` and requires `JE_ERR_INVALID` within a timeout.

This is an owner-gated AArch64 paging proposal.

### A4. The VM flush contract is not implemented by the real x86 backend

`kernel/core/vm.c:150-170`  
`kernel/arch/x86_64/vm_backend.c:291`

The VM transaction explicitly requires:

> clear PTE → flush → restore alias/free

But `vb_flush()` is a no-op:

```c
static void vb_flush(void *ctx) { (void)ctx; /* single BSP, CR3 reloads */ }
```

Why it matters: the current no-PCID, inactive-root lifecycle probably masks this because later CR3 loads flush translations. However, the backend does not implement the abstraction’s stated immediate-invalidation contract. This becomes unsafe with same-root reuse, PCID, altered scheduling, or another teardown path.

Fix: make invalidation explicit in the backend contract. Options include:

- pass the affected root/range to a real invalidation operation;
- perform `invlpg` for the active root and require a CR3 reload before root reuse;
- explicitly encode the no-PCID lifecycle precondition and test it in QEMU.

Add a real stale-PTE/reuse test. The hosted fake’s flush counter does not prove hardware invalidation.

### A5. Zeroization is not enforced at the allocator boundary

`kernel/core/alloc.c:77-93`, `:124-134`  
`kernel/arch/x86_64/vm_backend.c:231-237`

`je_pages_free()` returns raw frame contents to the allocator. VM-owned frames are scrubbed by the x86 VM backend, and heap allocations are zeroed on allocation, but generic allocator ownership does not enforce “zero before share.”

Why it matters: a future consumer can allocate/map a freed kernel page before zeroing it. The security invariant is currently spread across callers.

Fix: define an explicit ownership policy:

- scrub before every release, or
- zero at every mapping/share boundary with a mandatory API, or
- introduce allocator release classes requiring a scrub callback.

Add a cross-owner test: write secret bytes, free through each relevant owner, reuse as user/page-table/heap memory, and assert zeroes.

### A6. x86 paging initialization trusts `ctx->valid` but not its counts

`kernel/arch/x86_64/paging.c:20-29`

`je_arch_paging_init()` checks `ctx->valid` but directly loops over `ctx->map_count`. `je_pages_init()` separately bounds-checks the count, but paging initialization does not.

Why it matters: a manually constructed or corrupted “valid” context can make the loop read beyond `map[]`. The normal boot caller currently supplies parser-produced state, so this is a latent API-boundary defect rather than an observed boot exploit.

Fix: validate `map_count`, every map range, map type, and checked `base + length` before indexing. Prefer calling a shared boot-context validator rather than relying on caller history.

## (B) Robustness, clarity, and consistency improvements

### B1. Redundant FDT output zeroing

`kernel/core/fdt.c:599-603`

`je_fdt_parse()` zeroes `out` before argument validation and then zeroes it again after validation.

Why it matters: harmless, but it obscures the intended failure contract and invites future ordering changes.

Fix: retain one clearly documented zero-before-validation operation.

### B2. Boot reservation failures are ignored

`kernel/core/boot.c:31-33`

The return values of the three initial `reserve()` calls are discarded.

Why it matters: the current capacity makes failure impossible, but the code silently loses a failure if the capacity or reservation policy changes.

Fix: check and propagate each result. Add a test configuration or seam that forces reservation exhaustion.

### B3. Module ranges are not checked against the supported physical profile

`kernel/core/boot.c:65-71`

Module start/end are taken from 32-bit fields and passed to `reserve()` without requiring `end <= JE_PHYSICAL_LIMIT` or containment in RAM.

Why it matters: the resulting context can contain reservations outside the allocator’s supported physical profile. That is inconsistent with the parser’s “loader data reserved” claim.

Fix: either reject out-of-profile module ranges or explicitly classify them as ignored/out-of-window, with a documented contract and tests.

### B4. Linker-symbol pointer arithmetic in `image.c` is fragile C

`kernel/core/image.c:113-120`

`end <= start` and `end - start` operate on pointers to linker-defined symbols. Those symbols are not necessarily members of one C array object, so strict C pointer relational/subtraction rules are questionable.

Fix: convert both to `uintptr_t`, check `end > start`, then subtract integer addresses. Also generate image lengths as `size_t` or a checked fixed-width integer rather than casting `unsigned long` at `:135`, `:141`, etc.

### B5. `image.c` is not the actual untrusted image parser

`kernel/core/image.c:110-201`

This file registers built-in images. The real descriptor validator is `kernel/core/vm.c:56-125`; the actual build ordering is in `vm.c:231-267`.

Why it matters: audit and fuzz planning can incorrectly claim `image.c` is covered when only `je_vm_image_validate()` is tested.

Fix: rename/document the registry role and add direct `je_builtin_images_init()` tests for partial initialization, generated-length mismatch, and inaccessible built-in entries.

### B6. Handle type-zero policy is inconsistent

`include/jeios/handle.h` says type 0 is reserved/invalid, but `kernel/core/handle.c:92-108` accepts type 0. `tests/test_handle.c:337-343` intentionally permits arbitrary types and NULL objects.

Why it matters: the pure table’s type-agnostic contract and the header’s reserved-type statement disagree.

Fix: choose one contract:

- reject type 0 in `je_handle_alloc()`, or
- state explicitly that type validation belongs to syscall dispatch and type 0 is permitted internally.

Add a test for the chosen rule. This touches the handle contract and is owner-gated.

### B7. The no-NX profile reports “protected mappings”

`kernel/arch/x86_64/paging.c:23-39`  
`kernel/core/main.c:1926-1929`

When NX is unavailable, paging initialization succeeds and maps ordinary pages without NX. Main prints “protected mappings … ready,” although it separately prints that NX is absent.

Why it matters: this can be read as a W^X claim when the actual profile is degraded. User VM creation correctly refuses without NX, but kernel data remains executable.

Fix: return an explicit degraded-profile result or expose a profile object consumed by diagnostics. Make the message say exactly which guarantee is absent.

### B8. ARM physical-memory helpers use inconsistent overflow handling

`kernel/arch/aarch64/physmem.c:4-8` and `:10-23`

`range_intersects_page()` explicitly handles `base + size` overflow; `range_contains_page()` does not.

Why it matters: current callers fail closed for the live allocator window, but the helper pair does not share a total, auditable arithmetic contract.

Fix: use one checked range-end helper for both functions and reject overflow before comparison.

### B9. GIC initialization leaves partial side effects on failure

`kernel/arch/aarch64/gic.c:219-273`

Initialization masks the timer and then performs multiple writes before later readback checks can fail. Failure returns without restoring/quiescing all earlier state.

Why it matters: current boot halts on failure, but reuse or recovery would inherit partially configured interrupt state.

Fix: document “failure is terminal,” or add a rollback/quiesce path. Reset `je_arm_ticks`, `je_arm_spurious`, and `je_arm_last_iar` on successful initialization if re-init is supported.

### B10. AArch64 BSS clearing trusts linker alignment and ordering without a runtime guard

`kernel/arch/aarch64/boot.S:167-192`

The clear loop assumes `__bss_start <= __bss_end` and 16-byte alignment.

Why it matters: a linker-script regression can cause an overrun or a very long loop before diagnostics are available.

Fix: add build-time assertions in the linker script and a small assembly precondition check for ordering/alignment. The existing `ARM_BSS_CHECK` is excellent content validation but does not test malformed symbol geometry.

### B11. ARM table cache invalidation uses a broad min/max interval

`kernel/arch/aarch64/paging.c:375-382`

The code computes one interval from the minimum to maximum table frame, including unallocated gaps.

Why it matters: it is conservative, but it may perform cache maintenance over unrelated memory and makes the invalidation contract less precise.

Fix: maintain exact contiguous runs or document and bound the intentional over-invalidation. Add a test with deliberately noncontiguous table allocations.

### B12. Usercopy scope reentrancy is documented but not enforced

`kernel/core/usercopy.c:55`, `:108-118`

`g_scope` is a single mutable global. The comments require interrupts disabled and no nesting, but `copy_core()` does not assert that the scope is inactive or enforce ownership.

Why it matters: a nested copy or future interrupt path can overwrite the active scope and cause an unrelated page fault to be treated as recoverable.

Fix: reject an already-active scope, or make it per-CPU with explicit critical-section ownership. Owner-gated proposal.

### B13. x86 page restore has no guard ownership token

`kernel/arch/x86_64/paging.c:71-78`

`je_arch_page_restore()` restores any zero leaf in the allocator window.

Why it matters: a caller bug can restore a page that was never guarded, including a page intentionally unmapped for another reason.

Fix: track guard state or use a guard token containing the address and expected prior leaf. At minimum, make the API private to the scheduler and assert a matching guard record.

### B14. ARM canary loops can hang forever if the timer stops

`kernel/arch/aarch64/vectors.S:357-419`, `:550-572`

The canary loops wait for tick counts, but have no counter-based deadline or watchdog.

Why it matters: a timer/vector regression becomes an infinite loop; only the external smoke timeout detects it.

Fix: add a `CNTPCT` deadline or bounded iteration count with explicit failure evidence.

## (C) Tests that appear to prove more than they do

### C1. FDT deterministic mutation is not a semantic fuzz proof

`tests/test_fdt.c:5151-5215`, `:5293-5302`

The suite is excellent: exact-size buffers, truncation sweep, independent builder, pre-poisoned outputs, deterministic double-parse. But fixed-seed byte mutation is not coverage-guided fuzzing and may never hit a specific semantic mutant.

Mutants that could survive:

- accept a partially overlapping reservation as ignored;
- skip `strings_end > blob_len`;
- allow an unterminated strings entry;
- remove a duplicate-property check.

Add libFuzzer/AFL harnessing plus named targeted mutants.

### C2. Boot mutation coverage misses important physical-contract cases

`tests/test_boot.c:63-74`

The 20,000 mutations use one 128-byte fixture and only assert parser boundedness and a few reservation properties.

Missing killers:

- boot block located in non-RAM;
- module above `JE_PHYSICAL_LIMIT`;
- ignored `reserve()` failure;
- module range that is valid arithmetic but outside the supported profile.

A mutant removing the missing boot-block RAM check would survive the current suite.

### C3. Usercopy has no direct hosted kernel-buffer NULL test

`kernel/core/main.c:2256-2300`

The integration battery tests “null source” and “kernel source,” but those are user virtual addresses, not a NULL kernel destination/source passed to `copy_core()`.

A mutant adding/removing a kernel-buffer NULL check is invisible to current tests. Add a hosted fake scheduler/VM seam or a small direct unit harness for `copy_core()` and `je_usercopy_try_fixup()`.

### C4. `test_vm` does not test real x86 TLB behavior

`tests/vm_fake.c:3-6`, `:48-49`  
`tests/test_vm.c:493-543`

The fake explicitly says it proves nothing about CR3/TLB/invlpg. Its `flush` is only a counter.

A mutant replacing real x86 invalidation with a no-op passes the entire hosted VM suite. Add a QEMU test that clears a PTE, reuses the physical frame, and verifies the old user translation cannot read/write it.

### C5. Hosted ARM paging cannot prove `paging_enable.S`

`tests/test_paging_bits.c:92-107`

The MMU-enable leaf is replaced with a recorder stub. Therefore these mutants survive:

- remove `tlbi VMALLE1`;
- remove `dsb`/`isb`;
- omit `dc ivac`;
- omit `SCTLR.WXN`.

The test proves argument construction, not hardware transition ordering. Keep the independent hosted oracle, but add QEMU fault/cache-sensitive tests and assembly disassembly assertions.

### C6. Hosted GIC tests do not prove vector entry or real MMIO behavior

`tests/test_gic_bits.c:34-36`

The fake GIC model is excellent and catches IAR count, EOIR, spurious handling, readback, and timer semantics. It explicitly does not prove actual interrupt delivery, vector entry, or Device memory mapping.

A wrong `vectors.S` veneer or incorrect ARM MMU Device mapping can pass all hosted GIC tests. Keep the fake and add QEMU tests for vector-5 entry, EOI ordering, and real timer delivery.

### C7. Image tests validate descriptors, not executable behavior

`tests/test_image.c:112-143`

The independent content check only requires one nonzero byte in the RX segment. A blob containing `ud2`, an invalid entry sequence, or wrong service protocol can still pass descriptor validation.

Fix: add an image smoke test that boots/runs each generated image and checks its expected marker/protocol. `tools/check_user_image.py` is useful for forbidden instructions but is not an execution oracle.

### C8. Allocator tests prove zero-on-allocation, not zero-on-free

`tests/test_alloc.c:41-52`

The test fills and frees a heap allocation, then verifies the next allocation is zero. That kills removal of `je_heap_alloc()` zeroing, but not removal of VM/backend free scrubbing, because the next allocation itself may clear the page.

Add a test that inspects the physical backing immediately after owner release, or reuses it as a page table before any consumer-side zeroing.

### C9. Pure handle tests do not prove syscall rights masking

`tests/test_handle.c:337-343` intentionally accepts arbitrary rights; `tests/test_syscall.c:333-404` tests pure decoding.

A mutant removing `rights & dispatch->known_rights` at `kernel/core/syscall.c:869` can survive unless a hidden/internal rights bit is injected into a live handle and observed through the syscall glue.

Add a synthetic dispatch test with an internal bit and assert that only ABI-known rights reach user space.

### C10. Trap routing and GIC policy are tested separately

`kernel/arch/x86_64/trap.c:138-150`

The GIC fake proves `gic.c`; it does not prove that trap/IRQ routing calls the correct device handler and scheduler path. A vector-35/vector-39 swap or dropped device dispatch can survive hosted GIC tests.

Add a direct trap-dispatch seam test for timer, IRQ3, IRQ7, and yield vectors.

### C11. ARM canary phases do not cover the same register set

`kernel/arch/aarch64/vectors.S:410-438`, `:522-531`

The primary phase checks x0–x28. The secondary phase checks x29/x30 but uses x0–x2 as scratch and does not recheck x0–x28 during its later ticks.

A mutant corrupting x0 restoration only during the secondary phase can pass. The comments honestly acknowledge that x29/x30 are not directly asserted in the primary phase; this is a test gap, not a hidden claim.

Fix: run a full-register canary across the entire test interval, or add a third phase that checks the previously covered low registers after the high-register phase.

### C12. Current mutant infrastructure is incomplete and can hang

Untracked `tools/mutants.py:60-65`, `:82-110`; `tools/mutants.json` currently has only two allocator mutants.

The runner has good properties: exact-one-match drift detection, restoration in `finally`, distinct `SURVIVED`/`BLOCKED`, and nonzero failure status. But it has no subprocess timeout, no isolated worktree/build directory, and no manifest schema validation.

Why it matters: the proposed ARM cycle mutant or vector hang can stall the entire assurance job; deleting a pre-existing build target can also disturb the workspace.

Fix: add per-mutant timeouts, isolated `BUILD` directories, restore verification, manifest validation, and a preflight that rejects a dirty source file.

## Genuinely excellent patterns

- `include/jeios/fdt.h:13-20`: copy-once, parse-only-the-copy, then architecture-facts validation. This is an excellent TOCTOU contract.
- `kernel/core/fdt.c:614-685`: checked 64-bit layout arithmetic and bounded reservation scanning.
- `tests/test_fdt.c:14-22`, `:5045-5053`: exact-size buffers and exhaustive truncation are exactly the right parser-test pattern.
- `kernel/core/boot.c:83-99`: subtractive bounds checks in `je_boot_ram_page()` and default-deny free-page logic.
- `kernel/core/alloc.c:77-93`: validates address/window/alignment before deriving the state-array index.
- `kernel/core/usercopy.c:139-160`: exact RIP matching, direction/PFEC matching, CR2 range checking, and owner matching.
- `kernel/core/vm.c:231-267`: zero page, copy bytes, remove writable alias, demote permissions, then publish the user PTE.
- `kernel/core/handle.c:111-132`, `:186-207`: centralized precedence and saturating generation retirement prevent stale-handle reuse.
- `kernel/arch/aarch64/paging.c:388-409`, `:482-546`: independent exact descriptor literals, table-frame membership, alias scanning, and W^X rejection.
- `tests/test_paging_bits.c:6-13`, `:342-379`: independent architecture oracle and named tamper mutants.
- `kernel/arch/aarch64/gic.c:300-339` and `tests/test_gic_bits.c:18-36`: IAR-once, spurious-no-EOIR, modeled timer level behavior, and explicit non-claims.
- `kernel/arch/aarch64/vectors.S:145-165`: stack-free, bounded double-fault path.
- `kernel/arch/aarch64/boot.S:116-164`: DTB overflow/overlap guard before BSS clearing.
- The comments in `vm_fake.c`, `test_gic_bits.c`, and `test_paging_bits.c` explicitly state what the test does not prove. That honesty should be copied throughout the repository.

## CURL-style assurance deliverable

### Fuzz/torture harnesses

1. FDT — highest priority.

   Hosted libFuzzer target:

   ```c
   int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
   {
       if (size > JE_FDT_MAX_TOTAL) return 0;
       je_boot_ctx ctx;
       uint32_t off;
       je_fdt_status s = je_fdt_parse(data, size, &ctx, &off);
       /* On success, check the same postconditions as test_fdt.c. */
       return 0;
   }
   ```

   Seed with the QEMU DTB, generated positive cases, every named negative case, and truncation prefixes. Run parse twice and compare status/error/output for determinism.

2. Boot parser.

   Fuzz `(blob, accessible, physical, kernel_begin, kernel_end)` with exact-size input buffers. Target tag sizes, alignment padding, map overlap, module NUL termination, physical-range containment, and reservation exhaustion. Add a structure-aware mutator for valid Multiboot tags.

3. Image validation.

   The actual C target is `kernel/core/vm.c:je_vm_image_validate()`, not `image.c`. Fuzz a wire-format descriptor plus blob, checking no sanitizer failure and no accepted W+X/overflow/out-of-arena image.

   Separately fuzz `tools/mkimage.py` with Atheris or a deterministic malformed-ELF corpus. The kernel intentionally does not parse ELF.

4. Usercopy bounds/fixup.

   Build a hosted fake `je_vm_ops` and fuzz:

   - zero/nonzero lengths;
   - NULL kernel buffers;
   - arena start/end and wraparound;
   - spans crossing page boundaries;
   - RX/RW/NX/non-present PTEs;
   - all PFEC combinations;
   - exact/nearby RIPs;
   - CR2 at `user_lo`, `user_hi - 1`, and `user_hi`.

   Keep a QEMU deterministic test for the real assembly fault/fixup path.

### Named critical mutants

| ID | Broken invariant | Killer |
|---|---|---|
| `FDT-FAIL-ZERO` | Failed parse leaves no partial context | `test_fdt` pre-poisoned output checks |
| `FDT-TRUNCATED-BLOCK` | Nonzero block bytes must be inside `len` | FDT truncation sweep |
| `FDT-PARTIAL-RESERVATION` | Partially overlapping reservation rejects | targeted `BAD_RANGE` case |
| `BOOT-BLOCK-NONRAM` | Loader metadata must be in RAM | boot map with `physical` in reserved/MMIO range |
| `BOOT-RESERVE-RESULT` | Every reservation failure propagates | forced reservation-capacity failure |
| `USERCOPY-NULL-KERNEL` | Nonzero NULL kernel buffer returns INVALID | direct copy harness |
| `USERCOPY-CR2-END` | `cr2 == user_hi` is not recoverable | direct fixup predicate test |
| `VM-WX` | No user W+X PTE | hosted VM permission test plus real QEMU check |
| `VM-CLEAR-PTE` | Teardown clears user PTE before reuse | stale-PTE teardown/reuse test |
| `VM-TLB-FLUSH` | Cleared translations cannot survive reuse | real x86 CR3/TLB test |
| `VM-ZERO-FREE` | Released VM/page-table frames contain no prior data | cross-process physical reuse test |
| `HANDLE-GEN-CHECK` | Old generation is always stale | close/reuse test in `test_handle` |
| `HANDLE-RIGHTS-SUBSET` | Duplication cannot add rights | handle attenuation test |
| `HANDLE-RIGHTS-MASK` | Hidden internal rights never leak | synthetic syscall-glue hidden-bit test |
| `ARM-WXN` | Writable mappings cannot execute | ARM MMU/QEMU execution fault test |
| `ARM-AUDIT-CYCLE` | Corrupt table graph rejects and terminates | self-referential table test with timeout |
| `GIC-NO-EOI` | Every real non-spurious interrupt is EOI'd | `test_gic_bits` EOIR count |
| `GIC-SPURIOUS-EOI` | Spurious IAR does not receive EOIR | fake IAR=1023 test |
| `TRAP-IRQ-ROUTE` | IRQ vectors dispatch to the correct handler | direct `trap.c` routing test |
| `VECTOR-LATE-REG` | Register preservation holds for every live tick | full-duration ARM canary |

### Static-analysis and sanitizer matrix

Current baseline already has strict warnings including `-Wconversion`, ASan/UBSan on most hosted tests, and UBSan-only `test_core`.

Add:

1. Clang static analysis:

   ```sh
   scan-build --status-bugs make -B check
   ```

   Run separately for x86 and AArch64 C sources; record exact Clang version.

2. GCC analyzer:

   ```sh
   CFLAGS+=" -fanalyzer -Wanalyzer-too-complex "
   make -B test
   ```

   Review rather than blanket-suppress findings.

3. Cppcheck on critical C:

   ```sh
   cppcheck --std=c11 --enable=warning,performance,portability,inconclusive \
     kernel/core/fdt.c kernel/core/image.c kernel/core/boot.c \
     kernel/core/usercopy.c kernel/core/alloc.c kernel/core/handle.c \
     kernel/arch/x86_64 kernel/arch/aarch64
   ```

4. Hosted-only MSan for FDT, boot, image, handle, and usercopy harnesses. Do not apply it to freestanding kernel/assembly builds. MSan is valuable for uninitialized parser state, but requires all linked code to be instrumented.

5. Add explicit hosted warning sweeps: `-Wcast-align=strict`, `-Warray-bounds`, `-Wvla`, `-Walloca`, `-Wundef`, `-Wformat=2`, and compiler-specific duplicated-condition/branch warnings.

6. Add Clang source/branch coverage for FDT, boot, usercopy, VM validation, handle, and GIC. Treat coverage as a test-selection signal, not proof.

7. Add assembly checks using `objdump`/`llvm-objdump` for required AArch64 barriers, `tlbi`, `WXN`, vector frame offsets, and stack-free fatal paths.

ThreadSanitizer is not useful yet: the project explicitly has no lock primitive and is currently single-core-correct. Add it when a hosted concurrency model and SMP paths exist.

## Top 8 code improvements, ranked

1. Implement and test the real x86 VM invalidation contract around `vb_flush()`.
2. Reject nonzero NULL kernel buffers in usercopy and add direct boundary tests.
3. Make AArch64 MMU audits cycle-safe and explicitly bounded.
4. Validate the Multiboot source block and module ranges against the physical profile.
5. Centralize or mechanically enforce zeroization before page reuse/share.
6. Harden x86 paging initialization against malformed context counts and expose an explicit NX-degraded profile.
7. Complete ARM canary coverage across all phases and add a timeout.
8. Resolve the handle type-zero contract and add syscall-glue hidden-rights tests.

## Top 5 assurance/CI additions, ranked

1. Commit the mutant runner/manifest with timeouts, isolated builds, and the named critical mutants above.
2. Add libFuzzer/AFL-style FDT and boot harnesses, plus structured image and usercopy fuzzers.
3. Add real QEMU stale-PTE/TLB, ARM barrier/WXN, vector-routing, and late-canary tests.
4. Add scan-build, GCC analyzer, cppcheck, MSan-hosted, explicit warning sweeps, and coverage targets.
5. Preserve deterministic corpora and exact evidence in `docs/HANDOFF.md`, including blocked LeakSanitizer runs and unobserved hardware/physical claims.
