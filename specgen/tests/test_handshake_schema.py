"""Validator tests for constants.yaml + interface_handshake.json.

Covers all §4.2 rules from the design spec.
"""
from pathlib import Path
import pytest

from ni_spec.handshake_schema import (
    load_constants,
    load_interfaces,
    HandshakeSchemaError,
)

SOURCE_DIR = Path(__file__).resolve().parent.parent / "source"


# ---- constants.yaml positive cases ----

def test_load_constants_returns_expected_shape():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    assert c["schema_version"] == "1.0"
    assert c["axi"]["ID_WIDTH"]["default"] == 8
    assert c["axi"]["DATA_WIDTH"]["allowed"] == [32, 64, 128, 256, 512, 1024]
    assert c["noc"]["FLIT_WIDTH"]["sv_symbol"] == "NI_NOC_FLIT_WIDTH_DFLT"
    assert c["noc"]["SLAVE_VC_BUFFER_DEPTH"]["default"] == 4
    assert c["derived"]["WSTRB_WIDTH"]["expression"] == "DATA_WIDTH / 8"


# ---- constants.yaml naming discipline ----

def test_rejects_abbreviated_w_suffix(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "noc:\n"
        "  FLIT_W:\n"
        "    type: int\n    default: 408\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"FLIT_W.*abbreviated.*_WIDTH"):
        load_constants(bad)


def test_rejects_lowercase_param_name(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  id_width:\n"
        "    type: int\n    default: 8\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"UPPER_SNAKE_CASE"):
        load_constants(bad)


# ---- constants.yaml structural validation ----

def test_rejects_unknown_top_level_key(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text('schema_version: "1.0"\nbogus: {}\n')
    with pytest.raises(HandshakeSchemaError, match=r"unknown top-level"):
        load_constants(bad)


def test_rejects_unknown_per_param_field(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n    default: 8\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
        "    bogus_field: true\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown.*bogus_field"):
        load_constants(bad)


def test_rejects_missing_required_field(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n    type: int\n    default: 8\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"missing.*sv_symbol"):
        load_constants(bad)


def test_rejects_wrong_type(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: bool\n    default: true\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown type.*bool"):
        load_constants(bad)


def test_rejects_bool_default_for_int_type(tmp_path):
    """YAML bool is a subclass of int — must be rejected when type: int."""
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n    default: true\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"default.*not int"):
        load_constants(bad)


def test_rejects_string_default_for_int_type(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        '    type: int\n    default: "8"\n'
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"default.*not int"):
        load_constants(bad)


def test_rejects_plain_param_with_derived_only_field(tmp_path):
    """A plain axi/noc param must NOT accept 'expression' (only valid on derived)."""
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n    default: 8\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
        '    expression: "1 + 1"\n'
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown.*expression"):
        load_constants(bad)


def test_rejects_derived_param_with_plain_only_field(tmp_path):
    """A derived param must NOT accept 'default' (defaults come from expression evaluation)."""
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  DATA_WIDTH:\n"
        "    type: int\n    default: 256\n"
        "    sv_symbol: A\n    cpp_symbol: a\n"
        "derived:\n"
        "  WSTRB_WIDTH:\n"
        "    type: int\n"
        '    expression: "DATA_WIDTH / 8"\n'
        "    default: 99\n"
        "    sv_symbol: B\n    cpp_symbol: b\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown.*default"):
        load_constants(bad)


# ---- range/allowed checks ----

def test_rejects_default_above_max(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n    default: 100\n    min: 1\n    max: 32\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"default 100.*max 32"):
        load_constants(bad)


def test_rejects_default_not_in_allowed(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  DATA_WIDTH:\n"
        "    type: int\n    default: 33\n    allowed: [32, 64]\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"default 33 not in allowed"):
        load_constants(bad)


# ---- derived expression checks ----

def test_rejects_derived_referencing_undefined_param(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "derived:\n"
        "  FOO_WIDTH:\n"
        "    type: int\n"
        '    expression: "UNDEFINED_PARAM / 2"\n'
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"undefined.*UNDEFINED_PARAM"):
        load_constants(bad)


def test_rejects_circular_derived_expression(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "derived:\n"
        "  A_WIDTH:\n"
        "    type: int\n"
        '    expression: "B_WIDTH"\n'
        "    sv_symbol: A_SYM\n    cpp_symbol: a_sym\n"
        "  B_WIDTH:\n"
        "    type: int\n"
        '    expression: "A_WIDTH"\n'
        "    sv_symbol: B_SYM\n    cpp_symbol: b_sym\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"undefined.*B_WIDTH"):
        load_constants(bad)


def test_rejects_derived_constraint_violation(tmp_path):
    """If DATA_WIDTH default doesn't satisfy WSTRB_WIDTH constraint, error."""
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  DATA_WIDTH:\n"
        "    type: int\n    default: 33\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
        "derived:\n"
        "  WSTRB_WIDTH:\n"
        "    type: int\n"
        '    expression: "DATA_WIDTH / 8"\n'
        '    constraint: "DATA_WIDTH % 8 == 0"\n'
        "    sv_symbol: Y\n    cpp_symbol: y\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"constraint.*violated"):
        load_constants(bad)


# ---- interface_handshake.json positive case (gated on Task 3 — these will fail until Task 3 creates the file) ----

def test_load_interfaces_returns_three_named_interfaces():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    if not (SOURCE_DIR / "interface_handshake.json").exists():
        pytest.skip("interface_handshake.json not yet present (created in Task 3)")
    data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
    assert set(data["interfaces"].keys()) == {
        "axi4_intf", "noc_req_intf", "noc_rsp_intf"
    }


def test_noc_req_intf_protocol_semantics_complete():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    if not (SOURCE_DIR / "interface_handshake.json").exists():
        pytest.skip("interface_handshake.json not yet present (created in Task 3)")
    data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
    sem = data["interfaces"]["noc_req_intf"]["protocol_semantics"]
    assert sem["credit_return_encoding"]["scheme"] == "per_vc_credit_pulse_vector"
    assert sem["credit_return_encoding"]["onehot_check_required"] is False
    assert sem["initial_credits"]["value_per_vc"] == "SLAVE_VC_BUFFER_DEPTH"
    assert sem["combinational_loops"].startswith("forbidden")


# ---- interface_handshake.json negative cases ----

def test_rejects_interface_with_unknown_kind(tmp_path):
    c = load_constants(SOURCE_DIR / "constants.yaml")
    bad = tmp_path / "bad.json"
    bad.write_text('{"schema_version":"1.0","interfaces":{"x":{"kind":"weird","parameters":[],"modports":["m"]}}}')
    with pytest.raises(HandshakeSchemaError, match=r"unknown.*kind.*weird"):
        load_interfaces(bad, c)


def test_rejects_interface_parameter_referencing_unknown_const(tmp_path):
    c = load_constants(SOURCE_DIR / "constants.yaml")
    bad = tmp_path / "bad.json"
    bad.write_text(
        '{"schema_version":"1.0","interfaces":{'
        '"axi4_intf":{"kind":"axi4","channels":["AW"],'
        '"parameters":[{"name":"X","constants_yaml_key":"noc.BOGUS"}],'
        '"modports":["master","slave"]}'
        '}}'
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown constants.yaml key.*noc\.BOGUS"):
        load_interfaces(bad, c)


def test_rejects_interface_with_empty_modports(tmp_path):
    c = load_constants(SOURCE_DIR / "constants.yaml")
    bad = tmp_path / "bad.json"
    bad.write_text(
        '{"schema_version":"1.0","interfaces":{'
        '"x":{"kind":"axi4","channels":["AW"],"parameters":[],"modports":[]}'
        '}}'
    )
    with pytest.raises(HandshakeSchemaError, match=r"empty modports"):
        load_interfaces(bad, c)
