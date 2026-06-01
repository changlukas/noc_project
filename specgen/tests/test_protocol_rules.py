"""Protocol rule index parser + validator — Task 6 (minimal version)."""
from pathlib import Path
import json
import pytest
from ni_spec import generator, invariants, loader

SPEC_VALIDATE = Path(__file__).resolve().parent.parent
MD_DIR = SPEC_VALIDATE.parent / "spec" / "ni" / "doc"
PROTO_JSON = SPEC_VALIDATE / "generated" / "ni_protocol_rule_index.json"
PROTO_SCHEMA = SPEC_VALIDATE / "generated" / "ni_protocol_rule_index.schema.json"


def test_parser_finds_at_least_50_rules():
    """Spec has ~100 rules across 7 sections; parser should find most."""
    rules = generator.parse_protocol_rule_index(MD_DIR / "protocol_rules.md")
    assert len(rules) >= 50, f"expected >=50 rules, got {len(rules)}"


def test_id_uniqueness_on_real_data():
    """No duplicate ids in current spec."""
    rules = generator.parse_protocol_rule_index(MD_DIR / "protocol_rules.md")
    ids = [r["id"] for r in rules]
    assert len(ids) == len(set(ids)), f"duplicates: {[i for i in ids if ids.count(i)>1]}"


def test_reset_rules_have_proto_RESET():
    rules = generator.parse_protocol_rule_index(MD_DIR / "protocol_rules.md")
    reset_rules = [r for r in rules if r["id"].startswith("NI_RST_")]
    assert len(reset_rules) >= 1
    assert all(r["proto"] == "RESET" for r in reset_rules)


def test_severity_values_in_enum():
    rules = generator.parse_protocol_rule_index(MD_DIR / "protocol_rules.md")
    rfc2119 = {"MUST", "MUST NOT", "SHOULD", "SHOULD NOT", "MAY"}
    for r in rules:
        assert r["severity"] in rfc2119, f"{r['id']}: unexpected severity {r['severity']!r}"


def test_validator_catches_duplicate_id():
    """L2 check catches manually-injected duplicate."""
    bogus = {"rules": [
        {"id": "TEST", "severity": "FAIL", "source_section": "x", "source_line": 1, "proto": "RESET"},
        {"id": "TEST", "severity": "FAIL", "source_section": "y", "source_line": 2, "proto": "CDC"},
    ]}
    issues = invariants.check_protocol_rules_id_uniqueness(bogus)
    assert any("TEST" in i.message for i in issues)


def test_generated_json_passes_schema():
    """Real generated JSON validates against schema."""
    import jsonschema
    schema = json.loads(PROTO_SCHEMA.read_text(encoding="utf-8"))
    data = json.loads(PROTO_JSON.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(data)
