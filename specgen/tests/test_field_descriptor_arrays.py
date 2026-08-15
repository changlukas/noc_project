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


def test_header_fields_array_lists_all_twelve_fields():
    text = GENERATED.read_text()
    # Non-greedy match the array body; stops at `};` not at the first `}`.
    m = re.search(r"constexpr FieldDescriptor HEADER_FIELDS\[\]\s*=\s*\{(.*?)\};",
                  text, re.DOTALL)
    assert m, "HEADER_FIELDS[] array not emitted"
    body = m.group(1)
    # All 12 header fields present (in declaration order); the 48 b header
    # has no reserved bits, so none are skipped.
    for name in ["axi_ch", "src_id", "dst_id", "fixed_vc", "vc_id", "flit_tail",
                "ordering_req", "ordering_tag", "collective_op", "collective_mask",
                "dst_port_id", "src_port_id"]:
        assert f'"{name}"' in body, f"missing {name} in HEADER_FIELDS"


def test_payload_field_arrays_per_channel():
    text = GENERATED.read_text()
    for channel in ["AW", "AR", "NARROW_W", "DATA_W", "B", "NARROW_R", "DATA_R"]:
        assert f"constexpr FieldDescriptor {channel}_PAYLOAD_FIELDS[]" in text, \
            f"missing {channel}_PAYLOAD_FIELDS[] array"


def test_string_view_header_included():
    text = GENERATED.read_text()
    assert "#include <string_view>" in text
