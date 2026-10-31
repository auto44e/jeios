# WORKER_NOTES — M4 PR-C test writer (issue #53, ADR 0010 D5/D6)

Observed 2026-10-31T10:15Z, worktree `wt-m4c-tests`, branch `wip/m4c-tests`, base
`1f00b93` (pre-wiring commit). Not committed. Authorship: a single AI agent (Claude),
working as the TEST WRITER. This is AI-assisted work, not independent human review.

## What changed (tests/*.c only)

- `tests/test_handle.c`: new `test_can_alloc`, which runs in both the production
  (16/16) and reduced-width (gen_bits=2) builds. It uses the `ca_check` oracle on every
  state. The oracle checks four things:
  - `je_handle_can_alloc(t) == want`;
  - the slot bytes are unchanged (memcmp) and so are `slots`, `capacity`, `quota` and
    `live`;
  - a second call returns the same status;
  - a real `je_handle_alloc` on a byte-identical scratch copy returns the SAME status.

  States covered:
  - NULL → BADINDEX;
  - empty → OK;
  - quota 0 → QUOTA;
  - quota 3: OK, then QUOTA at the quota, OK after a close, QUOTA after a refill;
  - quota == cap with all LIVE → QUOTA (QUOTA before NOSLOTS), then OK after a close;
  - all slots RETIRED (live 0) → NOSLOTS. This takes CAP×GEN_MAX ≈ 524k cycles in
    production, which is fast;
  - RETIRED+LIVE with live < quota → NOSLOTS, then OK after a close;
  - RETIRED+LIVE with live == quota → QUOTA;
  - preflight followed by a real alloc on the same table agrees;
  - a capacity-1 table: OK, then QUOTA, then NOSLOTS once retired.
- `tests/test_syscall.c`:
  - **`test_spawn_abi_constants`**:
    - checks OBJ_LAUNCHER=4, RIGHT_SPAWN=0x20, that SPAWN is in RIGHTS_KNOWN, and that
      it is disjoint from the other rights;
    - checks GRANTS_MAX=4, STARTUP_V1=1, QUOTA_DEFAULT=15 and CHILD_QUOTA_MAX=0;
    - checks the grant layout: sizeof 8, `offsetof` handle=0 and rights=4, a
      little-endian wire decode, and a 4-grant array of 32 bytes;
    - checks that the quota macros round-trip across the field extremes, and that the
      unpack macros ignore bits [63:32].
  - **`test_clock_ticks_decode`**:
    - all-zero arguments → `JE_SYSCALL_CLOCK_TICKS` with OK;
    - each of the 6 argument registers set alone to 1, to the top bit and to all-ones,
      plus all of them dirty at once → RETURN/INVALID with res 0;
    - op `0x1_0000_000d` → UNSUPPORTED.
  - **`test_proc_spawn_decode`**: boundary OKs:
    - count 0 with RDX 0;
    - counts 1..4 with RDX≠0;
    - count 4 with handle quota MAX;
    - handle quota 1, 15 (DEFAULT), 39 (MAX) and 0 (the default);
    - endpoint quota = CHILD_QUOTA_MAX (0);
    - RDI = 0xffffffff, RSI = 0xffffffff and RDI = 0;
    - RDX = all-ones with count 1, because the pointer belongs to the glue's copy.
  - **`test_proc_spawn_decode`**: INVALID cases, each with res==0:
    - wide RDI (3 values) and wide RSI (2 values);
    - count 5, 0xff, 2^32, 2^32+1 (which must not narrow to 1) and all-ones;
    - RDX≠0 with count 0;
    - RDX==0 with each count from 1 to 4;
    - quotas with bit 32, bit 63, or a high half of all-ones over a valid low half;
    - handle quota 40 (MAX+1, which equals HANDLE_CAP) and 0xffff;
    - endpoint quota 1, 1 with a valid handle quota, and 0xffff;
    - R9 = 1, R9 = the top bit, and R9 on an otherwise valid call;
    - every rule broken at once;
    - op `0x1_0000_000e` → UNSUPPORTED.
  - **Rights decode**:
    - The old `dup(self, 0x20) == INVALID` lines are retired, with a HISTORY comment.
    - New expectations: `dup(self, SPAWN)` and `INSPECT|SPAWN` → DUP/OK. Bit 0x40,
      KILL|0x40, SPAWN|0x40 and KNOWN|0x80000000 → INVALID.
    - The every-bit-unknown loop now starts at 0x40.
    - `JE_ABI_RIGHTS_KNOWN == 0x3f` is pinned.
  - **Still-UNSUPPORTED ops**: the list is now {16, 17, 18, 19, 20}, with 13 and 14
    removed. A HISTORY comment records the change.
- `tests/test_uframe.c`: covers the startup-v1 frame.
  - RDI=1, RSI=4, RDX/RCX/R8 are handles and R9=0xffffffff, under 5 RFLAGS inputs: 0x202,
    0, IOPL=3, IOPL=3|IF, and all-ones.
  - Each passes `je_uframe_check`. The frame is byte-identical afterwards (memcmp) and
    each argument register keeps its value. IF is forced to 1 and IOPL to 0.
  - With all-ones in every GPR the frame still passes, and the sanitized RFLAGS is
    exactly IF|bit1.
  - Nonzero GPRs do not rescue a frame with a bad RIP.

## Commands run and results

1. Baseline before my edits: `ASAN_OPTIONS=detect_leaks=0 make test` gave rc=2.
   - `test_core`, `test_boot`, `test_alloc`, `test_vm` and `test_uframe` passed.
   - `test_syscall` failed at line 133, on the old `dup(self,0x20)==INVALID` assertion.
     The pre-wiring commit's abi.h made 0x20 KNOWN, so HEAD's own suite was already red
     on that one line. My change replaces it.
2. After my edits, the same command gave rc=2 (log: `/tmp/m4c-tests-run1.log`).
   - `test_core`, `test_boot`, `test_alloc`, `test_vm` and `test_uframe` passed.
   - `test_syscall` failed at `test_clock_ticks_decode`, on the first op-13 assertion
     (line 94).
   - `make` stops at the first failing binary, so I built and ran the rest one by one:
     - `test_handle` FAIL at line 491 (`can_alloc(NULL) == BADINDEX`), the first
       can_alloc assertion;
     - `test_handle_rw` FAIL at the same line;
     - `test_endpoint` PASS;
     - `test_proc` PASS;
     - `test_proc_retire` PASS;
     - `test_fdt` PASS.
3. To check that my assertions agree with each other, without reading the
   stubs or the real implementations, I wrote throwaway contract-literal
   references in `/tmp/m4c-tw-ref/` (outside the tree, not for commit):
   - `ref_handle.c`: NULL→BADINDEX, then `live >= quota`→QUOTA, then any FREE→OK, else
     NOSLOTS. `handle.c` was compiled with `-Dje_handle_can_alloc=stub_can_alloc`.
   - `ref_syscall.c`: wraps the real decoder, which is renamed with
     `-Dje_syscall_decode=real_decode`. It decodes ops 13 and 14 exactly as the abi.h
     text says.
   - Results: `test_handle` (production) PASS, `test_handle` (reduced width) PASS,
     `test_syscall` PASS. All other ops go through the real decoder unchanged, which
     confirms that the rights edits match HEAD's decoder.
4. Mutation check against those references (each must be killed):
   - **can_alloc mutants:** NOSLOTS before QUOTA, off-by-one quota, RETIRED treated as
     FREE, and a mutating can_alloc.
   - **decode mutants:** grant count narrowed to 32 bits, no R9 check, `count >= 4`,
     `quota >= MAX`, no check of quotas[63:32], no RDX/count pairing check, a nonzero
     res on INVALID, and CLOCK_TICKS ignoring R9.
   - Result: **12/12 killed**.
5. `git diff --check` was clean, with warnings as errors (`-Werror`) under the
   Makefile's WARN flags, which the make run uses.

## Failures against the stubs (expected), mapped to the stub hit

| Failing assertion | Stub hit |
|---|---|
| `test_syscall.c:94` op 13 → CLOCK_TICKS | `je_syscall_decode` in `kernel/core/syscall.c`: op 13 is not decoded yet (stub or old UNSUPPORTED path). `test_proc_spawn_decode` cannot run past this abort, so the op 14 decode is unverified against the tree but passes against the reference. |
| `test_handle.c:491` `can_alloc(NULL)` (both widths) | the `je_handle_can_alloc` fail-closed stub in `kernel/core/handle.c` |

I wrote no test for `je_thread_create_user_start`, `je_proc_spawn`,
`je_spawn_failpoint_set` or `je_builtin_image*`. `spawn.c`, `image.c` and `sched.c` are
kernel-only and have no hosted harness in the Makefile. They pull in the scheduler,
`vm_build`, the backend and linked image symbols. Their coverage (the startup registers
reaching ring 3, fail-Nth at each journal step and grant index, and pool baselines)
has to come from the kernel fixture/smoke path, which is not my file set.

## Decisions (with rejected alternatives)

- **can_alloc oracle = alloc on a scratch copy.** The alternative was to hand-code an
  expected status per case. That would test only my reading of the status rules. The
  header contract is "exactly what alloc would return", and the scratch copy tests
  that literally. I kept the explicit `want` as well, so that a buggy alloc in
  `handle.c` cannot silently pass both sides.
- **All-RETIRED is tested at the production width too.** The alternative was the
  reduced width only. 524k alloc/close cycles take well under a second under ASAN,
  and production is the configuration that ships.
- **Unknown-bit probe moved to 0x40.** I did not pick a high bit only. 0x40 is the
  lowest still-unknown bit, so it catches a `RIGHTS_KNOWN` that grew by one bit too
  many. I kept 0x80000000 (as `KNOWN|0x80000000`) and the full loop. I also pinned
  `RIGHTS_KNOWN == 0x3f`, so an accidental DEVICE-bit addition fails loudly in this PR.
- **On a spawn/ticks OK decode, `res` is not asserted.** The glue writes RDX (the
  ticks or the new handle), and the headers say nothing about decode's res on OK.
  On INVALID I assert res==0, following the existing suite's no-RDX-leak convention.
- **Decode OKs for RDI=0, an all-ones RDX pointer, and RSI=0xffffffff.** I read the
  abi.h list "Decode (pure) rejects with INVALID: …" as exhaustive. Handle validity,
  image_id existence and the user-pointer range therefore belong to the glue or
  spawn, as precedence steps 1–2 and the copy-in in spawn.h say. See the ambiguities
  below.
- **The `test_handle` resolve_any test already does GEN_MAX cycles in production**,
  so the extra cost of `ca_retire` is consistent with the existing suite.

## Header ambiguities (flagged, not resolved; I implemented the literal reading)

1. **Is the abi.h op-14 INVALID list exhaustive?** My tests assume it is. If the
   implementer adds a decode-level check (a canonical or user-range check on RDX,
   RDI==0, or grant-pointer alignment), my `spawn_ok` cases with
   RDX=0xffffffffffffffff and RDI=0 will fail. Under spawn.h's precedence, a bad
   grants pointer gives the copy-fault status from the glue, not INVALID from decode.
   ADR D5 says "A bad pointer gives the existing copy-fault status". That supports
   the literal reading.
2. **Duplicated quota check.** spawn.h precedence step 3 re-validates quotas inside
   `je_proc_spawn`, while decode already rejects them. That is harmless, but there are
   now two sources of truth. Only the decode copy is hostable and tested.
3. **The order of the INVALID rules within decode is unobservable.** All of them give
   INVALID, so no precedence among them is tested or needed.
4. **Decode's res on an OK decode** is unspecified for ops 13 and 14 (see above).
5. **The DEVICE rights 0x40/0x80/0x100 (ADR D3)** will make my 0x40 probe and the
   `RIGHTS_KNOWN == 0x3f` pin wrong in the PR that adds DEVICE. This is intentional,
   but that PR must update these lines, as the M3c and PR-B precedents did.
6. **`je_handle_can_alloc` on a table with `slots == NULL`, or with `live > quota`
   (corrupt accounting),** is unspecified. Neither is tested.

## Not verified

- Nothing was run against the REAL `can_alloc` or the real op-13/14 decoder. They are
  being written in parallel and were deliberately not read. The lead must run
  `ASAN_OPTIONS=detect_leaks=0 make test` after integration.
- `make check`, `make smoke`, `make integration`, CI profiles and ARM: not run, and not
  in scope for a tests-only change against stubs.
