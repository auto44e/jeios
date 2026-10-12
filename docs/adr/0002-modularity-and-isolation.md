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
