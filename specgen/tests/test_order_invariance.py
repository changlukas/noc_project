"""Verify the elaborator iterates source declaration order. Catches K-1 tripwire:
if anyone applies sorted() inside the helper or elaborator, byte-identical
golden tests still pass (alphabetic happens to match declaration in many
cases) but THIS test specifically asserts the names appear in declaration
order."""
from __future__ import annotations
import json
import re
from pathlib import Path

SPEC_VALIDATE = Path(__file__).resolve().parent.parent
HEADER_PATH = SPEC_VALIDATE / "include" / "ni_flit_constants.h"
JSON_PATH   = SPEC_VALIDATE / "generated" / "ni_packet.json"


def _field_widths_section(text: str) -> str:
    """Slice the '--- all field widths (from flit.field_widths) ---' block.

    The header also emits <NAME>_WIDTH entries from header_fields above and
    payload widths in between; only the field_widths block reflects JSON
    insertion order, so scoping the regex to this section is required to
    measure the invariant the docstring describes.
    """
    m = re.search(
        r"//\s*---\s*all field widths.*?---\s*\n(.*?)(?=\n//\s*---|\Z)",
        text,
        flags=re.DOTALL,
    )
    assert m, "field_widths section marker missing from generated header"
    return m.group(1)


def test_field_widths_declaration_order_preserved_in_header():
    """ni_flit_constants.h emits `constexpr int <NAME>_WIDTH = ...` lines for
    each field_widths entry; their order must match field_widths{} insertion
    order in the JSON (= MD source declaration order)."""
    spec = json.loads(JSON_PATH.read_text(encoding="utf-8"))
    text = HEADER_PATH.read_text(encoding="utf-8")
    section = _field_widths_section(text)
    header_order = re.findall(r"constexpr\s+int\s+(\w+_WIDTH)\s*=", section)
    fw_names_in_order = list(spec["flit"]["field_widths"].keys())
    assert header_order == fw_names_in_order, (
        f"field_widths declaration order broken!\n"
        f"  expected: {fw_names_in_order}\n"
        f"  got:      {header_order}"
    )


def test_header_fields_declaration_order_preserved():
    """Header fields (DST_ID_LSB, SRC_ID_LSB, ...) must appear in declaration order.

    Width-0 fields (e.g. noc_qos when NOC_QOS_WIDTH=0) are skipped because the
    elaborator does not emit _LSB / _MSB constants for them (not bit-addressable).
    Width is resolved via constants.header_field_width since PP-6 dropped the
    pre-computed ``width`` key from each header_fields entry.
    """
    from ni_spec import constants as C
    spec = json.loads(JSON_PATH.read_text(encoding="utf-8"))
    text = HEADER_PATH.read_text(encoding="utf-8")
    lsb_order = re.findall(r"constexpr\s+int\s+(\w+)_LSB\s*=", text)
    expected = []
    for f in spec["flit"]["header_fields"]:
        if C.header_field_width(spec, f["name"]) != 0:
            expected.append(f["name"].upper())
    assert lsb_order == expected
