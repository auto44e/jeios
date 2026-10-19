# ADR 0002: Separate modularity from isolation

Status: provisional architecture, 2026-10-12.

Use layered build modules now and a small privileged mechanism core over time.
Isolate suitable services and drivers using explicit capabilities. Keep one
scheduler and VM authority. NT-inspired objects/asynchronous requests structure
interfaces; they do not require NT binary driver compatibility.

Privileged modules share kernel trust and cannot be safely recovered after
arbitrary corruption. Dynamic loading is deferred until lifecycle, mappings,
quiescence and authorization exist. Userspace driver containment additionally
requires validated DMA/interrupt isolation or an explicitly weaker trust tier.

Revisit service placement using measured latency and recovery tests after M3.

## Decision-record annotation (reconstructed — sweep 2026-10-19)

Per ADR 0000; reconstructed from this ADR's text only. Part 4: no transcript
retained.

**Decision + why (as recorded).** Separate modularity (layered build modules now)
from isolation (a small privileged mechanism core + capability-isolated services
over time); one scheduler + VM authority; NT-inspired objects/async requests
structure interfaces without NT binary-driver compatibility. Status: provisional
architecture (2026-10-12).

**Predictions if other choices were made (reconstructed).**
- *Conflate modularity with isolation (isolate every module now):* the
  Consequences record why this was NOT done — "privileged modules share kernel
  trust and cannot be safely recovered after arbitrary corruption," and userspace
  driver containment "requires validated DMA/interrupt isolation or an explicitly
  weaker trust tier" that does not yet exist.
- *Allow dynamic loading now:* explicitly deferred "until lifecycle, mappings,
  quiescence and authorization exist"; doing it early would ship an
  unrecoverable/untrusted load path.
- *Multiple schedulers / VM authorities:* not in the record as considered;
  consequence **unknown — not in the record**.

**Flex options to consider in future (reconstructed).** Already carries an in-text
revisit: "Revisit service placement using measured latency and recovery tests
after M3." Firm until then. `FLEX (planning review)`: service-placement is the
planning-agent item.
