"""C++ emitter for signals domain.

Emits two things:

1. ``ni::signals::*_RESET`` constants for every output pin that has a
   non-external_driven reset_behavior.
2. ``ni::pins::<Name>Pins`` bundle structs (one per ``interfaces[].name``)
   with a ``reset_outputs()`` method that drives every output pin from
   the matching ``ni::signals::*_RESET`` constant.

Consumes ni_spec.constants only -- no direct JSON parsing.
"""
from __future__ import annotations
from pathlib import Path
import sys

SPEC_VALIDATE = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(SPEC_VALIDATE))

from ni_spec import constants as C
from ni_spec.loader import load_doc


def _resolve_pin_width(signals_spec, packet_spec, iface_name: str, pin: dict) -> int:
    """Return the pin width as an int.

    PP-9: every AXI/CSR/NoC pin in the spec now resolves to an int through
    ``signal_pin_width`` (the AXI_*_WIDTH symbol gap is closed by per-port
    ``port_parameters``). No string fallback path remains; any
    ``ExprNameError`` here is a real spec bug and propagates to the caller.
    """
    return C.signal_pin_width(signals_spec, packet_spec,
                              iface_name, pin["pin_name"])


def _cpp_type_for_width(width: int) -> str:
    """Map a resolved int width to a C++ unsigned integer type."""
    w = int(width)
    if w <= 8:
        return "uint8_t"
    if w <= 16:
        return "uint16_t"
    if w <= 32:
        return "uint32_t"
    if w <= 64:
        return "uint64_t"
    # Wide signals (e.g. 402-bit flit): use byte array, no silent truncation.
    bytes_needed = (w + 7) // 8
    return f"std::array<uint8_t, {bytes_needed}>"


def _emit_pin_bundles(signals_spec, packet_spec) -> list[str]:
    """Emit ``ni::pins::*Pins`` structs from interfaces[].channels[].signals[]
    and interfaces[].signals[]."""
    out: list[str] = []
    out.append("namespace pins {")
    out.append("")
    grouped = C.signals_pins_by_interface(signals_spec)
    for iface_name, sigs in grouped.items():
        bundle = "".join(p.capitalize() for p in iface_name.split("_") if p) + "Pins"
        out.append(f"struct {bundle} {{")
        if not sigs:
            out.append("  // (no signals defined for this interface)")
        for s in sigs:
            width = _resolve_pin_width(signals_spec, packet_spec, iface_name, s)
            ctype = _cpp_type_for_width(width)
            out.append(f"  {ctype:<10s} {s['pin_name']};")
        out.append("")
        out.append("  void reset_outputs() {")
        emitted_any = False
        for s in sigs:
            if s["direction"] != "output":
                continue
            rb = s.get("reset_behavior") or {}
            if rb.get("kind") == "external_driven":
                continue
            # Detect wide-array type vs scalar -- for array types use ``= {}``
            # since the RESET constant is ``int 0`` and won't assign to an
            # array. For scalars, still use the named RESET constant for
            # traceability.
            width = _resolve_pin_width(signals_spec, packet_spec, iface_name, s)
            is_wide = int(width) > 64
            if is_wide:
                out.append(f"    {s['pin_name']} = {{}};  // wide signal, zero-initialized")
            else:
                const_name = s["pin_name"].upper() + "_RESET"
                # Constants live in ni::signals; we are in ni::pins. The
                # enclosing ``ni`` scope is visible, so ``signals::FOO``
                # resolves correctly.
                out.append(f"    {s['pin_name']} = signals::{const_name};")
            emitted_any = True
        if not emitted_any:
            out.append("    // (no output pins with reset values in this interface)")
        out.append("  }")
        out.append("};")
        out.append("")
    out.append("}  // namespace pins")
    return out


def emit(signals_json: Path, spec_version: str) -> str:
    """Return C++ header body (no provenance banner -- caller prepends it).

    Loads the sibling ``ni_packet.json`` to feed the cross-domain namespace
    used by ``C.signal_pin_width`` (FLIT_WIDTH, AXI_*_WIDTH, ...).
    """
    spec = load_doc(signals_json)
    packet_spec = load_doc(signals_json.parent / "ni_packet.json")

    # Collect all signals with non-null, non-external_driven reset_behavior.
    # These are the output signals that have a defined reset value.
    reset_consts: list[tuple[str, str]] = []
    for pin_name in C.signals_pin_names(spec):
        sig = C.signals_signal_by_pin(spec, pin_name)
        if sig is None:
            continue
        rb = sig.get("reset_behavior")
        if rb is None:
            continue
        if rb.get("kind") == "external_driven":
            continue
        value = rb.get("value", "0")
        reset_consts.append((pin_name.upper(), value))

    out: list[str] = []
    out.append("#pragma once")
    out.append("#include <array>")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace ni {")
    out.append("namespace signals {")
    out.append("")
    out.append("// Reset initializer constants for output signals.")
    out.append("// Input signals (external_driven) have no reset value defined here.")
    out.append("")
    if reset_consts:
        for const_name, value in reset_consts:
            out.append(f"constexpr int {const_name}_RESET = {value};")
    else:
        out.append("// (No output signals with defined reset values in this spec.)")
    out.append("")
    out.append("}  // namespace signals")
    out.append("")
    out.extend(_emit_pin_bundles(spec, packet_spec))
    out.append("}  // namespace ni")
    return "\n".join(out) + "\n"
