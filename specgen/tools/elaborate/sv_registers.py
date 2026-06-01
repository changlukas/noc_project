"""SV emitter for registers domain.

Produces rtl_pkg/ni_regs_pkg.sv.
Uses localparam int unsigned for offsets and field masks (design doc §6.2).
Consumes ni_spec.constants only -- no direct JSON parsing.
"""
from __future__ import annotations
from pathlib import Path
import re
import sys

SPEC_VALIDATE = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(SPEC_VALIDATE))

from ni_spec import constants as C
from ni_spec.loader import load_doc


def _to_identifier(name: str) -> str:
    """Convert register name to a safe SV identifier (upper-case, special chars removed)."""
    s = name.replace("(", "").replace(")", "").replace("`", "").strip()
    s = re.sub(r"[^A-Za-z0-9_]", "_", s)
    s = re.sub(r"_+", "_", s).strip("_")
    return s.upper()


def _emit_sv_per_reg_reset(spec) -> list[str]:
    """Emit per-register reset values as localparam int unsigned <REG>_RESET = 32'hN;.

    Reserved rows and rows with reset_expr == None are skipped.
    """
    out: list[str] = []
    out.append("  // --- per-register reset values ---")
    for r in spec.get("registers", []):
        if r.get("kind") == "reserved" or r.get("reset_expr") is None:
            continue
        name = r["name"].upper()
        rst = r["reset_expr"]
        try:
            int_val = int(rst, 0)
            rst_lit = f"32'h{int_val:X}"
        except (TypeError, ValueError):
            rst_lit = rst
        out.append(f"  localparam int unsigned {name}_RESET = {rst_lit};")
    out.append("")
    return out


def _emit_sv_access_mode(spec) -> list[str]:
    """Emit access_mode_e typedef + per-register localparam <REG>_ACCESS.

    First-time SV emission for access modes. WC is silently remapped to RW
    because no current register uses WC; the typedef can grow when needed.
    """
    out: list[str] = []
    out.append("  // --- access mode typedef + per-register localparam ---")
    out.append("  typedef enum logic [1:0] { ACCESS_RO, ACCESS_RW, ACCESS_RW1C, ACCESS_WO } access_mode_e;")
    out.append("")
    for r in spec.get("registers", []):
        if r.get("kind") == "reserved":
            continue
        name = r["name"].upper()
        access = r.get("access", "RW")
        if access == "WC":
            access = "RW"
        out.append(f"  localparam access_mode_e {name}_ACCESS = ACCESS_{access};")
    out.append("")
    return out


def _emit_sv_all_offsets(spec) -> list[str]:
    """Emit ALL_OFFSETS[N] array enumerating every non-reserved register offset."""
    out: list[str] = []
    out.append("  // --- ALL_OFFSETS array (excludes reserved rows) ---")
    offsets: list[str] = []
    for r in spec.get("registers", []):
        if r.get("kind") == "reserved":
            continue
        offsets.append(f"    {r['name'].upper()}_OFFSET")
    out.append(f"  localparam int unsigned ALL_OFFSETS [{len(offsets)}] = '{{")
    out.append(",\n".join(offsets))
    out.append("  };")
    out.append(f"  localparam int unsigned ALL_OFFSETS_COUNT = {len(offsets)};")
    out.append("")
    return out


def emit(registers_json: Path, spec_version: str) -> str:
    """Return SV package body (no provenance banner -- caller prepends it)."""
    spec = load_doc(registers_json)

    offsets = C.regs_offsets(spec)

    out: list[str] = []
    out.append("`ifndef NI_REGS_PKG_SVH")
    out.append("`define NI_REGS_PKG_SVH")
    out.append("")
    out.append("package ni_regs_pkg;")
    out.append("")

    # Offset constants
    out.append("  // --- register offsets ---")
    for reg_name, offset_int in offsets.items():
        ident = _to_identifier(reg_name)
        out.append(f"  localparam int unsigned {ident}_OFFSET = {hex(offset_int)};")
    out.append("")

    # Field masks
    out.append("  // --- field bit masks ---")
    has_fields = False
    for reg in spec.get("registers", []):
        if reg.get("kind") != "register":
            continue
        fields = reg.get("fields", [])
        if not fields:
            continue
        reg_ident = _to_identifier(reg["name"])
        for f in fields:
            field_ident = _to_identifier(f["name"])
            try:
                mask = C.regs_field_mask(spec, reg["name"], f["name"])
            except KeyError:
                continue
            out.append(f"  localparam int unsigned {reg_ident}_{field_ident}_MASK = {hex(mask)};")
            has_fields = True
    if not has_fields:
        out.append("  // (No field mask definitions in this spec.)")
    out.append("")

    # Access mode typedef + per-register localparam (Phase X.3, first-time SV).
    out.extend(_emit_sv_access_mode(spec))

    # Per-register reset values + ALL_OFFSETS array (Phase X.2).
    out.extend(_emit_sv_per_reg_reset(spec))
    out.extend(_emit_sv_all_offsets(spec))

    out.append("endpackage")
    out.append("")
    out.append("`endif // NI_REGS_PKG_SVH")
    return "\n".join(out) + "\n"
