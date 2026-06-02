"""C++ emitter for packet domain.

Consumes ni_spec.constants only -- no direct JSON parsing.
"""
from __future__ import annotations
from pathlib import Path
import sys

SPEC_VALIDATE = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(SPEC_VALIDATE))

from ni_spec import constants as C
from ni_spec.loader import load_doc


def _emit_padding_fields(spec) -> list[str]:
    out = []
    out.append("// --- padding fields list (enabled: false, width > 0) ---")
    out.append("struct PaddingFieldPos { const char* name; int lsb; int msb; };")
    entries = []
    for f in spec["flit"]["header_fields"]:
        if C.header_field_enabled(spec, f["name"]):
            continue
        width = C.header_field_width(spec, f["name"])
        if width == 0:
            continue
        pos = C.header_field_position(spec, f["name"])
        entries.append((f["name"], pos[0], pos[1]))
    out.append("constexpr PaddingFieldPos PADDING_FIELDS[] = {")
    if entries:
        out.append(",\n".join(
            f"  {{ \"{n}\", {lsb}, {msb} }}" for n, lsb, msb in entries))
    out.append("};")
    out.append(f"constexpr std::size_t PADDING_FIELDS_COUNT = {len(entries)};")
    out.append("")
    return out


def emit(packet_json: Path, spec_version: str) -> str:
    """Return C++ header body (no provenance banner -- caller prepends it)."""
    spec = load_doc(packet_json)

    out: list[str] = []
    out.append("#pragma once")
    out.append("#include <cstddef>")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace ni {")
    out.append("")

    out.append("// --- top-level flit widths (from flit.derived) ---")
    out.append(f"constexpr int FLIT_WIDTH        = {C.flit_width_resolved(spec)};")
    out.append(f"constexpr int HEADER_WIDTH      = {C.header_width_resolved(spec)};")
    out.append(f"constexpr int PAYLOAD_WIDTH     = {C.payload_width_resolved(spec)};")
    out.append(f"constexpr int LINK_WIDTH        = {C.link_width_resolved(spec)};")
    for k, val in (
        ("FLIT_DATA_WIDTH",   C.flit_data_width_resolved(spec)),
        ("HEADER_DATA_WIDTH", C.header_data_width_resolved(spec)),
        ("WSTRB_WIDTH",       C.wstrb_width_resolved(spec)),
    ):
        out.append(f"constexpr int {k:<15} = {val};")
    out.append("")

    out.append("// --- header field bit positions (from flit.header_fields) ---")
    out.append("namespace header {")
    for f in spec["flit"]["header_fields"]:
        n = f["name"].upper()
        width = C.header_field_width(spec, f["name"])
        enabled_val = "true" if C.header_field_enabled(spec, f["name"]) else "false"
        if width == 0:
            # width=0 reserved placeholder: emit WIDTH=0 + ENABLED only; no LSB/MSB (field not bit-addressable)
            out.append(f"constexpr int  {n}_WIDTH   = 0;  // reserved placeholder (width=0 -- not in flit)")
            out.append(f"constexpr bool {n}_ENABLED = {enabled_val};")
        else:
            pos = C.header_field_position(spec, f["name"])
            out.append(f"constexpr int  {n}_LSB     = {pos[0]};")
            out.append(f"constexpr int  {n}_MSB     = {pos[1]};")
            out.append(f"constexpr int  {n}_WIDTH   = {width};")
            out.append(f"constexpr bool {n}_ENABLED = {enabled_val};")
    out.extend(_emit_padding_fields(spec))
    out.append("}  // namespace header")
    out.append("")

    out.append("// --- payload widths per channel (from flit.payload_channels) ---")
    out.append("namespace payload {")
    for ch in spec["flit"]["payload_channels"]:
        out.append(f"constexpr int {ch['name']}_WIDTH = {C.payload_channel_width(spec, ch['name'])};")
    out.append("}  // namespace payload")
    out.append("")

    # --- payload field bit positions per channel ---
    out.append("// --- payload field bit positions (from flit.payload_channels) ---")
    for ch in spec["flit"]["payload_channels"]:
        ch_lower = ch["name"].lower()
        out.append(f"namespace payload::{ch_lower} {{")
        last_positioned = None  # last field that actually occupies bits (for static_assert)
        for f in ch["fields"]:
            pos = C.payload_field_position(spec, ch["name"], f["name"])
            n = f["name"].upper()
            if pos is None:
                # width=0 reserved placeholder: emit WIDTH only; no LSB/MSB (field not bit-addressable)
                out.append(f"constexpr int {n}_WIDTH   = 0;  // reserved placeholder (width=0 -- not in flit)")
                continue
            out.append(f"constexpr int {n}_LSB     = {pos[0]};")
            out.append(f"constexpr int {n}_MSB     = {pos[1]};")
            out.append(f"constexpr int {n}_WIDTH   = {pos[1] - pos[0] + 1};")
            last_positioned = n
        # static_assert: last positioned field's MSB + 1 == channel's payload_width
        if last_positioned is not None:
            out.append(f"static_assert({last_positioned}_MSB + 1 == ni::payload::{ch['name']}_WIDTH, "
                       f"\"payload[{ch['name']}] field positions inconsistent with channel width\");")
        out.append(f"}}  // namespace payload::{ch_lower}")
        out.append("")

    out.append("// --- all field widths (from flit.field_widths) ---")
    out.append("namespace width {")
    for name, val in spec["flit"].get("field_widths", {}).items():
        out.append(f"constexpr int {name:<22} = {val};")
    out.append("}  // namespace width")
    out.append("")

    # --- axi_ch encoding (from header_fields[axi_ch].encoding) ---
    enc = C.axi_channel_encoding(spec)
    if enc:
        out.append("// --- axi_ch encoding (from flit.header_fields[axi_ch].encoding) ---")
        for name, value in sorted(enc.items(), key=lambda kv: kv[1]):
            out.append(f"constexpr int AXI_CH_{name:<3} = {value};")
        out.append("")

    # --- static_assert arithmetic invariants (design doc sec 6.4) ---
    # Only equality invariants; no tiling/cross-ref/width_param eval.
    out.append("// --- static_assert: arithmetic equality invariants (design doc sec 6.4) ---")

    out.append(
        "static_assert(FLIT_WIDTH == HEADER_WIDTH + PAYLOAD_WIDTH,"
        " \"Flit width arithmetic inconsistent: HEADER_WIDTH + PAYLOAD_WIDTH must equal FLIT_WIDTH\");"
    )

    # SECDED bound: 2^parity_bits >= data_bits + parity_bits + 1
    # flit_ecc covers FLIT_DATA_WIDTH data bits.
    # We compute the literal check in Python and emit a compile-time boolean constant assertion.
    flit_data = C.flit_data_width_resolved(spec)
    flit_ecc_w = None
    try:
        flit_ecc_w = C.header_field_width(spec, "flit_ecc")
    except Exception:
        flit_ecc_w = None
    if flit_data is not None and flit_ecc_w is not None:
        # Emit using header:: qualified name so the constant is in scope.
        out.append(
            "static_assert((1 << header::FLIT_ECC_WIDTH) >= FLIT_DATA_WIDTH + header::FLIT_ECC_WIDTH + 1,"
            " \"SECDED bound: 2^parity_bits must be >= data_bits + parity_bits + 1\");"
        )
    out.append("")

    out.append("}  // namespace ni")
    return "\n".join(out) + "\n"
