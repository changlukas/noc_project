"""registers.md → ni_registers.json (CSR register domain).

Parses the master register table, CSR access policy, and per-register
field-layout sub-tables. Reserved placeholder rows (em-dash access/reset)
are surfaced as kind="reserved" entries; section-header rows are skipped.
"""

from __future__ import annotations
import json
import re
import warnings
from pathlib import Path
from typing import Union

from ._common import (
    _parse_bit_range,
    _strip_cell,
    _section_slice,
    _extract_table,
    _col_idx,
    _split_table_row,
    _is_dash,
)


def parse_csr_policy(md_path: Path) -> dict:
    """HARDCODED: return the access policy dict for ni-spec v0.4.0.

    Does NOT parse md_path (kept in signature for API symmetry with other
    parse_* functions and to leave room for future MD-driven implementation).
    Values reflect registers.md §Access policy lines 5-8 (v0.4.0). If the spec
    updates the policy wording, update both this function and the schema enums.
    """
    # md_path intentionally unused; see docstring
    return {
        "sub_word_write": "slverr",
        "unmapped_read": "decerr",
        "misaligned": "slverr",
        "wo_read": "zero",
    }


def parse_register_map(md_path: Path) -> list:
    """Parse the master register table from registers.md.

    Columns: Offset | Register | Access | Reset | Description

    Row variants:
      1. Normal register: offset + backtick-name + access + reset + description
      2. Reserved placeholder: name like "(reserved for X)", access/reset are em-dashes
      3. Section header: | **Section Name** ||||| — skip these

    Returns list of dicts with keys: offset, name, kind, access, reset_expr, width_expr.
    """
    text = md_path.read_text(encoding="utf-8")

    # Find the "## Register map" section
    sec = _section_slice(text, r"^## Register map")
    if sec is None:
        raise ValueError("registers.md: '## Register map' section not found")

    rows = []
    # Parse only lines that start with a pipe and contain an offset (0x...)
    # We need to find the header row to know column indices, then parse data rows.
    lines = [l for l in sec.splitlines() if l.lstrip().startswith("|")]
    if len(lines) < 3:
        return rows

    # Skip header + separator
    for raw in lines[2:]:
        cells = _split_table_row(raw)
        if not cells or not cells[0]:
            continue

        offset_raw = cells[0].strip()

        # Skip rows that don't start with a hex offset — these are section headers
        # Section header rows look like: | **QoS Generator** ||||| with bold in first cell
        if not re.match(r"^0x[0-9A-Fa-f]+$", offset_raw):
            continue

        if len(cells) < 5:
            continue

        offset = offset_raw
        name_raw = cells[1].strip()
        access_raw = cells[2].strip()
        reset_raw = cells[3].strip()
        desc_raw = cells[4].strip() if len(cells) > 4 else ""

        # Strip backticks and bold markers from name
        name = name_raw.strip("`").strip("*").strip("`").strip("*").strip()

        # Detect reserved placeholder: name contains "reserved" and access/reset are dashes
        access_dash = _is_dash(access_raw)
        reset_dash = _is_dash(reset_raw)
        if access_dash and reset_dash:
            rows.append({
                "offset": offset,
                "name": name,
                "kind": "reserved",
                "access": None,
                "reset_expr": None,
            })
            continue
        if access_dash != reset_dash:
            # One-dash mismatch: previously demoted to reserved silently, which
            # would mask a typo. Warn and fall through to normal field parsing
            # so downstream validators see the real (broken) row.
            warnings.warn(
                f"register row {name!r} @offset={offset}: one-dash mismatch "
                f"(access={access_raw!r}, reset={reset_raw!r}); not demoting to reserved",
                stacklevel=2,
            )

        # Clean up access: strip inline comment suffixes like "0x0 (Bypass)"
        access = access_raw.strip()
        if access not in ("RO", "RW", "RW1C", "WO", "WC"):
            # Try to extract just the access token
            m = re.match(r"(RO|RW1C|RW|WO|WC)", access)
            access = m.group(1) if m else access

        # Reset: take just the first token (0x... value), strip trailing prose
        reset = reset_raw.strip()
        # Extract first hex or decimal token from reset cell
        rst_m = re.match(r"(0x[0-9A-Fa-f]+|\d+)", reset)
        if rst_m:
            reset = rst_m.group(1)
            # Normalize decimal to 0x hex
            if not reset.startswith("0x"):
                reset = hex(int(reset))

        entry = {
            "offset": offset,
            "name": name,
            "kind": "register",
            "access": access if access in ("RO", "RW", "RW1C", "WO", "WC") else None,
            "reset_expr": reset,
            "width_expr": "32",
        }
        rows.append(entry)

    return rows


def parse_register_fields(md_path: Path, reg_name: str) -> list:
    """Parse field layout table for a given register from registers.md.

    Looks for a section like '## §BASE_QOS Register (0x018) Field Layout' and
    parses the | Field | Bit | Width | Description | Reset | table.

    Returns list of field dicts: {name, bit_high, bit_low, access, reset, description}.
    Only non-Reserved fields are returned (Reserved rows are common but not spec-critical).
    """
    text = md_path.read_text(encoding="utf-8")

    # Try multiple section heading patterns for the register
    reg_clean = reg_name.strip("`")
    sec = _section_slice(text, rf"^##\s+§{re.escape(reg_clean)}\s+Register")
    if sec is None:
        # Some sections use just the name without §
        sec = _section_slice(text, rf"^##\s+{re.escape(reg_clean)}\s+Register")
    if sec is None:
        return []

    header, rows_raw = _extract_table(sec)
    if not header:
        return []

    i_field = _col_idx(header, "Field")
    i_bit = _col_idx(header, "Bit")
    i_desc = _col_idx(header, "Description")
    i_reset = _col_idx(header, "Reset")

    if i_field is None or i_bit is None:
        return []

    fields = []
    for cells in rows_raw:
        if i_field >= len(cells):
            continue
        name = _strip_cell(cells[i_field])
        if not name or name.lower() == "reserved":
            continue

        bit_str = _strip_cell(cells[i_bit]) if i_bit < len(cells) else ""
        # Parse bit range: "[3:0]" → hi=3, lo=0; "[0]" → hi=0, lo=0
        rng = _parse_bit_range(bit_str)
        if rng is None:
            continue
        lsb, msb = rng  # _parse_bit_range returns (lsb, msb)

        field_entry: dict = {
            "name": name,
            "bit_high": msb,
            "bit_low": lsb,
        }
        if i_reset is not None and i_reset < len(cells):
            rst = _strip_cell(cells[i_reset])
            if rst and rst != "—":
                field_entry["reset"] = rst
        if i_desc is not None and i_desc < len(cells):
            desc = _strip_cell(cells[i_desc])
            if desc:
                field_entry["description"] = desc
        fields.append(field_entry)

    return fields


# Register names that have field layout sections in registers.md
_REGISTERS_WITH_FIELDS = [
    "ERR_STATUS",
    "IRQ_ENABLE",
    "LAST_ERR_INFO",
    "PENDING_R_COUNT",
    "PENDING_W_COUNT",
    "QUIESCE_CTRL",
    "QUIESCE_STATUS",
    "EXCLUSIVE_MONITOR_CTRL",
    "EXCLUSIVE_MONITOR_STATUS",
]


def generate_ni_registers_json(md_dir: Union[str, Path], out_path: Union[str, Path]) -> dict:
    """Compose ni_registers.json from registers.md.

    Reads:
      - registers.md for register map + CSR policy
      - spec/ni/VERSION for spec_version

    Writes out_path and returns the dict.
    """
    md_dir_path = Path(md_dir) if not isinstance(md_dir, Path) else md_dir
    md_path = md_dir_path / "registers.md"
    if not md_path.exists():
        raise FileNotFoundError(f"registers.md not found at {md_path}")

    # Read spec_version from sibling VERSION file
    version_file = md_dir_path.parent / "VERSION"
    if not version_file.exists():
        raise FileNotFoundError(f"spec/ni/VERSION not found at {version_file}")
    spec_version = version_file.read_text(encoding="utf-8").strip()

    csr_policy = parse_csr_policy(md_path)
    registers = parse_register_map(md_path)

    # Attach field layout to registers that have documented fields
    for reg in registers:
        if reg["kind"] != "register":
            continue
        if reg["name"] in _REGISTERS_WITH_FIELDS:
            fields = parse_register_fields(md_path, reg["name"])
            if fields:
                reg["fields"] = fields
            else:
                warnings.warn(
                    f"parse_register_fields: register {reg['name']!r} expected fields but got "
                    f"empty list; field layout may use unsupported parametric notation",
                    stacklevel=2,
                )

    result = {
        "$schema_version": "ni-spec/2.0",
        "meta": {
            "spec_version": spec_version,
        },
        "csr_policy": csr_policy,
        "registers": registers,
    }

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return result
