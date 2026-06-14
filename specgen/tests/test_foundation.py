"""Foundation gate tests — Task 1."""
from pathlib import Path

from ni_spec import loader
from ni_spec import constants as C

SPECGEN_ROOT = Path(__file__).resolve().parent.parent
PACKET_JSON = SPECGEN_ROOT / "generated" / "json" / "ni_packet.json"


def test_load_spec_version_returns_string():
    """spec_version comes from specgen/generated/json/VERSION single source."""
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
    """header_field_enabled returns False for disabled fields.

    All 6 optional header fields are turned off: ``noc_qos``, ``route_par``,
    ``commtype``, ``multicast``, ``seq``, ``flit_ecc`` (width-0 reserved
    placeholders), plus the synthetic ``rsvd`` derived padding. The 7
    mandatory fields stay enabled.
    """
    spec = _load_packet()
    assert C.header_field_enabled(spec, "rsvd") is False
    # The 6 optional fields are all disabled.
    assert C.header_field_enabled(spec, "noc_qos") is False
    assert C.header_field_enabled(spec, "route_par") is False
    assert C.header_field_enabled(spec, "commtype") is False
    assert C.header_field_enabled(spec, "multicast") is False
    assert C.header_field_enabled(spec, "seq") is False
    assert C.header_field_enabled(spec, "flit_ecc") is False
    # Mandatory field stays enabled.
    assert C.header_field_enabled(spec, "axi_ch") is True


def test_header_field_enabled_raises_for_unknown():
    """header_field_enabled raises KeyError for unknown field name."""
    import pytest
    spec = _load_packet()
    with pytest.raises(KeyError):
        C.header_field_enabled(spec, "nonexistent_field")


def test_header_fields_padding_list():
    """header_fields_padding returns every enabled=false field.

    All 6 optional header fields are disabled plus the synthetic ``rsvd``
    derived padding.
    """
    spec = _load_packet()
    padding = C.header_fields_padding(spec)
    assert set(padding) == {"noc_qos", "route_par", "commtype", "multicast",
                            "seq", "flit_ecc", "rsvd"}


def test_header_fields_padding_json_has_enabled_bool():
    """All header_fields entries in JSON must have enabled as a boolean."""
    spec = _load_packet()
    for f in spec["flit"]["header_fields"]:
        assert "enabled" in f, f"Field {f['name']!r} missing enabled key"
        assert isinstance(f["enabled"], bool), f"Field {f['name']!r} enabled is not bool"

