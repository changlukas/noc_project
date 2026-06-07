#!/usr/bin/env python3
"""Mandatory ASCII byte check for maintained docs (spec sec 3.2).

Reads each file path argument as raw bytes; reports any byte > 0x7F.
Exits 0 if clean across all inputs, 1 if any non-ASCII byte found.

Usage:
    py -3 tools/lint_docs.py path1.md path2.md ...

Designed for use in make check on a curated set of maintained docs
(NOT archive, NOT normative spec, NOT sub-project guides, NOT
ATTRIBUTION.md or FEATURE_INVENTORY.md).
"""
import sys


def check_file(path: str) -> int:
    """Return count of non-ASCII bytes found in path."""
    with open(path, "rb") as f:
        data = f.read()
    bad = [(i, b) for i, b in enumerate(data) if b > 0x7F]
    if not bad:
        return 0
    # Compute line numbers for the first few bad bytes.
    sample = []
    for offset, byte in bad[:5]:
        # Count newlines before offset for line number.
        line = data[:offset].count(b"\n") + 1
        sample.append(f"  offset {offset} line {line} byte 0x{byte:02X}")
    print(
        f"LINT {path}: {len(bad)} non-ASCII byte(s) found\n"
        + "\n".join(sample),
        file=sys.stderr,
    )
    return len(bad)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: lint_docs.py FILE [FILE ...]", file=sys.stderr)
        return 2
    total = 0
    for path in sys.argv[1:]:
        total += check_file(path)
    if total == 0:
        print(f"lint_docs: {len(sys.argv) - 1} file(s) OK")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
