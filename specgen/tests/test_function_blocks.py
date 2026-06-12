"""Function blocks JSON + validator — Task 5."""
from pathlib import Path
import json
import re
from ni_spec import loader, invariants

SPECGEN_ROOT = Path(__file__).resolve().parent.parent
FB_JSON = SPECGEN_ROOT / "source" / "noc_function_blocks.json"
FB_SCHEMA = SPECGEN_ROOT / "source" / "noc_function_blocks.schema.json"


def test_function_blocks_json_exists():
    assert FB_JSON.exists()


def test_function_blocks_passes_schema():
    import jsonschema
    data = json.loads(FB_JSON.read_text(encoding="utf-8"))
    schema = json.loads(FB_SCHEMA.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(data)


def test_two_blocks_nmu_nsu():
    data = json.loads(FB_JSON.read_text(encoding="utf-8"))
    names = [b["name"] for b in data["blocks"]]
    assert "NMU" in names and "NSU" in names


def test_at_least_one_feature_per_block():
    data = json.loads(FB_JSON.read_text(encoding="utf-8"))
    for block in data["blocks"]:
        assert len(block["features"]) >= 1, f"{block['name']} has no features"


def test_id_pattern_matches_block():
    data = json.loads(FB_JSON.read_text(encoding="utf-8"))
    for block in data["blocks"]:
        block_name = block["name"]
        for feat in block["features"]:
            assert feat["id"].startswith(f"FEAT-{block_name}-"), \
                f"{feat['id']} doesn't start with FEAT-{block_name}-"


def test_summary_length_under_200():
    data = json.loads(FB_JSON.read_text(encoding="utf-8"))
    for block in data["blocks"]:
        for feat in block["features"]:
            assert len(feat["summary"]) <= 200, \
                f"{feat['id']} summary too long ({len(feat['summary'])} chars)"


def test_mode_identifiers_valid_for_cpp_sv():
    data = json.loads(FB_JSON.read_text(encoding="utf-8"))
    pattern = re.compile(r"^[A-Z][A-Za-z0-9_]*$")
    for block in data["blocks"]:
        for feat in block["features"]:
            for mode in feat.get("modes", []):
                assert pattern.match(mode), f"mode {mode!r} not valid identifier"


def test_xref_packet_fields_exist():
    fb = json.loads(FB_JSON.read_text(encoding="utf-8"))
    pkt = loader.load_doc(SPECGEN_ROOT / "generated" / "json" / "ni_packet.json")
    issues = invariants.check_blocks_xref_packet(fb, pkt)
    err = [i for i in issues if i.severity == "ERROR"]
    assert not err, f"cross-ref errors to packet: {[i.message for i in err]}"


def test_xref_registers_exist():
    fb = json.loads(FB_JSON.read_text(encoding="utf-8"))
    regs = loader.load_doc(SPECGEN_ROOT / "generated" / "json" / "ni_registers.json")
    issues = invariants.check_blocks_xref_registers(fb, regs)
    err = [i for i in issues if i.severity == "ERROR"]
    assert not err, f"cross-ref errors to registers: {[i.message for i in err]}"


def test_compile_time_params_unique_across_features():
    fb = json.loads(FB_JSON.read_text(encoding="utf-8"))
    issues = invariants.check_blocks_param_uniqueness(fb)
    err = [i for i in issues if i.severity == "ERROR"]
    assert not err, f"param uniqueness: {[i.message for i in err]}"
