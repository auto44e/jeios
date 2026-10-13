# SPDX-License-Identifier: Apache-2.0
"""Read-only candidate registry inspector; not an Android builder."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
config = json.loads((root / "config/targets.json").read_text())
if config.get("schema") != 1:
    raise SystemExit("Unsupported registry schema")
seen = set()
for target in config["targets"]:
    name = target["id"]
    if name in seen or target["status"] != "candidate":
        raise SystemExit("Duplicate or unqualified registry state")
    seen.add(name)
    if not target["blockers"]:
        raise SystemExit("Candidate must document unresolved qualification")
    print(f"{name}: CANDIDATE, NOT BUILD-QUALIFIED")
    print(f"  lunch: {target['lunch'] or 'unresolved'}")
    for blocker in target["blockers"]:
        print(f"  - {blocker}")
