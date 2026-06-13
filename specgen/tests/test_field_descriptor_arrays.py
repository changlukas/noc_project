"""Verify cpp_packet.py emits FieldDescriptor arrays alongside LSB/MSB constants."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
GENERATED = REPO / "specgen/generated/cpp/ni_flit_constants.h"


def test_field_descriptor_struct_present():
    text = GENERATED.read_text()
    assert "struct FieldDescriptor" in text
    assert "std::string_view name" in text
    assert "int lsb" in text
    assert "int msb" in text


def test_header_fields_array_skips_disabled():
    text = GENERATED.read_text()
    # Non-greedy match the array body; stops at `};` not at the first `}`.
    m = re.search(r"constexpr FieldDescriptor HEADER_FIELDS\[\]\s*=\s*\{(.*?)\};",
                  text, re.DOTALL)
    assert m, "HEADER_FIELDS[] array not emitted"
    body = m.group(1)
    # All 12 enabled header fields present.
    for name in ["noc_qos", "axi_ch", "src_id", "dst_id", "vc_id", "route_par",
                 "last", "rob_req", "rob_idx", "seq", "commtype", "flit_ecc"]:
        assert f'"{name}"' in body, f"missing {name} in HEADER_FIELDS"
    # rsvd (derived padding) and multicast (width=0) are enabled=false; skipped.
    assert '"rsvd"' not in body, "rsvd should be skipped (enabled=false)"
    assert '"multicast"' not in body, "multicast should be skipped (enabled=false)"


def test_payload_field_arrays_per_channel():
    text = GENERATED.read_text()
    for channel in ["AW", "AR", "W", "B", "R"]:
        assert f"constexpr FieldDescriptor {channel}_PAYLOAD_FIELDS[]" in text, \
            f"missing {channel}_PAYLOAD_FIELDS[] array"


def test_string_view_header_included():
    text = GENERATED.read_text()
    assert "#include <string_view>" in text
