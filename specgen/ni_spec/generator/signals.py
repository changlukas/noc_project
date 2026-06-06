"""signal_interface.md (+ pin_level_reset.md cross-merge) → ni_signals.json.

Path B simplified flow:
  - parameters: from §Parameters
  - interfaces: from §Per-block interface summary NMU/NSU tables
  - AXI 5-channel matrix expansion via _AXI_CHANNEL_SIGNALS template
  - NoC link signals via _NOC_INTERFACE_SIGNALS template
  - pin_name + reset_behavior cross-merged from pin_level_reset.md

Per-wire detail lives in signal_interface.md for human review and is not
machine-extracted.
"""

from __future__ import annotations
import json
import re
import warnings
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Union

from ._common import (
    _strip_cell,
    _section_slice,
    _col_idx,
    _extract_all_tables,
)
from .packet import parse_field_widths, parse_derived


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
                warnings.warn(
                    f"parse_pin_level_reset: unknown pin prefix {pin!r}, "
                    f"defaulting reset-domain to arst_ni",
                    stacklevel=2,
                )
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
