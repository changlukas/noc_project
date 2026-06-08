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
    """header_field_enabled returns False for padding fields.

    Post fixed-56b header refactor: the only enabled=false field is the
    synthetic ``rsvd`` (derived padding that anchors HEADER_TOTAL_WIDTH=56).
    All previously-disabled fields (route_par, commtype, multicast, flit_ecc)
    are now enabled per docs/image/header.jpg.
    """
    spec = _load_packet()
    assert C.header_field_enabled(spec, "rsvd") is False
    # All other fields are functional (driven by hardware)
    assert C.header_field_enabled(spec, "axi_ch") is True
    assert C.header_field_enabled(spec, "route_par") is True
    assert C.header_field_enabled(spec, "commtype") is True
    assert C.header_field_enabled(spec, "multicast") is True
    assert C.header_field_enabled(spec, "flit_ecc") is True


def test_header_field_enabled_raises_for_unknown():
    """header_field_enabled raises KeyError for unknown field name."""
    import pytest
    spec = _load_packet()
    with pytest.raises(KeyError):
        C.header_field_enabled(spec, "nonexistent_field")


def test_header_fields_padding_list():
    """header_fields_padding returns the single derived padding field.

    Post fixed-56b refactor: only the synthetic ``rsvd`` field is padding.
    """
    spec = _load_packet()
    padding = C.header_fields_padding(spec)
    assert set(padding) == {"rsvd"}


def test_header_fields_padding_json_has_enabled_bool():
    """All header_fields entries in JSON must have enabled as a boolean."""
    spec = _load_packet()
    for f in spec["flit"]["header_fields"]:
        assert "enabled" in f, f"Field {f['name']!r} missing enabled key"
        assert isinstance(f["enabled"], bool), f"Field {f['name']!r} enabled is not bool"

