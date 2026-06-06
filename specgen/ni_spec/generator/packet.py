"""packet_format.md → ni_packet.json.

Parses §2.1 header bit allocation, §3.1-3.4 payload channels, and
§1.2 Group 1/2/3/5 field_widths + Group 6 derived. Outputs the symbolic
flit JSON (per-field width / lsb / msb are derived on demand by
ni_spec.constants helpers — JSON is name + width_param only).
"""

from __future__ import annotations
import json
import warnings
from pathlib import Path
from typing import Dict, List, Optional, Union

from ._common import (
    _parse_bit_range,
    _parse_int_cell,
    _section_slice,
    _extract_table,
    _col_idx,
)


# ---------- §2.1 header bit allocation ----------


def _is_zero_width_range(s: str) -> bool:
    """Return True if the Default Range cell signals a width=0 (reserved) field.

    Convention: a cell containing "(none" (case-insensitive) or "width=0" marks
    a 0-width reserved placeholder (e.g. "(none — width=0)").
    """
    sl = s.strip().lower()
    return sl.startswith("(none") or "width=0" in sl


def parse_header_fields(md_text: str) -> List[dict]:
    sec = _section_slice(md_text, r"^### 2\.1\s+.*Bit Allocation")
    if sec is None:
        raise ValueError("找不到 packet_format.md §2.1 Bit Allocation")
    header, rows = _extract_table(sec)
    i_name = _col_idx(header, "Field")
    i_wp = _col_idx(header, "Width Symbol")
    i_rng = _col_idx(header, "Default Range")
    i_stage = _col_idx(header, "Stage")
    i_enabled = _col_idx(header, "Enabled")
    if None in (i_name, i_wp, i_rng):
        raise ValueError(f"§2.1 缺欄位 (Field/Width Symbol/Default Range)；實際 header={header}")
    result = []
    for cells in rows:
        if max(i_name, i_wp, i_rng) >= len(cells):
            warnings.warn(
                f"parse_header_fields: skipping row with too few columns: {cells!r}",
                stacklevel=2,
            )
            continue
        name = cells[i_name]
        if not name:
            warnings.warn(
                f"parse_header_fields: skipping row with empty Field cell: {cells!r}",
                stacklevel=2,
            )
            continue
        rng_cell = cells[i_rng]

        # Parse enabled column: "true" → True, "false" → False, missing → True (default)
        enabled: bool = True
        if i_enabled is not None and i_enabled < len(cells):
            raw_enabled = cells[i_enabled].strip().lower()
            if raw_enabled == "false":
                enabled = False
            # any other value (including empty string) defaults to True

        # Handle width=0 reserved placeholder fields (e.g. noc_qos when NOC_QOS_WIDTH=0)
        if _is_zero_width_range(rng_cell):
            field: dict = {
                "name": name,
                "width_param": cells[i_wp],
            }
            field["enabled"] = enabled
            result.append(field)
            continue

        rng = _parse_bit_range(rng_cell)
        if rng is None:
            warnings.warn(
                f"parse_header_fields: skipping row {name!r} with unparseable "
                f"Default Range {rng_cell!r}",
                stacklevel=2,
            )
            continue
        # Bit range parsed only to validate the row; positions are computed
        # on-the-fly by constants.header_field_position. Per PP-6 the JSON is
        # purely symbolic — width/lsb/msb are derived, not authored.
        field = {
            "name": name,
            "width_param": cells[i_wp],
        }
        field["enabled"] = enabled
        result.append(field)
    return result


# ---------- §3 payload channels ----------


_CHANNEL_TO_NETWORK = {"AW": "REQ", "W": "REQ", "AR": "REQ", "B": "RSP", "R": "RSP"}


def _parse_payload_section(md_text: str, section_re: str, channel_name: str) -> Optional[dict]:
    sec = _section_slice(md_text, section_re)
    if sec is None:
        return None
    header, rows = _extract_table(sec)
    i_name = _col_idx(header, "Field")
    i_wp = _col_idx(header, "Width Symbol")
    i_rng = _col_idx(header, "Default Range")
    if None in (i_name, i_wp, i_rng):
        return None
    fields = []
    max_msb = -1
    for cells in rows:
        if max(i_name, i_wp, i_rng) >= len(cells):
            continue
        name = cells[i_name]
        rng = _parse_bit_range(cells[i_rng])
        if not name or rng is None:
            continue
        lsb, msb = rng
        if msb > max_msb:
            max_msb = msb
        wp = cells[i_wp]
        # "derived (3)" → 規一化為 "derived"，width 由 channel.payload_width
        # 與其他 field 之和推得（payload_field_width helper 處理）
        if wp.startswith("derived"):
            wp = "derived"
        # PP-6: per-field width/lsb/msb are derived. JSON keeps only the
        # symbolic (name, width_param) pair; helpers compute positions.
        fields.append({
            "name": name,
            "width_param": wp,
        })
    if not fields:
        return None
    # Per-channel payload_width stays in JSON — it is authored metadata
    # (set by the spec author per AXI channel), not a derived quantity.
    payload_width = max_msb + 1
    return {
        "name": channel_name,
        "network": _CHANNEL_TO_NETWORK[channel_name],
        "payload_width": payload_width,
        "fields": fields,
    }


def parse_payload_channels(md_text: str) -> List[dict]:
    """§3.1 (AW + 從 AW 衍生 AR) + §3.2 W + §3.3 B + §3.4 R。"""
    channels = []

    aw = _parse_payload_section(md_text, r"^### 3\.1\s+AW/AR Channel Payload", "AW")
    if aw:
        channels.append(aw)
        # §3.1 只列 aw* fields，AR 結構相同（spec 寫死 prefix swap）
        ar = {
            "name": "AR",
            "network": "REQ",
            "payload_width": aw["payload_width"],
            "fields": [
                {**f, "name": f["name"].replace("aw", "ar", 1) if f["name"].startswith("aw") else f["name"]}
                for f in aw["fields"]
            ],
        }
        channels.append(ar)

    for name, sec_re in (
        ("W", r"^### 3\.2\s+W Channel Payload"),
        ("B", r"^### 3\.3\s+B Channel Payload"),
        ("R", r"^### 3\.4\s+R Channel Payload"),
    ):
        ch = _parse_payload_section(md_text, sec_re, name)
        if ch:
            channels.append(ch)

    return channels


# ---------- §1.2 field_widths (Group 1-5) ----------


_FIELD_WIDTHS_GROUPS = (
    r"^#### Group 1 — Topology",
    r"^#### Group 2 — Header Fields",
    r"^#### Group 3 — AXI Payload Sub-Fields",
    # Group 4 (ECC) 已 retired，跳過
    r"^#### Group 5 — B Channel Reserved",
)


def parse_field_widths(md_text: str) -> Dict[str, int]:
    widths: Dict[str, int] = {}
    for group_re in _FIELD_WIDTHS_GROUPS:
        sec = _section_slice(md_text, group_re)
        if sec is None:
            continue
        header, rows = _extract_table(sec)
        i_name = _col_idx(header, "Parameter")
        i_default = _col_idx(header, "Default")
        if None in (i_name, i_default):
            continue
        for cells in rows:
            if max(i_name, i_default) >= len(cells):
                continue
            name = cells[i_name]
            val = _parse_int_cell(cells[i_default])
            if name and val is not None:
                widths[name] = val
    return widths


# ---------- §1.2 derived (Group 6) ----------


# 跳過 per-channel payload width — 那些屬於 payload_channels[].payload_width
_DERIVED_SKIP = {"AW_PAYLOAD_WIDTH", "W_PAYLOAD_WIDTH", "AR_PAYLOAD_WIDTH",
                 "B_PAYLOAD_WIDTH", "R_PAYLOAD_WIDTH"}


def parse_derived(md_text: str) -> Dict[str, int]:
    derived: Dict[str, int] = {}
    sec = _section_slice(md_text, r"^#### Group 6 — Composite / Derived")
    if sec is None:
        return derived
    header, rows = _extract_table(sec)
    i_name = _col_idx(header, "Parameter")
    i_default = _col_idx(header, "Default")
    if None in (i_name, i_default):
        return derived
    for cells in rows:
        if max(i_name, i_default) >= len(cells):
            continue
        name = cells[i_name]
        if name in _DERIVED_SKIP:
            continue
        val = _parse_int_cell(cells[i_default])
        if name and val is not None:
            derived[name] = val
    return derived


# ---------- 組裝 ----------


def generate_ni_packet_json(md_dir: Union[str, Path]) -> dict:
    """讀 packet_format.md 產出完整 ni_packet.json 結構。"""
    md_path = Path(md_dir) / "packet_format.md"
    if not md_path.exists():
        raise FileNotFoundError(f"找不到 {md_path}")
    md_text = md_path.read_text(encoding="utf-8")

    # PP-6: flit.derived is dropped from JSON. Resolved totals (HEADER_WIDTH,
    # FLIT_WIDTH, LINK_WIDTH, etc.) are computed on demand by
    # ni_spec.constants.*_resolved helpers — JSON is purely symbolic.
    # parse_derived(md_text) is still used by signal namespace builder
    # (_build_params_namespace) to seed AXI parameter widths.
    #
    # Exception: WSTRB_WIDTH is a derived leaf (NOC_DATA_WIDTH/8) directly
    # referenced as width_param by W-channel fields. Promote it into
    # field_widths so packet_eval_expr resolves it in one namespace.
    field_widths = parse_field_widths(md_text)
    derived = parse_derived(md_text)
    if "WSTRB_WIDTH" in derived:
        field_widths["WSTRB_WIDTH"] = derived["WSTRB_WIDTH"]
    return {
        "$schema_version": "ni-spec/2.0",
        "meta": {
            "block": "Network Interface (NI: NMU + NSU)",
            "spec_version": "v0.4.0",
        },
        "flit": {
            "field_widths": field_widths,
            "header_fields": parse_header_fields(md_text),
            "payload_channels": parse_payload_channels(md_text),
            "route_par_coverage": ["dst_id", "last"],
        },
    }


def write_generated_json(md_dir: Union[str, Path], out_path: Union[str, Path]) -> dict:
    """生成 + 寫檔。回傳 dict 給 caller。"""
    data = generate_ni_packet_json(md_dir)
    p = Path(out_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return data
