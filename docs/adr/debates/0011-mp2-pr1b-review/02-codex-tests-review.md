## Assessment: Needs fixes before approval

The fault modes and independent encoder test are substantially non-vacuous. `make check` and `git diff --check` pass locally; ARM smoke/remote CI were not rerun.

### P1 — Alias-proof tests do not exercise the alias scanner

`tests/test_paging_bits.c:363-366` changes a leaf’s physical address to create aliases, but both mutations are rejected earlier by the identity check at `kernel/arch/aarch64/paging.c:505-516`, specifically `va != physical`, before `audit_alias_scan()` at lines 524-526 runs.

Likewise, the install-time ledger is bypassed: `map_page()` rejects an already-valid slot at `paging.c:299-304` before `record_frame_flags()` executes.

Therefore, removing `audit_alias_scan()` or disabling `record_frame_flags()` still leaves these tests passing. The handoff’s claim at `docs/HANDOFF.md:72-73` that removing alias checks fails the test is not supported by this staged code.

Add a direct alias-detector test seam, or explicitly document that identity uniqueness—not the alias scanner—is the tested enforcement mechanism.

### P2 — Smoke leaf-count oracle does not verify table placement

`tools/smoke_arm.py:751-765` checks only that the reported count equals an independently calculated count. It does not verify:

- actual table-frame addresses;
- that every table frame is allocator-free and contiguous;
- that every table frame has an RW/XN identity leaf;
- that the reported root is actually installed in `TTBR0_EL1`.

The hosted test partially covers the pinned first-fit layout at `tests/test_paging_bits.c:318-326`, but the ARM oracle alone could accept a wrong table layout with the same count if the audit were defective.

### P2 — Audit does not reject all non-spec leaf bits

The independent encoder is good: `tests/test_paging_bits.c:61-76` uses separate field macros and does not copy `leaf_descriptor()`. `class_descriptor_matches()` also does not call the builder encoder (`paging.c:388-424`).

However, the audit does not reject leaf `NS`, `DBM`, `Contig`, software/PBHA, or other non-governed bits. The encoder test catches a bad builder encoder, but no tamper test proves the audit rejects such post-build mutations. If “exact descriptor” includes those fields, this is uncovered.

### P2 — Enable assembly is only argument-tested

`tests/test_paging_bits.c:394-403` stubs `je_arm_mmu_enable_leaf()` and verifies its arguments. It cannot fail if the real assembly uses `DC CVAC` instead of `DC IVAC`, omits TLBI/barriers, writes the wrong SCTLR sequence, or touches the stack.

The handoff correctly limits cache/barrier evidence to review-only under QEMU TCG; this remains an assembly-review obligation, not a runtime-tested property.

### Fault-mode verdict

The four fault modes are strong for the pinned QEMU profile:

- wrong fault class: caught by exact EC/FSC/WnR and raw ESR equality;
- wrong target/page: caught by ELF-bound target, FAR, ELR, section range, and Image instruction checks;
- no fault: reaches `FATAL ... did not fault`, so the expected terminal sequence fails;
- wrong kernel comparator: caught because the harness re-decodes raw ESR/FAR/VBAR independently.

The ESR check intentionally pins QEMU’s zero ISV/reserved ISS bits (`smoke_arm.py:848-851`); that is profile-specific and honestly documented.

Also update the contradictory stale handoff finding at `docs/HANDOFF.md:77-80`: the staged `class_descriptor_matches()` is already independent of `leaf_descriptor()`.

**Recommendation:** request changes for the alias-proof gap and clarify/document the table-placement and assembly-test limits.
