# ADR 0005: License selection pending

Status: pending owner decision, 2026-10-12.

The owner requested a fresh project but did not choose a distribution license.
Do not silently choose GPL, BSD, MIT or another license. A public GitHub repository
is not by itself a general permission grant to redistribute modified source.
No third-party code or assets were imported in the scaffold.

Before accepting external contributions/imports, the owner should choose the
project license and contributor policy. Record per-component provenance and
terms. Linux GPL code, permissive BSD code, ReactOS components, userspace
libraries, GPU firmware and proprietary driver interfaces require specific
review; architectural resemblance does not determine license compatibility.
