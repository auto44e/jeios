# M4 PR-E cross-lineage adversarial reviews

ADR 0000 part 4 attachment for PR-E (ring-3 C images + `tools/mkimage.py`).
Two AI model lineages reviewed each other's worker output adversarially. This is
AI-assisted review and is recorded as such; it is **not** the independent human
qualification the critical profile requires.

- **Impl worker:** Codex (`tools/mkimage.py`, `user/crt0.S`, `user/user.ld`,
  `kernel/arch/x86_64/user_demo.S`, `kernel/core/{main,image}.c`, Makefile).
- **Tests worker:** Claude (`tools/test_mkimage.py`, `tests/test_image.c`).
- **Cross-lineage reviewers:** Claude reviewed the Codex impl; Codex reviewed the
  Claude tests. The lead (me) integrated, ran mutants, and decided fixes.

Base for both diffs: `1df911a` (pre-wiring) .. PR-E head.

## Convergent finding (acted on)

Both reviewers independently landed on the same defect from opposite sides:

- **Claude (on impl), HIGH:** `mkimage.py` did not report the *lowest* violated
  rule. E6/E7 shared one per-segment loop and E8/E9 shared another, so a later
  segment's low-numbered violation lost to an earlier segment's high-numbered
  one. Reproduced with hand-made ELFs: an E7 seg-0 + E6 seg-1 input reported E7
  (should be E6); an E9-total + E8-overlap input reported E9 (should be E8).
- **Codex (on tests):** the suite's rule-order coverage was "mostly absent" —
  almost every reject case violated a single rule, so an implementation that ran
  rules in the wrong order would survive. Listed the exact missing pairs.

**Fix (commit e9bb4d7):** each rule runs as its own complete pass (E6→E7→E8→E9→
E10); `test_mkimage.py` gained paired-violation precedence cases. Verified by A/B:
the `E6<E7` case FAILS against the pre-fix tool (reports E7) and passes after;
the `E8<E9` case reports E9 when E8 is disabled by a mutant.

## Other findings acted on

- **Atomic output (Claude medium 3/4):** a reject left a stale `OUT.c`, and a
  mid-write failure could leave a truncated file with a fresh mtime that make
  treats as current. Now tempfile + `os.replace`; `.DELETE_ON_ERROR` added.
- **Usage prefix (Claude low 7):** usage/IO errors (exit 1) dropped the
  `reject:` prefix so a rule-id parser can't misread `usage`.
- **Makefile sha guard (Claude low 6):** the `;`-joined guard couldn't abort;
  now under `set -e`.
- **User dep tracking (Claude medium 5):** `-MMD` on `user/%.S` + `-include
  $(USER_OBJECTS:.o=.d)` so an ABI-header edit rebuilds the embedded image.
- **Orphan sections (Claude low 8):** user link now `--orphan-handling=error`.
- **Later-segment validator mutations (Codex gap 3):** per-segment field
  mutations in `test_image.c` now run on every segment, not just seg 0.
- **Non-degenerate blob (Codex gap 1):** `test_image.c` now asserts the entry RX
  segment's bytes are not all-zero (the kernel validator never reads blob bytes,
  so a zeroed-but-structurally-valid artifact would otherwise pass).

## Findings considered and NOT acted on (with reason)

- **Claude low 9 (no `PT_GNU_STACK` emitted):** harmless — the kernel builds the
  user stack RW+NX itself; E4 is still exercised by `test_mkimage.py` on crafted
  ELFs. No change.
- **Claude low 10 / Codex (smoke doesn't assert which image ran):** the boot line
  and exit-0 check don't prove the child was `JE_IMAGE_C_HELLO` vs spawn-child.
  Accepted as-is for PR-E; the image id is hard-coded in the launch asm and the
  `test_image` cross-check ties the generated descriptor to the real validator.
  Revisit if PR-F's supervisor selects images dynamically.
- **Codex: run `test_image` under `-DNDEBUG`:** the CHECK-not-assert property is
  documented and the macro is unconditional; not worth a second build variant now.
- **Exact blob sha/len/segment_count pin in `test_image.c`:** deliberately not
  added — it would restate the build output, not provide an independent oracle,
  and would be brittle across legitimate `hello.c` changes. The kernel already
  pins the sha via `JE_C_HELLO_BLOB_SHA256` at compile time and smoke prints it.

## Mutants run by the lead (through the real build)

1. Swap W↔X in the ELF→JE perm mapping → generated c_hello code segment becomes
   RW → entry no longer in an RX segment → `make` fails at generation with E10.
   Caught.
2. Disable the E8 overlap check → `test_mkimage.py` fails on many E8 cases and
   the new `E8<E9` precedence case reports E9. Caught.
3. (Earlier, PR-D) supervisor-guard mutant — caught; recorded in that PR.

## Evidence after fixes

`ASAN_OPTIONS=detect_leaks=0 make check`: PASS — `test_image` 88 checks
(image=c_hello blob=613 segments=3), `test_mkimage` 562 checks (E1–E10 + CLI +
output). `make smoke`: PASS (M4e-s1, image sha256=351bc37034ee86c3). Full sweep
(integration 11 fault modes, NX ×11 across 3 profiles, ARM64 base + 3 variants)
green locally. Local evidence only; remote CI reported separately on the PR.
