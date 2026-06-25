"""Perf JSON parity checker.

Compares two perf.json files for structural equality, ignoring the noc.links
field (which is re-emitted by the generator and may differ between runs).

CLI:
    python3 check_perf_parity.py <a.json> <b.json>

Exit 0 if equal (modulo noc.links), exit 1 otherwise.
"""
import json
import sys


def load(path: str) -> dict:
    with open(path) as f:
        d = json.load(f)
    d.get("noc", {}).pop("links", None)
    return d


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <a.json> <b.json>", file=sys.stderr)
        return 2
    a = load(sys.argv[1])
    b = load(sys.argv[2])
    if a == b:
        return 0
    print(f"MISMATCH: {sys.argv[1]} vs {sys.argv[2]}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
