"""MD → JSON generator. Path B 的核心：把 packet_format.md 解析成完整 ni_packet.json。

對齊業界做法（SystemRDL / Protocol Buffers）：人類只改 source（MD），
工具產衍生品（JSON），下游消費衍生品。本檔取代原 crosscheck.py 的角色。
"""

from __future__ import annotations
import json
import re
import warnings
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Union


# ---------- 共用 helper ----------

_RANGE_RE = re.compile(r"\[(\d+)(?::(\d+))?\]")
# 整數解析容忍 backtick：`8` → 8
_INT_TOKEN_RE = re.compile(r"-?\d+")


def _parse_bit_range(s: str) -> Optional[Tuple[int, int]]:
    """`[3:0]` → (0, 3)；`[26]` → (26, 26)；失敗 None。回 (lsb, msb)。"""
    m = _RANGE_RE.search(s)
    if not m:
        return None
    msb = int(m.group(1))
    lsb = int(m.group(2)) if m.group(2) is not None else msb
    return (lsb, msb)


def _strip_cell(c: str) -> str:
    """剝掉 markdown 強調符號（`、*）跟空白。"""
    return c.strip().strip("`").strip("*").strip("`").strip()


def _parse_int_cell(s: str) -> Optional[int]:
    """從 cell 字串裡抓第一個整數（容忍 backtick / 描述文字）。"""
    s = _strip_cell(s)
    m = _INT_TOKEN_RE.search(s)
    return int(m.group(0)) if m else None


def _section_slice(md_text: str, heading_pattern: str) -> Optional[str]:
    """切出 heading 後到下一個同層或淺層 heading 之前的文字。找不到 heading 回 None。

    自動偵測 heading 層級——例如 `### Foo` (level 3) 結束於下一個 level ≤ 3 的 heading
    (i.e. `###` / `##` / `#`)，**不**結束於更深的 `####` (level 4)。這讓 sub-section
    parser 可以放心抓母 section 完整內容。
    """
    pat = re.compile(heading_pattern, re.MULTILINE)
    m = pat.search(md_text)
    if not m:
        return None
    # 計算這個 matched heading 的 # 個數
    matched = m.group(0).lstrip()
    level = 0
    for ch in matched:
        if ch == "#":
            level += 1
        else:
            break
    if level == 0:
        level = 6  # fallback：不限制深度
    after = md_text[m.end():]
    next_re = re.compile(rf"^#{{1,{level}}}\s", re.MULTILINE)
    nxt = next_re.search(after)
    return after[: nxt.start()] if nxt else after


def _extract_table(section_text: str) -> Tuple[List[str], List[List[str]]]:
    """從 section 文字裡抓第一張 markdown table。回 (header_cells, [data_rows])。

    自動跳過分隔列 (|---|---|) 跟空 first-column 的 summary 列。
    支援 escaped pipe (\\|) — 不會被誤切。
    """
    lines = [l for l in section_text.splitlines() if l.lstrip().startswith("|")]
    if len(lines) < 3:
        return [], []
    header = _split_table_row(lines[0])
    rows: List[List[str]] = []
    for raw in lines[2:]:
        cells = _split_table_row(raw)
        if not cells or not cells[0]:
            continue  # summary 列
        rows.append(cells)
    return header, rows


def _col_idx(header: List[str], name: str) -> Optional[int]:
    try:
        return header.index(name)
    except ValueError:
        return None


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
            continue
        name = cells[i_name]
        if not name:
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


# ════════════════════════════════════════════════════════════════════
# signal_interface.md — Phase 2 (Path B), 簡化版：只抓 top-level interface + parameters
# ════════════════════════════════════════════════════════════════════
#
# 之前版本曾 parse 134 根 wire，使用者裁定太複雜。化繁為簡：
#   - parameters: 從 §Parameters 抓
#   - interfaces: 從 §Per-block interface summary 的 NMU/NSU table 抓
#   - 其他 per-wire 細節留在 signal_interface.md 給人讀，不機器化


_ESC_PIPE = "\x00ESC_PIPE\x00"


def _split_table_row(raw: str) -> List[str]:
    """安全切 markdown table 列：先用 placeholder 保護 escaped pipe (\\|)，split 後還原。"""
    protected = raw.replace(r"\|", _ESC_PIPE)
    cells = [_strip_cell(c).replace(_ESC_PIPE, "|") for c in protected.strip().strip("|").split("|")]
    return cells


def _extract_all_tables(section_text: str) -> List[Tuple[List[str], List[List[str]]]]:
    """Section 內所有 markdown table（不只第一張）。回 [(header, rows), ...]。"""
    tables = []
    lines = section_text.splitlines()
    i = 0
    while i < len(lines):
        if not lines[i].lstrip().startswith("|"):
            i += 1
            continue
        block = []
        while i < len(lines) and lines[i].lstrip().startswith("|"):
            block.append(lines[i])
            i += 1
        if len(block) >= 3:
            header = _split_table_row(block[0])
            rows = []
            for raw in block[2:]:
                cells = _split_table_row(raw)
                if not cells or not cells[0]:
                    continue
                rows.append(cells)
            tables.append((header, rows))
    return tables


def parse_signal_parameters(md_text: str) -> List[dict]:
    """§Parameters table。"""
    sec = _section_slice(md_text, r"^## Parameters")
    if sec is None:
        return []
    tables = _extract_all_tables(sec)
    if not tables:
        return []
    header, rows = tables[0]
    i_name = _col_idx(header, "Name")
    i_type = _col_idx(header, "Type")
    i_default = _col_idx(header, "Default")
    i_constraint = _col_idx(header, "Constraint")
    i_desc = _col_idx(header, "Description")
    params = []
    for cells in rows:
        if i_name is None or i_name >= len(cells):
            continue
        name = _strip_cell(cells[i_name])
        if not name:
            continue
        p = {"name": name}
        if i_type is not None and i_type < len(cells):
            p["type"] = _strip_cell(cells[i_type])
        if i_default is not None and i_default < len(cells):
            p["default"] = _strip_cell(cells[i_default])
        if i_constraint is not None and i_constraint < len(cells):
            p["constraint_text"] = _strip_cell(cells[i_constraint])
        if i_desc is not None and i_desc < len(cells):
            p["description"] = _strip_cell(cells[i_desc])
        params.append(p)
    return params


# AXI4 / AXI4-Lite 5-channel direction map（per ARM IHI 0022）
# 從 host port 視角：slave port 看 AW/W/AR 是 input，B/R 是 output；master 反過來
_AXI_CHANNELS_SLAVE = [
    ("AW", "input",  "request"),
    ("W",  "input",  "request"),
    ("AR", "input",  "request"),
    ("B",  "output", "response"),
    ("R",  "output", "response"),
]
_AXI_CHANNELS_MASTER = [
    (n, ("output" if d == "input" else "input"), c)
    for n, d, c in _AXI_CHANNELS_SLAVE
]


def _port_type_of(entry: dict) -> Optional[str]:
    """slave / master / None。

    Uses UPPER_SNAKE_CASE name (e.g. AXI_SLAVE_PORT, AXI_MASTER_PORT, CSR).
    AXI4-Lite CSR is treated as slave (same channel directions as AXI slave).
    """
    proto = entry.get("protocol", "")
    name = entry.get("name", "").upper()
    if proto not in ("AXI4", "AXI4-Lite"):
        return None
    if "SLAVE" in name or proto == "AXI4-Lite":
        return "slave"
    if "MASTER" in name:
        return "master"
    return None


def _build_axi_channels(port_type: str, ns: Dict[str, dict],
                         with_signals: bool) -> List[dict]:
    """組 AXI 5 channel list。with_signals=True 時每 channel 帶 signals[] (展 per-signal)。

    port_type: 'slave' 或 'master'。AXI_ID_WIDTH 在所有 port 共用；per-port 預設值
    在 interface port_parameters[] 裡，不在 signal entry 裡。

    PP-9: pins are purely symbolic — only ``width_param`` is stored. Default
    values come from the interface's ``port_parameters`` (resolver namespace),
    not from per-pin storage.
    """
    template = _AXI_CHANNELS_SLAVE if port_type == "slave" else _AXI_CHANNELS_MASTER
    channels = []
    for ch_name, direction, carrier in template:
        ch = {"name": ch_name, "direction": direction, "carrier": carrier}
        if with_signals:
            ch["signals"] = []
            for sig_suffix, src_param in _AXI_CHANNEL_SIGNALS[ch_name]:
                # _sig_suffix is a build-time helper consumed by _derive_pin_name;
                # it is stripped by write_generated_signals_json before writing.
                sig_entry = {"width_param": src_param, "_sig_suffix": sig_suffix}
                ch["signals"].append(sig_entry)
        channels.append(ch)
    return channels


# Per-channel signal listing — each AXI channel 自己 carry 哪些 signal、各 signal 寬度
# 來自哪個 parameter。signal 名稱用 channel 前綴（AW_ADDR / AR_ADDR）讓 user 一眼分辨。
#
# Signal naming: channel-suffix。Width source param 來自 packet_format.md §1.2 Group 3
# 或 signal_interface.md §Parameters（ID 寬度依 slave/master 不同）。
# 不列 valid/ready (1-bit fixed handshake)。

_AXI_CHANNEL_SIGNALS = {
    # Per AMBA AXI4 IHI 0022; USER signals are per-channel per spec.
    # src_param must exist in the params namespace built by _build_params_namespace.
    "AW": [
        ("ID",     "AXI_ID_WIDTH"),
        ("ADDR",   "AXI_ADDR_WIDTH"),
        ("LEN",    "AXI_LEN_WIDTH"),
        ("SIZE",   "AXI_SIZE_WIDTH"),
        ("BURST",  "AXI_BURST_WIDTH"),
        ("CACHE",  "AXI_CACHE_WIDTH"),
        ("LOCK",   "AXI_LOCK_WIDTH"),
        ("PROT",   "AXI_PROT_WIDTH"),
        ("REGION", "AXI_REGION_WIDTH"),
        ("USER",   "AXI_AWUSER_WIDTH"),  # per-channel per AMBA
        ("QOS",    "AXI_QOS_WIDTH"),
    ],
    "W": [
        ("DATA",   "AXI_DATA_WIDTH"),
        ("STRB",   "AXI_STRB_WIDTH"),
        ("LAST",   "AXI_LAST_WIDTH"),
        ("USER",   "AXI_WUSER_WIDTH"),   # per-channel per AMBA
    ],
    "B": [
        ("ID",     "AXI_ID_WIDTH"),
        ("RESP",   "AXI_RESP_WIDTH"),
        ("USER",   "AXI_BUSER_WIDTH"),   # per-channel per AMBA
    ],
    "AR": [
        ("ID",     "AXI_ID_WIDTH"),
        ("ADDR",   "AXI_ADDR_WIDTH"),
        ("LEN",    "AXI_LEN_WIDTH"),
        ("SIZE",   "AXI_SIZE_WIDTH"),
        ("BURST",  "AXI_BURST_WIDTH"),
        ("CACHE",  "AXI_CACHE_WIDTH"),
        ("LOCK",   "AXI_LOCK_WIDTH"),
        ("PROT",   "AXI_PROT_WIDTH"),
        ("REGION", "AXI_REGION_WIDTH"),
        ("USER",   "AXI_ARUSER_WIDTH"),  # per-channel per AMBA
        ("QOS",    "AXI_QOS_WIDTH"),
    ],
    "R": [
        ("ID",     "AXI_ID_WIDTH"),
        ("DATA",   "AXI_DATA_WIDTH"),
        ("RESP",   "AXI_RESP_WIDTH"),
        ("LAST",   "AXI_LAST_WIDTH"),
        ("USER",   "AXI_RUSER_WIDTH"),   # per-channel per AMBA
    ],
}


# 每個 NoC link interface 帶的 signals（per-signal name + width_param + direction）
# 跟 AXI 同樣風格 — 看 JSON 就知道每根 wire 名字/寬度/方向。
# Credit return 是反向走（_o 側 interface 收 _i credit；_i 側 interface 送 _o credit）。
# Internal NoC 使用 static credit preload at reset，不需要 dynamic credit-init handshake。
_NOC_INTERFACE_SIGNALS = {
    # Keys are UPPER_SNAKE_CASE (post Task 3.5 canonical names).
    "NOC_REQ_OUT": [
        ("req_valid_o",  "1",          "output"),
        ("req_flit_o",   "FLIT_WIDTH", "output"),
        ("req_credit_i", "NUM_VC",     "input"),
    ],
    "NOC_RSP_IN": [
        ("rsp_valid_i",  "1",          "input"),
        ("rsp_flit_i",   "FLIT_WIDTH", "input"),
        ("rsp_credit_o", "NUM_VC",     "output"),
    ],
    "NOC_REQ_IN": [
        ("req_valid_i",  "1",          "input"),
        ("req_flit_i",   "FLIT_WIDTH", "input"),
        ("req_credit_o", "NUM_VC",     "output"),
    ],
    "NOC_RSP_OUT": [
        ("rsp_valid_o",  "1",          "output"),
        ("rsp_flit_o",   "FLIT_WIDTH", "output"),
        ("rsp_credit_i", "NUM_VC",     "input"),
    ],
}

# NoC port-level parameters (knobs that configure the link)
_INTERFACE_PARAMS = {
    "NOC_REQ_OUT":  ["NUM_VC", "FLIT_WIDTH"],
    "NOC_RSP_IN":   ["NUM_VC", "FLIT_WIDTH"],
    "NOC_REQ_IN":   ["NUM_VC", "FLIT_WIDTH"],
    "NOC_RSP_OUT":  ["NUM_VC", "FLIT_WIDTH"],
    "CSR":          [],  # AXI4-Lite，width 寫死 32/12
}

# AXI port 層級 parameter（不屬任一 channel，是整個 port 的 toggle 或 width 設定）。
# PP-9: closes the AXI_*_WIDTH symbolic gap — the resolver now finds these widths
# in interface port_parameters instead of relying on per-pin stored defaults.
# Width parameters follow AXI4 convention (AMBA IHI 0022): each width is an
# instance-level knob on the AXI port.
_AXI_PORT_PARAMS = [
    "ENABLE_AXI_PARITY",
    # AXI scalar widths (AMBA IHI 0022 §A1)
    "AXI_DATA_WIDTH",
    "AXI_STRB_WIDTH",
    "AXI_QOS_WIDTH",
    "AXI_AWUSER_WIDTH",
    "AXI_WUSER_WIDTH",
    "AXI_BUSER_WIDTH",
    "AXI_ARUSER_WIDTH",
    "AXI_RUSER_WIDTH",
]


def _build_noc_signals(interface_name: str, ns: Dict[str, dict]) -> List[dict]:
    """組 NoC link interface 的 signals list。

    pin_name 在 write_generated_signals_json 後處理時填入（透過 _derive_pin_name）。
    width_param: "1" 是 literal width，不是 parameter name → ``width_expr`` 存原值，
    ``width_param`` 設為 None（讓 resolver 從 width_expr 拿到 literal int）。

    PP-9: pins are purely symbolic — no ``default`` is stored. Width resolution
    is handled by ``signal_pin_width`` via the merged namespace (packet domain
    + interface port_parameters).
    """
    sigs = _NOC_INTERFACE_SIGNALS.get(interface_name, [])
    out = []
    for sig_name, raw_width_param, direction in sigs:
        # Determine whether raw_width_param is a real parameter reference or a literal.
        # Numeric strings (e.g. "1") are literal widths — width_param should be null.
        try:
            int(raw_width_param)
            width_param_val: Optional[str] = None
            width_expr_val: Optional[str] = raw_width_param
        except ValueError:
            width_param_val = raw_width_param
            width_expr_val = None

        entry = {
            "width_param": width_param_val,
            "direction": direction,
        }
        if width_expr_val is not None:
            entry["width_expr"] = width_expr_val
        # _sig_name is a build-time helper consumed by _derive_pin_name for NoC signals;
        # it mirrors the logical signal name (e.g. "req_valid_o") used to form noc_<name>.
        entry["_sig_name"] = sig_name
        out.append(entry)
    return out

# Block-level presence toggle（不屬於任一 interface 而是 block 本身）
_BLOCK_ENABLE_PARAMS = ["EN_MST_PORT", "EN_SLV_PORT"]

# Interface name normalisation: MD prose string → UPPER_SNAKE_CASE identifier.
_IFACE_NAME_MAP: Dict[str, str] = {
    "AXI slave port (host)":  "AXI_SLAVE_PORT",
    "AXI master port (host)": "AXI_MASTER_PORT",
    "NoC request out":        "NOC_REQ_OUT",
    "NoC response in":        "NOC_RSP_IN",
    "NoC request in":         "NOC_REQ_IN",
    "NoC response out":       "NOC_RSP_OUT",
    "CSR":                    "CSR",
}

# Protocol normalization: MD prose → clean identifier (matches AXI4 style).
# AXI protocol values ("AXI4", "AXI4-Lite") are already clean — no mapping needed.
_PROTOCOL_MAP: Dict[str, str] = {
    "NoC request flit link":  "NoC",
    "NoC response flit link": "NoC",
}

def _build_params_namespace(md_dir: Union[str, Path],
                             signal_params: List[dict]) -> Dict[str, dict]:
    """聯集 signal_interface.md §Parameters + packet_format.md §1.2 field_widths/derived。

    signal_interface 提供 type / constraint / description；
    packet_format 只提供 default value（field width 是 fixed-by-AXI4-spec）。

    在聯集後，加入 AMBA-canonical 別名（packet_format / signal_interface 用舊名；
    generator 輸出用新名，保持 JSON 與 AMBA IHI 0022 對齊）：
      ADDR_WIDTH        → AXI_ADDR_WIDTH
      AXI_QOS_WIDTH     = 4 (fixed per AMBA IHI 0022; NOT from NOC_QOS_WIDTH which is 0)
      NOC_DATA_WIDTH    → AXI_DATA_WIDTH   (for AXI W/R channel signals)
      WSTRB_WIDTH       → AXI_STRB_WIDTH
      USER_WIDTH        → AXI_AWUSER_WIDTH / AXI_WUSER_WIDTH / AXI_BUSER_WIDTH /
                          AXI_ARUSER_WIDTH / AXI_RUSER_WIDTH  (per-channel per AMBA)
      AXI_ID_WIDTH is already in packet_format.md Group 3 — no alias needed.
    """
    by_name = {p["name"]: p for p in signal_params}

    # 從 packet_format.md 補 AXI signal width / WSTRB_WIDTH 等
    packet_md = (Path(md_dir) / "packet_format.md").read_text(encoding="utf-8")
    fw = parse_field_widths(packet_md)
    derived = parse_derived(packet_md)
    for name, val in {**fw, **derived}.items():
        if name not in by_name:
            by_name[name] = {
                "name": name,
                "type": "int",
                "default": str(val),
                "source": "packet_format.md §1.2",
            }

    # Inject AMBA-canonical aliases for names used in _AXI_CHANNEL_SIGNALS.
    # Source of truth for the default value is the old (pre-alias) entry.
    # NOTE: AXI_QOS_WIDTH is NOT aliased from NOC_QOS_WIDTH.  NOC_QOS_WIDTH is the NoC-header
    # field width (currently 0 — reserved placeholder).  AXI QoS signals (awqos/arqos) are
    # fixed at 4 bits per AMBA IHI 0022, independent of the NoC-layer QoS width.
    _ALIASES: List[Tuple[str, str]] = [
        ("ADDR_WIDTH",    "AXI_ADDR_WIDTH"),
        # NOC_QOS_WIDTH removed — see note above
        ("NOC_DATA_WIDTH","AXI_DATA_WIDTH"),
        ("WSTRB_WIDTH",   "AXI_STRB_WIDTH"),
        ("USER_WIDTH",    "AXI_AWUSER_WIDTH"),
        ("USER_WIDTH",    "AXI_WUSER_WIDTH"),
        ("USER_WIDTH",    "AXI_BUSER_WIDTH"),
        ("USER_WIDTH",    "AXI_ARUSER_WIDTH"),
        ("USER_WIDTH",    "AXI_RUSER_WIDTH"),
    ]
    for src_name, alias in _ALIASES:
        if alias not in by_name and src_name in by_name:
            src = by_name[src_name]
            by_name[alias] = {k: src[k] for k in src if k != "name"} | {"name": alias}

    # Inject AXI_QOS_WIDTH as a fixed AMBA constant (4 bits per IHI 0022).
    # This is independent of NOC_QOS_WIDTH (the NoC-layer placeholder, currently 0).
    if "AXI_QOS_WIDTH" not in by_name:
        by_name["AXI_QOS_WIDTH"] = {
            "name": "AXI_QOS_WIDTH",
            "type": "int",
            "default": "4",
            "source": "AMBA IHI 0022 fixed (independent of NOC_QOS_WIDTH)",
        }

    return by_name


def _select_params(ns: Dict[str, dict], names: List[str]) -> List[dict]:
    out = []
    for n in names:
        p = ns.get(n)
        if p is None:
            continue
        out.append({k: p[k] for k in ("name", "type", "default", "constraint_text", "source") if k in p})
    return out


def parse_top_level_interfaces(md_text: str) -> List[dict]:
    """§Per-block interface summary — NMU + NSU 各一張 table。

    AXI / AXI4-Lite interface 會展成 channels[] sub-list 列出每 channel 真實方向
    （AW/W/AR 跟 B/R 對 slave 是反向）。NoC link 本來單向，無 channels。

    Interface entry shape (Task 3.5 clean shape):
      name        — UPPER_SNAKE_CASE identifier (from _IFACE_NAME_MAP)
      block       — NMU | NSU
      direction   — as-parsed from MD table
      protocol    — as-parsed from MD table
      clock       — split from clock_domain (before " / ")
      reset       — split from clock_domain (after " / ")
    Removed: peer, wire_source, clock_domain, description, flow_control.
    """
    sec = _section_slice(md_text, r"^## Per-block interface summary")
    if sec is None:
        return []
    interfaces = []
    for sub_re, block in (
        (r"^### NMU\b",  "NMU"),
        (r"^### NSU\b",  "NSU"),
    ):
        sub = _section_slice(sec, sub_re)
        if sub is None:
            continue
        for header, rows in _extract_all_tables(sub):
            i_name = _col_idx(header, "Interface")
            i_dir = _col_idx(header, "Direction")
            i_proto = _col_idx(header, "Protocol")
            i_clk = _col_idx(header, "Clock domain")
            if i_name is None:
                continue
            for cells in rows:
                if i_name >= len(cells):
                    continue
                clean = [c.replace("`", "") for c in cells]
                raw_name = clean[i_name]
                clean_name = _IFACE_NAME_MAP.get(raw_name, raw_name)

                # Split clock_domain "aclk_i / arst_ni" → clock + reset
                clock_val: Optional[str] = None
                reset_val: Optional[str] = None
                if i_clk is not None and i_clk < len(clean):
                    clk_str = clean[i_clk]
                    if " / " in clk_str:
                        clock_val, reset_val = [s.strip() for s in clk_str.split(" / ", 1)]
                    else:
                        clock_val = clk_str.strip()

                entry: dict = {
                    "name": clean_name,
                    "block": block,
                }
                if i_dir is not None and i_dir < len(clean):
                    entry["direction"] = clean[i_dir]
                if i_proto is not None and i_proto < len(clean):
                    raw_proto = clean[i_proto]
                    entry["protocol"] = _PROTOCOL_MAP.get(raw_proto, raw_proto)
                if clock_val is not None:
                    entry["clock"] = clock_val
                if reset_val is not None:
                    entry["reset"] = reset_val
                # channels[] / signals[] 在 generate_ni_signals_json 後處理（需要 ns）
                interfaces.append(entry)
    return interfaces


def generate_ni_signals_json(md_dir: Union[str, Path]) -> dict:
    """讀 signal_interface.md 產出 ni_signals.json — top-level only。

    每個 interface 自帶它的 parameters (inline，不另列 standalone array)。
    與 top-level interface 無關的 parameter (RoB / CDC / 行為相關) 不收進這份 JSON。
    """
    md_path = Path(md_dir) / "signal_interface.md"
    if not md_path.exists():
        raise FileNotFoundError(f"找不到 {md_path}")
    md_text = md_path.read_text(encoding="utf-8")

    signal_params = parse_signal_parameters(md_text)
    interfaces = parse_top_level_interfaces(md_text)

    # 兩源聯集成 params namespace（signal_interface + packet_format Group 3/6）
    ns = _build_params_namespace(md_dir, signal_params)

    # Sanity check: 所有引用的 parameter name 都要存在 namespace
    referenced = set(_BLOCK_ENABLE_PARAMS) | set(_AXI_PORT_PARAMS)
    for names in _INTERFACE_PARAMS.values():
        referenced.update(names)
    for ch_list in _AXI_CHANNEL_SIGNALS.values():
        for _, src_param in ch_list:
            referenced.add(src_param)
    missing = referenced - set(ns.keys())
    if missing:
        raise ValueError(f"interface param reference 不存在於 spec source: {sorted(missing)}")

    # 後處理每個 interface
    for iface in interfaces:
        port_type = _port_type_of(iface)
        if port_type is not None:
            # AXI / AXI4-Lite: 展 per-channel per-signal
            iface["channels"] = _build_axi_channels(port_type, ns, with_signals=True)
            port_params = _select_params(ns, _AXI_PORT_PARAMS)
            if port_params:
                iface["port_parameters"] = port_params
        elif iface["name"] in _NOC_INTERFACE_SIGNALS:
            # NoC link: 展 per-signal
            iface["signals"] = _build_noc_signals(iface["name"], ns)
            param_names = _INTERFACE_PARAMS.get(iface["name"], [])
            if param_names:
                iface["port_parameters"] = _select_params(ns, param_names)
        else:
            # CSR / 其他: 簡單 port_parameters list
            param_names = _INTERFACE_PARAMS.get(iface["name"], [])
            if param_names:
                iface["port_parameters"] = _select_params(ns, param_names)

    return {
        "$schema_version": "ni-spec/2.0",
        "meta": {
            "block": "NI signal interface (top-level)",
            "spec_version": "v0.4.0",
        },
        "block_enables": _select_params(ns, _BLOCK_ENABLE_PARAMS),
        "interfaces": interfaces,
    }


def write_generated_signals_json(md_dir: Union[str, Path], out_path: Union[str, Path]) -> dict:
    data = generate_ni_signals_json(md_dir)

    md_dir_path = Path(md_dir) if not isinstance(md_dir, Path) else md_dir

    # Task 3: cross-merge pin_name + reset_behavior from pin_level_reset.md
    reset_map = parse_pin_level_reset(md_dir_path / "pin_level_reset.md")

    for iface in data["interfaces"]:
        for ch in iface.get("channels", []):
            for sig in ch.get("signals", []):
                pin_name = _derive_pin_name(iface, ch, sig)
                sig["pin_name"] = pin_name
                sig["reset_behavior"] = reset_map.get(pin_name) or _default_reset_for(sig, iface, pin_name)
                sig.setdefault("presence", None)
                # PP-9: width_expr stays None for AXI/CSR pins (width comes from
                # port_parameters via width_param); only NoC literal-width pins
                # set it explicitly in _build_noc_signals.
                sig.setdefault("width_expr", None)
                sig.pop("_sig_suffix", None)  # build-time helper; not part of schema
        for sig in iface.get("signals", []):
            pin_name = _derive_pin_name(iface, None, sig)
            sig["pin_name"] = pin_name
            sig["reset_behavior"] = reset_map.get(pin_name) or _default_reset_for(sig, iface, pin_name)
            sig.setdefault("presence", None)
            sig.setdefault("width_expr", None)
            sig.pop("_sig_name", None)  # build-time helper; not part of schema

    # Extract and inject meta.reset_signals
    reset_signals = extract_reset_signals(md_dir_path / "pin_level_reset.md")
    data.setdefault("meta", {})["reset_signals"] = reset_signals

    p = Path(out_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return data


def parse_pin_level_reset(md_path: Path) -> dict:
    """Parse the pin-level reset table from pin_level_reset.md.

    Returns {pin_name: reset_behavior_dict}, e.g.
        {"axi_awid_i": {"kind": "external_driven"},
         "noc_req_valid_o": {"kind": "async-active-low", "value": "0", "domain": "noc_rst_ni"},
         ...}

    Only parses the first half of the file (the "During reset" section) to avoid
    double-counting the identical "After reset" tables.  Rows that don't match
    the expected pattern are skipped silently.
    """
    text = md_path.read_text(encoding="utf-8")

    # Only parse up to "## After reset" to avoid duplicating entries
    after_reset_m = re.search(r"^## After reset", text, re.MULTILINE)
    if after_reset_m:
        text = text[: after_reset_m.start()]

    result = {}

    # Match rows: | CHANNEL_TAG | pin_name[optional_range] | reset_text | ...
    # Channel tag ends with _IN, _OUT, or is CSR_* / REQ_* / RSP_* etc.
    table_row = re.compile(
        r"\|\s*([A-Z_]+)\s*\|\s*`?([a-zA-Z_][a-zA-Z0-9_]*)`?(?:\s*\[[^\]]*\])?\s*\|\s*([^|]+?)\s*\|"
    )
    for m in table_row.finditer(text):
        channel_tag = m.group(1).strip()
        pin = m.group(2).strip()
        reset_text = m.group(3).strip().strip("`").strip()

        # Skip header rows
        if pin.lower() in ("signal", "name", "channel"):
            continue

        rt_lower = reset_text.lower()

        # Input wires driven externally
        if ("driven by" in rt_lower or "external" in rt_lower
                or rt_lower.startswith("input") or "as driven" in rt_lower):
            rb = {"kind": "external_driven"}
        elif "pass-through" in rt_lower or "pass through" in rt_lower:
            rb = {"kind": "external_driven"}
        elif re.match(r"^0\s*(\(.*\))?$", reset_text) or reset_text in ("1'b0", "0x0") \
                or re.match(r"^0x0+$", reset_text) or reset_text == "0 (all bits)":
            if pin.startswith("noc_"):
                domain = "noc_rst_ni"
            elif pin.startswith("csr_") or pin.startswith("axi_"):
                domain = "arst_ni"
            else:
                # Unknown prefix — default to arst_ni and let L2 catch domain whitelist violation
                domain = "arst_ni"
            rb = {"kind": "async-active-low", "value": "0", "domain": domain}
        elif re.match(r"^1\s*(\(.*\))?$", reset_text) or reset_text == "1'b1":
            if pin.startswith("noc_"):
                domain = "noc_rst_ni"
            else:
                domain = "arst_ni"
            rb = {"kind": "async-active-low", "value": "1", "domain": domain}
        else:
            warnings.warn(
                f"parse_pin_level_reset: unrecognized reset text {reset_text!r} for pin {pin!r} "
                f"in {md_path.name}; skipping (will use _default_reset_for)",
                stacklevel=2,
            )
            continue

        result[pin] = rb
    return result


def _derive_pin_name(iface: dict, ch: Optional[dict], sig: dict) -> str:
    """Derive the RTL-level pin name from interface / channel / signal context.

    Rules:
    - NoC interfaces: signal 'name' is already pin-style (without noc_ prefix); add it.
    - AXI slave/master port: axi_<ch_lower><sig_suffix_lower>_i/o
      sig_suffix comes from sig['_sig_suffix'] (e.g. "ID" → "id"), set during build.
      Direction suffix (_i/_o) matches the channel direction.
    - CSR interface (AXI4-Lite slave): csr_<ch_lower><sig_suffix_lower>_i/o
      Same direction logic as AXI slave.
    """
    iface_name = iface.get("name", "")

    # --- NoC interfaces: prepend noc_ to the build-time signal name helper ---
    if iface_name in _NOC_INTERFACE_SIGNALS:
        sig_name = sig.get("_sig_name", "")
        return f"noc_{sig_name}"

    # --- CSR (AXI4-Lite) or AXI port ---
    if ch is None:
        # Fallback: should not happen for AXI/CSR interfaces
        return sig.get("_sig_suffix", "").lower()

    ch_name = ch.get("name", "")  # "AW", "W", "B", "AR", "R"
    ch_dir = ch.get("direction", "")  # "input" or "output"
    dir_suffix = "_i" if ch_dir == "input" else "_o"

    # _sig_suffix is the signal suffix set during _build_axi_channels (e.g. "ID", "ADDR")
    sig_suffix = sig.get("_sig_suffix", "").lower()

    if iface_name.startswith("CSR") or iface.get("protocol") == "AXI4-Lite":
        return f"csr_{ch_name.lower()}{sig_suffix}{dir_suffix}"
    else:
        return f"axi_{ch_name.lower()}{sig_suffix}{dir_suffix}"


def _default_reset_for(sig: dict, iface: dict, pin_name: str) -> dict:
    """Fallback reset_behavior for signals not found in pin_level_reset.md table.

    Output wire → async-active-low, value=0, domain depends on interface.
    Input wire → external_driven.
    """
    direction = sig.get("direction", "")
    # For AXI channel signals, direction comes from the channel, not the sig entry.
    # sig entries under channels[] don't have a "direction" key — use pin suffix.
    pin = pin_name or ""
    is_output = (direction == "output") or pin.endswith("_o")
    if is_output:
        iface_name = iface.get("name", "")
        domain = "noc_rst_ni" if iface_name in _NOC_INTERFACE_SIGNALS else "arst_ni"
        return {"kind": "async-active-low", "value": "0", "domain": domain}
    return {"kind": "external_driven"}


def extract_reset_signals(pin_level_reset_md: Path) -> list:
    """Extract the reset signal whitelist from pin_level_reset.md.

    The MD file has a section headed by **Reset signals:** followed by
    bullet lines of the form:
        - `arst_ni` (description...)
        - `noc_rst_ni` (description...)

    Returns list of reset signal names (order-preserving).
    """
    if not pin_level_reset_md.exists():
        raise FileNotFoundError(f"pin_level_reset.md not found at {pin_level_reset_md}")
    text = pin_level_reset_md.read_text(encoding="utf-8")

    # Find the **Reset signals:** line (with or without bold markers).
    # The heading may be formatted as **Reset signals:** (bold) so the match
    # ends before the closing ** and/or trailing whitespace on the heading line.
    m = re.search(r"\*{0,2}Reset signals:?\*{0,2}\s*\n", text)
    if not m:
        raise ValueError(f"No 'Reset signals:' section found in {pin_level_reset_md}")

    # Scan lines after the heading; collect backtick-names from bullet lines
    after = text[m.end():]
    results = []
    for line in after.splitlines():
        stripped = line.strip()
        if not stripped:
            break  # blank line ends the block
        if not stripped.startswith("-"):
            break  # non-bullet line ends the block
        # Extract first backtick-wrapped identifier on this bullet line
        names = re.findall(r"`([a-zA-Z_][a-zA-Z0-9_]*)`", stripped)
        if names:
            results.append(names[0])
    return results


# ════════════════════════════════════════════════════════════════════
# registers.md — Task 4: CSR register domain
# ════════════════════════════════════════════════════════════════════


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


# Em-dash characters that appear in reserved row Access/Reset cells.
# Codepoints: U+2014 em-dash (—), U+2013 en-dash (–), U+002D hyphen-minus (-).
# The original set had two U+2014 entries; deduplicated here.
_EM_DASH_VARIANTS = {
    "—",  # U+2014 em-dash
    "–",  # U+2013 en-dash
    "-",  # U+002D hyphen-minus
}


def _is_dash(cell: str) -> bool:
    """True if cell content is an em-dash / en-dash / hyphen placeholder.

    Accept plain hyphen (U+002D) too — some MD authors use it as placeholder;
    safe because real hex offsets / RW1C tokens never start with bare hyphen.
    """
    stripped = cell.strip().strip("`")
    return stripped in _EM_DASH_VARIANTS


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
        if _is_dash(access_raw) or _is_dash(reset_raw):
            rows.append({
                "offset": offset,
                "name": name,
                "kind": "reserved",
                "access": None,
                "reset_expr": None,
            })
            continue

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


# ════════════════════════════════════════════════════════════════════
# protocol_rules.md — Task 6: minimal metadata lift-shift
# Extracts: id, severity, source_section, source_line, proto only.
# Does NOT extract prose columns (condition_summary, channels, etc.).
# ════════════════════════════════════════════════════════════════════


_PROTO_MAP = [
    ("Reset", "RESET"),
    ("CDC", "CDC"),
    ("AXI4 host-side", "AXI4"),
    ("NoC flit-side", "NOC"),
    ("CSR access", "CSR"),
    ("Configuration-knob", "CONFIG"),
    ("Interrupt", "INTERRUPT"),
]

_SECTION_PAT = re.compile(r"^##\s+(.+)$")
# Rule row: ID is uppercase+underscores, severity is FAIL/WARN/RECOMMEND/MUST/MUST NOT.
# Pattern: | ID | (anything) | (anything) | SEVERITY | ...
_ROW_PAT = re.compile(
    r"^\|\s*([A-Z][A-Z0-9_]+)\s*\|[^|]*\|[^|]*\|\s*(MUST NOT|MUST|SHOULD NOT|SHOULD|MAY)\s*\|"
)


def _infer_proto(section: str) -> str:
    """Map section heading to proto enum via substring match (case-insensitive)."""
    for key, val in _PROTO_MAP:
        if key.lower() in section.lower():
            return val
    return "CONFIG"  # default fallback (unreachable for known spec sections)


def parse_protocol_rule_index(md_path: Path) -> list:
    """Extract structured metadata from protocol_rules.md.

    Returns list of dicts with: id, severity, source_section, source_line, proto.
    Prose columns (Condition, Required behavior, ARM SVA equivalent) are NOT parsed.

    The MD has tables structured:
        | ID | Condition | Required behavior | Severity | ARM SVA equivalent |
    under `## <section name>` headings. Sub-headings `### ...` belong to same proto.
    """
    text = md_path.read_text(encoding="utf-8")
    lines = text.splitlines()

    rules = []
    cur_section = None  # tracked from `## ` headings only

    for i, line in enumerate(lines, start=1):
        ms = _SECTION_PAT.match(line)
        if ms:
            cur_section = ms.group(1).strip()
            continue
        m = _ROW_PAT.match(line)
        if not m or cur_section is None:
            continue
        rid, severity = m.group(1), m.group(2)
        # Skip header rows: literal "ID" can't match UPPER pattern but guard anyway.
        if rid == "ID":
            continue
        rules.append({
            "id": rid,
            "severity": severity,
            "source_section": cur_section,
            "source_line": i,
            "proto": _infer_proto(cur_section),
        })
    return rules


def generate_ni_protocol_rule_index_json(md_dir, out_path: Path) -> dict:
    """Compose ni_protocol_rule_index.json from protocol_rules.md."""
    md_dir_path = Path(md_dir) if not isinstance(md_dir, Path) else md_dir
    version_file = md_dir_path.parent / "VERSION"
    if not version_file.exists():
        raise FileNotFoundError(f"spec/ni/VERSION not found at {version_file}")
    spec_version = version_file.read_text(encoding="utf-8").strip()

    result = {
        "$schema_version": "ni-spec/2.0",
        "meta": {
            "spec_version": spec_version,
        },
        "rules": parse_protocol_rule_index(md_dir_path / "protocol_rules.md"),
    }
    out_path_p = Path(out_path)
    out_path_p.parent.mkdir(parents=True, exist_ok=True)
    out_path_p.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    return result


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
