# JeiOS Mobile

A proposed AOSP-adjacent mobile member of the JeiOS ecosystem. This is an
exportable repository seed, temporarily preserved in auto44e/jeios until the
separate `auto44e/jeios-mobile` repository can be created. No Android source,
vendor binaries, flashable image or working lunch product is included.

The initial engineering proposal uses AOSP framework/runtime and its supported
Linux-based device stack. The fresh C JeiOS kernel remains a separate project;
this seed does not claim Android runs on that kernel. Share ecosystem protocols,
security semantics and user capabilities before attempting kernel convergence.

- [Architecture and parity](docs/DESIGN.md)
- [Build and device qualification](docs/BUILD.md)
- [Roadmap and handoff](docs/HANDOFF.md)
- [Target registry](config/targets.json)

First virtual target: Cuttlefish. First physical candidate: base Pixel 9 (`tokay`).
Base Pixel 10 (`frankel`) is the next candidate, pending independently verified
source/vendor/boot feasibility. These are candidates, not qualified lunch targets.

Run `python3 tools/target_plan.py` to inspect readiness. It performs no downloads,
builds, flashing or credential operations. Original scaffold code/docs are
Apache-2.0; Android, Linux and vendor components retain their own licenses.
