#!/usr/bin/env python3
"""Lint tests/scenarios/ — 8 invariants per design spec §5.7.

Exits 0 on clean, prints errors and exits 1 on any violation.
"""
import os
import re
import sys
import yaml

ROOT = os.path.join(os.path.dirname(__file__), "..", "tests", "scenarios")
NAME_RE = re.compile(
    r"^AX4-(BAS|BUR|BND|ORD|EXC|RSP|STR|HSH|INF)-\d{3}_[a-z0-9_]+$"
)
CAT_CATEGORY = {
    "BAS": "basic", "BUR": "burst", "BND": "boundary", "ORD": "ordering",
    "EXC": "exclusive", "RSP": "response", "STR": "stress",
    "HSH": "handshake", "INF": "infrastructure",
}

def main() -> int:
    errors: list[str] = []
    seen_names: dict[str, str] = {}
    ax4_count = 0

    for entry in sorted(os.listdir(ROOT)):
        path = os.path.join(ROOT, entry)
        if not os.path.isdir(path):
            continue
        if entry in ("_data",):  # known shared data dir
            continue
        if not entry.startswith("AX4-"):
            # Invariant 1: only AX4-* dirs allowed (after migration).
            errors.append(f"unknown dir: {entry} (expected AX4-CAT-NNN_slug)")
            continue
        ax4_count += 1
        yaml_path = os.path.join(path, "scenario.yaml")
        # Invariant 2: each AX4-* dir has scenario.yaml.
        if not os.path.isfile(yaml_path):
            errors.append(f"{entry}: missing scenario.yaml")
            continue
        # Invariant 3: parses successfully.
        try:
            with open(yaml_path) as f:
                doc = yaml.safe_load(f)
        except Exception as e:
            errors.append(f"{entry}: YAML parse error: {e}")
            continue
        md = doc.get("metadata") or {}
        name = md.get("name", "")
        category = md.get("category", "")
        # Invariant 4: name equals dir basename.
        if name != entry:
            errors.append(f"{entry}: metadata.name '{name}' != dir basename")
        # Invariant 5: name globally unique.
        if name in seen_names:
            errors.append(f"{entry}: duplicate metadata.name; also at {seen_names[name]}")
        else:
            seen_names[name] = entry
        # Invariant 6: name matches regex.
        if name and not NAME_RE.match(name):
            errors.append(f"{entry}: metadata.name '{name}' fails AX4-CAT-NNN_slug regex")
        # Invariant 7: category matches CAT prefix.
        if name and len(name) > 7:
            cat3 = name[4:7]
            expect = CAT_CATEGORY.get(cat3)
            if expect is None:
                errors.append(f"{entry}: unknown CAT prefix '{cat3}'")
            elif expect != category:
                errors.append(
                    f"{entry}: CAT '{cat3}' implies category '{expect}', got '{category}'"
                )

    # Invariant 8: non-empty scenario set.
    # Skipped in commit 1 (zero AX4-* dirs is the legitimate transient state);
    # the lint becomes mandatory in commit 2.
    if "--require-nonempty" in sys.argv and ax4_count == 0:
        errors.append("tests/scenarios/ contains zero AX4-* dirs")

    if errors:
        for e in errors:
            print(f"LINT: {e}", file=sys.stderr)
        return 1
    print(f"lint: {ax4_count} scenario dirs OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
