"""protocol_rules.md → ni_protocol_rule_index.json (minimal metadata lift-shift).

Extracts: id, severity, source_section, source_line, proto only.
Does NOT extract prose columns (condition_summary, channels, etc.).
"""

from __future__ import annotations
import json
import re
import warnings
from pathlib import Path


_PROTO_MAP = [
    ("Reset", "RESET"),
    ("CDC", "CDC"),
    ("AXI4 host-side", "AXI4"),
    ("NoC flit-side", "NOC"),
    ("CSR access", "CSR"),
    ("Configuration-knob", "CONFIG"),
    ("Interrupt", "INTERRUPT"),
]

_SECTION_PAT = re.compile(r"^##\s+(.+)$")
# Rule row: ID is uppercase+underscores, severity is FAIL/WARN/RECOMMEND/MUST/MUST NOT.
# Pattern: | ID | (anything) | (anything) | SEVERITY | ...
_ROW_PAT = re.compile(
    r"^\|\s*([A-Z][A-Z0-9_]+)\s*\|[^|]*\|[^|]*\|\s*(MUST NOT|MUST|SHOULD NOT|SHOULD|MAY)\s*\|"
)


def _infer_proto(section: str) -> str:
    """Map section heading to proto enum via substring match (case-insensitive)."""
    for key, val in _PROTO_MAP:
        if key.lower() in section.lower():
            return val
    # Default fallback (unreachable for known spec sections): warn so unknown
    # headings surface instead of silently being lumped into CONFIG.
    warnings.warn(
        f"_infer_proto: unrecognized section {section!r}, defaulting to CONFIG",
        stacklevel=2,
    )
    return "CONFIG"


def parse_protocol_rule_index(md_path: Path) -> list:
    """Extract structured metadata from protocol_rules.md.

    Returns list of dicts with: id, severity, source_section, source_line, proto.
    Prose columns (Condition, Required behavior, ARM SVA equivalent) are NOT parsed.

    The MD has tables structured:
        | ID | Condition | Required behavior | Severity | ARM SVA equivalent |
    under `## <section name>` headings. Sub-headings `### ...` belong to same proto.
    """
    text = md_path.read_text(encoding="utf-8")
    lines = text.splitlines()

    rules = []
    cur_section = None  # tracked from `## ` headings only

    for i, line in enumerate(lines, start=1):
        ms = _SECTION_PAT.match(line)
        if ms:
            cur_section = ms.group(1).strip()
            continue
        m = _ROW_PAT.match(line)
        if not m or cur_section is None:
            continue
        rid, severity = m.group(1), m.group(2)
        # Skip header rows: literal "ID" can't match UPPER pattern but guard anyway.
        if rid == "ID":
            continue
        rules.append({
            "id": rid,
            "severity": severity,
            "source_section": cur_section,
            "source_line": i,
            "proto": _infer_proto(cur_section),
        })
    return rules


def generate_ni_protocol_rule_index_json(md_dir, out_path: Path) -> dict:
    """Compose ni_protocol_rule_index.json from protocol_rules.md."""
    md_dir_path = Path(md_dir) if not isinstance(md_dir, Path) else md_dir
    version_file = md_dir_path.parent / "VERSION"
    if not version_file.exists():
        raise FileNotFoundError(f"spec/ni/VERSION not found at {version_file}")
    spec_version = version_file.read_text(encoding="utf-8").strip()

    result = {
        "$schema_version": "ni-spec/2.0",
        "meta": {
            "spec_version": spec_version,
        },
        "rules": parse_protocol_rule_index(md_dir_path / "protocol_rules.md"),
    }
    out_path_p = Path(out_path)
    out_path_p.parent.mkdir(parents=True, exist_ok=True)
    out_path_p.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    return result
