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
    # The 7 mandatory header fields present (in declaration order).
    for name in ["axi_ch", "src_id", "dst_id", "vc_id", "last", "rob_req", "rob_idx"]:
        assert f'"{name}"' in body, f"missing {name} in HEADER_FIELDS"
    # All 6 optional fields + rsvd are enabled=false; skipped.
    for name in ["noc_qos", "route_par", "commtype", "multicast", "seq", "flit_ecc", "rsvd"]:
        assert f'"{name}"' not in body, f"{name} should be skipped (enabled=false)"


def test_payload_field_arrays_per_channel():
    text = GENERATED.read_text()
    for channel in ["AW", "AR", "W", "B", "R"]:
        assert f"constexpr FieldDescriptor {channel}_PAYLOAD_FIELDS[]" in text, \
            f"missing {channel}_PAYLOAD_FIELDS[] array"


def test_string_view_header_included():
    text = GENERATED.read_text()
    assert "#include <string_view>" in text
