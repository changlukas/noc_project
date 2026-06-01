"""C++ emitter for registers domain.

Emits offset macros, field bit masks, and access mode constants.
Consumes ni_spec.constants only -- no direct JSON parsing.
"""
from __future__ import annotations
from pathlib import Path
import sys

SPEC_VALIDATE = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(SPEC_VALIDATE))

from ni_spec import constants as C
from ni_spec.loader import load_doc


def _to_identifier(name: str) -> str:
    """Convert register name to a safe C++ identifier (upper-case, parens removed)."""
    # Strip surrounding parens and backticks that appear in reserved-row names.
    s = name.replace("(", "").replace(")", "").replace("`", "").strip()
    # Replace spaces and special chars with underscore.
    import re
    s = re.sub(r"[^A-Za-z0-9_]", "_", s)
    s = re.sub(r"_+", "_", s).strip("_")
    return s.upper()


def _emit_per_reg_reset(spec) -> list[str]:
    """Emit per-register reset values as constexpr uint32_t <REG>_RESET = N;.

    Reserved rows (kind == "reserved") and rows with reset_expr == None are skipped.
    """
    out: list[str] = []
    out.append("// --- per-register reset values ---")
    for r in spec.get("registers", []):
        if r.get("kind") == "reserved":
            continue
        if r.get("reset_expr") is None:
            continue
        name = r["name"].upper()
        rst = r["reset_expr"]
        try:
            int_val = int(rst, 0)
            rst_lit = f"0x{int_val:X}"
        except (TypeError, ValueError):
            rst_lit = rst
        out.append(f"constexpr uint32_t {name}_RESET = {rst_lit};")
    out.append("")
    return out


def _emit_all_offsets(spec) -> list[str]:
    """Emit ALL_OFFSETS[] array enumerating every non-reserved register offset."""
    out: list[str] = []
    out.append("// --- ALL_OFFSETS array (excludes reserved rows) ---")
    offsets: list[str] = []
    for r in spec.get("registers", []):
        if r.get("kind") == "reserved":
            continue
        offsets.append(f"  {r['name'].upper()}_OFFSET")
    out.append("constexpr uint32_t ALL_OFFSETS[] = {")
    out.append(",\n".join(offsets))
    out.append("};")
    out.append(f"constexpr std::size_t ALL_OFFSETS_COUNT = {len(offsets)};")
    out.append("")
    return out


def _emit_access_mode(spec) -> list[str]:
    """Emit single AccessMode enum class + per-register constexpr <REG>_ACCESS.

    Replaces the older shape (one single-value enum class per register). WC is
    silently remapped to RW because no current register uses WC; the enum can
    grow when a real consumer appears.
    """
    out: list[str] = []
    out.append("// --- access mode enum + per-register constexpr ---")
    out.append("enum class AccessMode { RO, RW, RW1C, WO };")
    out.append("")
    for r in spec.get("registers", []):
        if r.get("kind") == "reserved":
            continue
        name = r["name"].upper()
        access = r.get("access", "RW")
        if access == "WC":
            access = "RW"  # WC dropped from enum (no current consumer)
        out.append(f"constexpr AccessMode {name}_ACCESS = AccessMode::{access};")
    out.append("")
    return out


def _emit_csr_policy(spec) -> list[str]:
    """Emit csr_policy values as constexpr constants in ni::regs::csr_policy.

    For each key (sub_word_write, unmapped_read, misaligned, wo_read) emit:
      - constexpr const char* <KEY>           = "<value>";
      - constexpr int         <KEY>_IS_<VAL>  = 1;
    The integer sentinel lets c_model branch via `if constexpr (...)` without
    string comparison. Identifiers are not prefixed with CSR_POLICY_ because
    the enclosing namespace already provides that qualifier.
    """
    policy = spec.get("csr_policy", {})
    out: list[str] = []
    out.append("// --- csr_policy ---")
    out.append("namespace csr_policy {")
    for key in ("sub_word_write", "unmapped_read", "misaligned", "wo_read"):
        val = policy.get(key, "")
        enum_val = val.upper().replace("-", "_")
        out.append(f"constexpr const char* {key.upper()} = \"{val}\";")
        out.append(f"constexpr int         {key.upper()}_IS_{enum_val} = 1;")
    out.append("}  // namespace csr_policy")
    return out


def emit(registers_json: Path, spec_version: str) -> str:
    """Return C++ header body (no provenance banner -- caller prepends it)."""
    spec = load_doc(registers_json)

    offsets = C.regs_offsets(spec)

    out: list[str] = []
    out.append("#pragma once")
    out.append("#include <cstdint>")
    out.append("#include <cstddef>")
    out.append("")
    out.append("namespace ni {")
    out.append("namespace regs {")
    out.append("")

    # Offset constants
    out.append("// --- register offsets ---")
    for reg_name, offset_int in offsets.items():
        ident = _to_identifier(reg_name)
        out.append(f"constexpr int {ident}_OFFSET = {hex(offset_int)};")
    out.append("")

    # Field masks -- only for registers with fields
    out.append("// --- field bit masks ---")
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
            out.append(f"constexpr int {reg_ident}_{field_ident}_MASK = {hex(mask)};")
            has_fields = True
    if not has_fields:
        out.append("// (No field mask definitions in this spec.)")
    out.append("")

    # Access mode enum + per-register constexpr (single shared enum class).
    out.extend(_emit_access_mode(spec))

    # --- static_assert: per-register field width sum <= data_width (design doc sec 6.4) ---
    # Only registers with both fields and a parseable width_expr are checked.
    out.append("// --- static_assert: per-register field width sum <= data_width (design doc sec 6.4) ---")
    any_assert = False
    for reg in spec.get("registers", []):
        if reg.get("kind") != "register":
            continue
        fields = reg.get("fields", [])
        if not fields:
            continue
        width_expr = reg.get("width_expr")
        if width_expr is None:
            continue
        try:
            data_width = int(width_expr)
        except (ValueError, TypeError):
            continue
        # Sum field widths: high - low + 1 for each field.
        field_sum = 0
        sum_ok = True
        for f in fields:
            try:
                hi = int(f["bit_high"])
                lo = int(f["bit_low"])
                field_sum += hi - lo + 1
            except (KeyError, ValueError):
                sum_ok = False
                break
        if not sum_ok:
            continue
        reg_ident = _to_identifier(reg["name"])
        out.append(
            f"static_assert({field_sum} <= {data_width},"
            f" \"{reg['name']}: field width sum ({field_sum}) must be <= data_width ({data_width})\");"
        )
        any_assert = True
    if not any_assert:
        out.append("// (No per-register field width assertions applicable in this spec.)")
    out.append("")

    # Per-register reset values + ALL_OFFSETS array (Phase X.2).
    out.extend(_emit_per_reg_reset(spec))
    out.extend(_emit_all_offsets(spec))

    # csr_policy constants -- consumed by c_model RegisterFile to avoid hardcoding spec values.
    out.extend(_emit_csr_policy(spec))
    out.append("")

    out.append("}  // namespace regs")
    out.append("}  // namespace ni")
    return "\n".join(out) + "\n"
