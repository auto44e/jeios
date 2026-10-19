# ADR 0004: Consistent behavior with public customization

Status: accepted product direction from conversation; implementation deferred.

Provide one coherent native app contract with customizable tokens/components,
replaceable shell and window-management policy. Stock and third-party shells use
the same public service APIs. Data themes are distinct from executable widgets.

Complexity settings expose controls independently by area; authority and security
policy do not change with difficulty. Trustworthy permission prompts and
accessibility must survive customization. Compositor live replacement is not
promised until a tested protocol/lifetime design supports it.

Toolkit, compositor protocol and UI implementation language remain open choices.

## Decision-record annotation (reconstructed — sweep 2026-10-19)

Per ADR 0000; reconstructed from this ADR's text only. Part 4: no transcript
retained.

**Decision + why (as recorded).** One coherent native app contract with
customizable tokens/components, a replaceable shell and window-management policy;
stock and third-party shells use the same public service APIs; data themes are
distinct from executable widgets; complexity settings expose controls by area
WITHOUT changing authority/security policy; permission prompts + accessibility
survive customization. Status: accepted product direction; implementation
deferred.

**Predictions if other choices were made (reconstructed).**
- *Let difficulty/complexity settings change authority or security policy:* the
  decision explicitly forbids it — doing so would make the security posture a
  function of a UI setting (a privilege-escalation-by-preference hazard).
- *Promise live compositor replacement now:* explicitly "not promised until a
  tested protocol/lifetime design supports it"; promising early would commit to an
  untested hot-swap lifetime.
- *Merge themes and executable widgets:* kept distinct; merging would let a
  "theme" ship code.

**Flex options to consider in future (reconstructed).** In-text open item:
"Toolkit, compositor protocol and UI implementation language remain open choices"
(`FLEX (planning review)`). The security-invariants-survive-customization rule is
firm.
