# WORKER_NOTES — M4 PR-A (#53), TEST WRITER round 2

Observed 2026-10-31 (local, hosted only). Worktree `wt-m4a-tests`, branch `wip/m4a-tests`, base
commit `f2e85e7` (round-1 tests + real `je_vm_build_quota` merged). Nothing is committed or pushed.
This is AI-assisted work. It is not independent human review.

## State
- **implemented:** fixes for all four round-2 review items. Only `tests/test_vm.c` and `tests/test_endpoint.c` changed.
- **tested (local, hosted):** `ASAN_OPTIONS=detect_leaks=0 make test` gives rc=0, and every hosted binary prints PASS.
- **not verified:** `make check`, `smoke`, `integration`, CI profiles, ARM. Those are the lead's job. I did not mutation-test
  the new endpoint test 4b or the second-NOSLOTS snapshot.
- I did not read `kernel/core/vm.c`. One fact about it came out by accident: the ASan stack trace of a scratch mutant
  showed that `je_vm_build` calls `je_vm_build_quota` (vm.c:389). That fact shaped the mutant harness below.

## Changes
1. **P2 quota-first vs NULL validation** (`test_vm.c`, `test_build_quota_range`): there is a new helper
   `assert_quota_rejected_args(target, image, data, len, nx, quota, with_out)`. For each bad quota
   {0, MAX+1, H, UINT32_MAX} and each nx in {0, 1}, it calls with: NULL vm, NULL image, NULL out_index (good image),
   NULL blob with len 0, NULL blob with len `sizeof blob`, and NULL image + NULL blob + NULL out_index. Each call must
   return INVALID with zero fake backend attempts, byte-identical `pages` and global `vm.procs`, and `*out_index`
   left at its sentinel.
2. **P2 endpoint second (E+1) attempt** (`test_endpoint.c` test 9): after the record is recreated, the test checks that
   both new handles resolve to the SAME record on opposite sides, that they are nonzero and distinct, and that neither
   collides with any other a0/a1 handle. It also checks `live == 2E`. The second NOSLOTS attempt then snapshots pool,
   table header and slots, and memcmps all three afterwards.
3. **P3 de-duplication** (`test_vm.c`): `#define FIXTURE_FREE_PAGES 253u` with a comment giving its derivation
   (256 pages − 3 reserved). `init_pages` asserts the allocator agrees, and every former `253u` literal uses it.
   `build_need()` now takes the image page count from `image_fixture()` (each segment's memory_size rounded up) and
   uses `JE_VM_STACK_PAGES` from vm.h. The literals 3/4 are gone, and a sanity assert checks `image_pages == 3`.
4. **REFOVERFLOW with table not the limiter** (`test_endpoint.c` new test 4b): a dedicated table has
   `REF_T_CAP = JE_EP_REF_MAX + 4` and quota == capacity, with a static assert that capacity >= REF_MAX + 3. One end
   is dup'd to JE_EP_REF_MAX refs. The test asserts `live < quota && live < capacity`. The next dup must return
   REFOVERFLOW, `*out` must be 0, and pool, table header and slots must be byte-identical. Afterwards the peer end
   still dups OK (the bound is per end), and a SELF alloc still succeeds, which shows the table really had room.
   The original test 4 is kept.

## Decisions
- **No discriminating case exists between "quota first" and "NULL check first, then quota".** Both return INVALID with no
  side effect, and the contract does not say whether `*out_index` is written. So no input can tell them apart from
  outside. I recorded this rather than invent a distinction. What the NULL rows DO discriminate is listed next.
  (a) An NX check, BUSY check or validation step that runs before the quota check: the nx=0 rows would return UNSUPPORTED.
  (b) A quota check placed after the build: the NULL-out_index row with a good image would build, write through NULL, or
  make backend attempts.
  Alternative rejected: asserting the NULL-arg statuses with a VALID quota. vm.h does not document them for
  je_vm_build, so that would test unstated behavior.
- **`*out_index` sentinel unchanged on rejection.** The header says "no state change". I read the caller's output as
  state that must not change on a rejected call. If the lead disagrees, this is the one assertion that goes beyond
  the literal text.
- **A named constant instead of a value captured at runtime for 253.** The review allowed either. A constant plus the
  init_pages assert keeps `initial_free` a `const` and fails loudly if the fixture changes.
- **4b uses a separate table and a fresh pool**, not `fresh()`. The point of 4b is to rule out the table limit
  (CAP=40/quota 15 is not obviously non-binding), so the table is sized explicitly.

## Commands and results
| Command | Result |
|---|---|
| `ASAN_OPTIONS=detect_leaks=0 make test` (baseline, before edits) | rc=0, all PASS |
| `make build/0/test_endpoint && ./build/0/test_endpoint` | builds clean (-Werror -Wconversion -Wshadow ...). PASS |
| `ASAN_OPTIONS=detect_leaks=0 make test` (after all edits) | **rc=0, all PASS** (core, boot, alloc, vm, uframe, syscall, handle, handle_rw, endpoint, fdt) |
| `git diff --check` | clean |
| Scratch mutants in `/tmp/m4a_scratch`. Real vm.o is built with the same flags, then `objcopy --redefine-sym je_vm_build_quota=real_build_quota`. A wrapper `je_vm_build_quota` reorders the checks, and the real `tests/test_vm.c` is linked against it | identity wrapper: **PASS** (harness sanity). NX-before-quota: **killed** (test_vm.c:628). Quota-after-build (build at DEFAULT, then destroy and return INVALID): **killed** (test_vm.c:630, fallible count != 0). NULL-check-then-quota: **survives**, as expected (indistinguishable, see Decisions) |

A dead end: the first mutant attempt delegated to `je_vm_build` and used `--allow-multiple-definition`. It recursed,
because `je_vm_build` calls `je_vm_build_quota`. Putting the mutant later in the link order picked the real
definition instead, so the run was vacuous. Both attempts were discarded and replaced by the objcopy rename.
Caveat: both killed mutants were first caught by round-1 rows (`assert_quota_rejected`). I did not isolate whether
the new NULL rows alone would also kill them.

## Not enforced / open
- The NULL-vm row can only check `pages`, the global `vm` and the backend counter. It cannot check the internals of a
  vm that does not exist.
- Endpoint 4b and the second NOSLOTS snapshot were not mutation-tested.
