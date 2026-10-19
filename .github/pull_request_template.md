## Change and motivation

What problem does this solve? Link the issue, requirement and relevant contract.
State implemented / tested / planned / blocked accurately.

## Design and review summary

Risk profile and applicable coding rules; changed invariants; ownership/lifetime,
permissions, failure/rollback behavior; alternatives and why the selected approach
fits; revisit triggers and limitations. Identify owner-gated decisions and their
approval record. Single-agent contributions require the full technical decision,
correctness and verification summary described in AGENTS.md. State review type
honestly; AI-assisted review is not independent human qualification.

## Local environment and tested revision

- Source SHA (or base SHA plus exact patch), observation date/time and timezone:
- Host OS/architecture; compiler/linker/emulator and relevant dependency versions:
- Target/profile; flags, overrides, deterministic seeds and fixture/image hashes:
- Applicable local CI jobs and additional risk-based test coverage:

## Result data

| Command / profile | Expected and observed result | Status / exit code | Duration if measured | Sanitized log / artifact |
| --- | --- | --- | --- | --- |
| Fill with actual runs | Markers/counts or precise failure | PASS / FAIL / BLOCKED / NOT RUN / NOT APPLICABLE | Unmeasured if unavailable | Reproducible evidence |

Include failure reproductions and corrected regression results. Explain unrun or
inapplicable checks, environment restrictions, sampled coverage and next steps.
Keep remote CI links/results in a separate paragraph tied to their exact SHA.
Do not claim full leak coverage with leak scanning disabled. No secrets, private
host logs or generated binaries in source. Docs-only edits should explain why
emulator runs are not applicable; runtime changes need relevant native evidence.

## Documentation and handoff

Updated implementation/status docs and handoff; unresolved risks; next bounded
task. Assistance preference is optional and may change mid-session; it never
changes acceptance criteria. Do not close implementation issues for planning alone.
