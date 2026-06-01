"""Foundation gate tests — Task 1."""
from pathlib import Path

from ni_spec import loader
from ni_spec import constants as C

SPECGEN_ROOT = Path(__file__).resolve().parent.parent
PACKET_JSON = SPECGEN_ROOT / "generated" / "json" / "ni_packet.json"


def test_load_spec_version_returns_string():
    """spec_version comes from spec/ni/VERSION single source."""
    v = loader.load_spec_version()
    assert isinstance(v, str)
    assert v.startswith("v")
    assert v.count(".") == 2  # semver


# ---------------------------------------------------------------------------
# enabled field: constants API tests
# ---------------------------------------------------------------------------

def _load_packet():
    return loader.load_doc(PACKET_JSON)


def test_header_field_enabled_returns_true_for_functional():
    """header_field_enabled returns True for a functional field."""
    spec = _load_packet()
    assert C.header_field_enabled(spec, "src_id") is True
    assert C.header_field_enabled(spec, "dst_id") is True
    assert C.header_field_enabled(spec, "last") is True
    assert C.header_field_enabled(spec, "rob_req") is True
    assert C.header_field_enabled(spec, "rob_idx") is True
    assert C.header_field_enabled(spec, "vc_id") is True


def test_header_field_enabled_returns_false_for_padding():
    """header_field_enabled returns False for padding fields."""
    spec = _load_packet()
    assert C.header_field_enabled(spec, "route_par") is False
    assert C.header_field_enabled(spec, "rsvd_commtype") is False
    assert C.header_field_enabled(spec, "multicast") is False
    assert C.header_field_enabled(spec, "flit_ecc") is False
    # axi_ch is functional (MUST for AXI request/response routing)
    assert C.header_field_enabled(spec, "axi_ch") is True


def test_header_field_enabled_raises_for_unknown():
    """header_field_enabled raises KeyError for unknown field name."""
    import pytest
    spec = _load_packet()
    with pytest.raises(KeyError):
        C.header_field_enabled(spec, "nonexistent_field")


def test_header_fields_padding_list():
    """header_fields_padding returns exactly the four padding field names."""
    spec = _load_packet()
    padding = C.header_fields_padding(spec)
    assert set(padding) == {"route_par", "rsvd_commtype", "multicast", "flit_ecc"}


def test_header_fields_padding_json_has_enabled_bool():
    """All header_fields entries in JSON must have enabled as a boolean."""
    spec = _load_packet()
    for f in spec["flit"]["header_fields"]:
        assert "enabled" in f, f"Field {f['name']!r} missing enabled key"
        assert isinstance(f["enabled"], bool), f"Field {f['name']!r} enabled is not bool"


def test_parser_extracts_enabled_from_md():
    """parser extracts Enabled column correctly from packet_format.md."""
    from ni_spec.generator import parse_header_fields
    md_path = SPECGEN_ROOT.parent / "spec" / "ni" / "doc" / "packet_format.md"
    md_text = md_path.read_text(encoding="utf-8")
    fields = parse_header_fields(md_text)
    field_map = {f["name"]: f["enabled"] for f in fields}
    # Padding fields (stubbed to zero, future-implementation)
    assert field_map["route_par"] is False
    assert field_map["rsvd_commtype"] is False
    assert field_map["multicast"] is False
    assert field_map["flit_ecc"] is False
    # Functional fields (driven by hardware)
    assert field_map["axi_ch"] is True   # MUST for AXI channel routing
    assert field_map["src_id"] is True
    assert field_map["last"] is True


def test_parser_defaults_enabled_true_when_column_missing():
    """If the Enabled column is absent, parser defaults to enabled=True."""
    from ni_spec.generator import parse_header_fields
    # Construct a minimal §2.1 table WITHOUT an Enabled column
    md_text = """
### 2.1 Bit Allocation

| Field | Width Symbol | Default Range | Stage | Description |
|-------|--------------|---------------|-------|-------------|
| test_field | TEST_WIDTH | [3:0] | routing | Test field |
"""
    fields = parse_header_fields(md_text)
    assert len(fields) == 1
    assert fields[0]["name"] == "test_field"
    assert fields[0].get("enabled", True) is True
