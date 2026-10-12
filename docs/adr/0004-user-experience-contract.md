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
