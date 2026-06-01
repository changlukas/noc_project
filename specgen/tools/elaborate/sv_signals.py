"""SV emitter for signals domain.

Produces rtl_pkg/ni_signals_pkg.sv.
Uses localparam int unsigned for reset constants (design doc §6.2).
Also emits one SV ``interface`` block per ni_signals.json interface, outside
the package (SV interfaces cannot be declared inside packages).
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

    PP-9: every AXI/CSR/NoC pin resolves to an int through ``signal_pin_width``
    (the AXI_*_WIDTH symbol gap is closed by per-port ``port_parameters``).
    No symbolic-string fallback remains; an ``ExprNameError`` here is a
    real spec bug and propagates to the caller.
    """
    return C.signal_pin_width(signals_spec, packet_spec,
                              iface_name, pin["pin_name"])


def _sv_width_for(width: int) -> str:
    """Return SV bit-vector range ``[W-1:0] `` or empty string for 1-bit signals."""
    w = int(width)
    return "" if w == 1 else f"[{w - 1}:0] "


def _strip_dir_suffix(pin_name: str) -> str:
    """Strip literal ``_i``/``_o`` direction suffix from a pin name."""
    return pin_name[:-2] if pin_name.endswith(("_i", "_o")) else pin_name


def _emit_sv_interfaces(signals_spec, packet_spec) -> list[str]:
    """Emit ``interface ni_<name>_intf;`` blocks from interfaces[].

    SV interfaces cannot be declared inside packages, so callers MUST place
    these lines after ``endpackage``.
    """
    out: list[str] = []
    grouped = C.signals_pins_by_interface(signals_spec)
    for iface_name, sigs in grouped.items():
        iface_id = f"ni_{iface_name.lower()}_intf"
        out.append(f"interface {iface_id};")
        for s in sigs:
            width = _sv_width_for(
                _resolve_pin_width(signals_spec, packet_spec, iface_name, s)
            )
            sig_name = _strip_dir_suffix(s["pin_name"])
            out.append(f"  logic {width}{sig_name};")
        out.append("")
        in_sigs  = [s for s in sigs if s.get("direction") == "input"]
        out_sigs = [s for s in sigs if s.get("direction") == "output"]
        if in_sigs or out_sigs:
            out.append("  modport endpoint (")
            entries: list[str] = []
            if in_sigs:
                entries.append(
                    "    input " + ", ".join(_strip_dir_suffix(s["pin_name"]) for s in in_sigs)
                )
            if out_sigs:
                entries.append(
                    "    output " + ", ".join(_strip_dir_suffix(s["pin_name"]) for s in out_sigs)
                )
            out.append(",\n".join(entries))
            out.append("  );")
        out.append(f"endinterface : {iface_id}")
        out.append("")
    return out


def emit(signals_json: Path, spec_version: str) -> str:
    """Return SV package body (no provenance banner -- caller prepends it).

    Loads the sibling ``ni_packet.json`` to feed the cross-domain namespace
    used by ``C.signal_pin_width``.
    """
    spec = load_doc(signals_json)
    packet_spec = load_doc(signals_json.parent / "ni_packet.json")

    # Collect output signals with a defined (non-external_driven) reset value.
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
    out.append("`ifndef NI_SIGNALS_PKG_SVH")
    out.append("`define NI_SIGNALS_PKG_SVH")
    out.append("")
    out.append("package ni_signals_pkg;")
    out.append("")
    out.append("  // Reset initializer constants for output signals.")
    out.append("  // Input signals (external_driven) have no reset value defined here.")
    out.append("")
    if reset_consts:
        for const_name, value in reset_consts:
            out.append(f"  localparam int unsigned {const_name}_RESET = {value};")
    else:
        out.append("  // (No output signals with defined reset values in this spec.)")
    out.append("")
    out.append("endpackage")
    out.append("")
    # SV interface blocks live OUTSIDE the package (SV LRM: interface
    # declarations are top-level, not allowed inside packages).
    out.extend(_emit_sv_interfaces(spec, packet_spec))
    out.append("`endif // NI_SIGNALS_PKG_SVH")
    return "\n".join(out) + "\n"
