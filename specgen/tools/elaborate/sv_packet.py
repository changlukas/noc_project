"""SV emitter for packet domain.

Produces rtl_pkg/ni_flit_pkg.sv.
Uses localparam int unsigned for all integer constants (design doc §6.2).
Consumes ni_spec.constants only -- no direct JSON parsing.
"""
from __future__ import annotations
from pathlib import Path
import sys

SPEC_VALIDATE = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(SPEC_VALIDATE))

from ni_spec import constants as C
from ni_spec.loader import load_doc


def emit(packet_json: Path, spec_version: str) -> str:
    """Return SV package body (no provenance banner -- caller prepends it)."""
    spec = load_doc(packet_json)

    out: list[str] = []
    emitted: set[str] = set()  # dedup localparam names (SV LRM §6.20 forbids dup in same scope)

    def emit_param(line: str, name: str) -> None:
        """Append `line` only if `name` has not been emitted yet."""
        if name in emitted:
            return
        emitted.add(name)
        out.append(line)

    out.append("`ifndef NI_FLIT_PKG_SVH")
    out.append("`define NI_FLIT_PKG_SVH")
    out.append("")
    out.append("package ni_flit_pkg;")
    out.append("")

    out.append("  // --- top-level flit widths (from flit.derived) ---")
    emit_param(f"  localparam int unsigned FLIT_WIDTH        = {C.flit_width_resolved(spec)};", "FLIT_WIDTH")
    emit_param(f"  localparam int unsigned HEADER_WIDTH      = {C.header_width_resolved(spec)};", "HEADER_WIDTH")
    emit_param(f"  localparam int unsigned PAYLOAD_WIDTH     = {C.payload_width_resolved(spec)};", "PAYLOAD_WIDTH")
    emit_param(f"  localparam int unsigned LINK_WIDTH        = {C.link_width_resolved(spec)};", "LINK_WIDTH")
    for k, val in (
        ("FLIT_DATA_WIDTH",   C.flit_data_width_resolved(spec)),
        ("HEADER_DATA_WIDTH", C.header_data_width_resolved(spec)),
        ("WSTRB_WIDTH",       C.wstrb_width_resolved(spec)),
    ):
        emit_param(f"  localparam int unsigned {k:<15} = {val};", k)
    out.append("")

    out.append("  // --- header field bit positions (from flit.header_fields) ---")
    for f in spec["flit"]["header_fields"]:
        n = f["name"].upper()
        width = C.header_field_width(spec, f["name"])
        enabled_val = "1'b1" if C.header_field_enabled(spec, f["name"]) else "1'b0"
        if width == 0:
            # width=0 reserved placeholder: emit WIDTH=0 + ENABLED only; no LSB/MSB (field not bit-addressable)
            emit_param(f"  localparam int unsigned {n}_WIDTH   = 0;  // reserved placeholder (width=0 -- not in flit)", f"{n}_WIDTH")
            emit_param(f"  localparam bit          {n}_ENABLED = {enabled_val};", f"{n}_ENABLED")
        else:
            pos = C.header_field_position(spec, f["name"])
            emit_param(f"  localparam int unsigned {n}_LSB     = {pos[0]};", f"{n}_LSB")
            emit_param(f"  localparam int unsigned {n}_MSB     = {pos[1]};", f"{n}_MSB")
            emit_param(f"  localparam int unsigned {n}_WIDTH   = {width};", f"{n}_WIDTH")
            emit_param(f"  localparam bit          {n}_ENABLED = {enabled_val};", f"{n}_ENABLED")
    out.append("")

    out.append("  // --- payload widths per channel (from flit.payload_channels) ---")
    for ch in spec["flit"]["payload_channels"]:
        emit_param(f"  localparam int unsigned {ch['name']}_WIDTH = {C.payload_channel_width(spec, ch['name'])};", f"{ch['name']}_WIDTH")
    out.append("")

    out.append("  // --- all field widths (from flit.field_widths) ---")
    for name, val in spec["flit"].get("field_widths", {}).items():
        emit_param(f"  localparam int unsigned {name:<22} = {val};", name)
    out.append("")

    out.append("endpackage")
    out.append("")
    out.append("`endif // NI_FLIT_PKG_SVH")
    return "\n".join(out) + "\n"
