"""Spec 萃取常數的純函式。

C-model 直接 import 這個模組即可取得所有 packet 寬度／bit 位置／enum 編碼，
不該再有任何手抄常數。C++ C-model 透過 codegen 從這層產生 ni_flit_constants.h。
"""

from __future__ import annotations
from typing import Dict


def _int_params(packet_spec) -> dict:
    """parameters[] 中 type=int 的 name→default。"""
    return {p["name"]: p["default"]
            for p in packet_spec.get("parameters", [])
            if p["type"] == "int"}


def _resolved_field_widths(packet_spec) -> dict:
    """flit.field_widths 覆蓋 parameters[]，給寬度運算式做求值用。"""
    return {**_int_params(packet_spec), **packet_spec["flit"].get("field_widths", {})}


# PP-10: legacy thin getters (flit_width/header_width/payload_width/link_width/
# header_field_pos/payload_field_pos/all_header_fields) were removed.
# They read flit.derived[...] / lsb / msb which PP-6 dropped from JSON. Use the
# *_resolved variants and header_field_position / payload_field_position below.


def all_field_widths(packet_spec) -> Dict[str, int]:
    """所有 field_widths 解析後的寬度，給 C-model packer 用。"""
    return dict(_resolved_field_widths(packet_spec))


def header_field_enabled(packet_spec, field_name: str) -> bool:
    """Return whether header field is functional (True) or padding (False).

    A field is functional when its ``enabled`` property is True (the default).
    Padding fields are currently stubbed to 0 and not driven by hardware.
    """
    for f in packet_spec["flit"]["header_fields"]:
        if f["name"] == field_name:
            return f.get("enabled", True)
    raise KeyError(f"header field {field_name!r} not in spec")


def header_fields_padding(packet_spec) -> list:
    """Return list of field names marked enabled=false (padding/stubbed)."""
    return [f["name"] for f in packet_spec["flit"]["header_fields"]
            if not f.get("enabled", True)]


def axi_channel_encoding(packet_spec) -> Dict[str, int]:
    """axi_ch 欄位的 {channel_name: value}。沒有 encoding 欄位則回 {}。"""
    for f in packet_spec["flit"]["header_fields"]:
        if f["name"] == "axi_ch" and "encoding" in f:
            return {name: int(v) for v, name in f["encoding"].items()}
    return {}


def field_encoding(packet_spec, field_name: str) -> Dict[str, int]:
    """通用：任何 header field 上的 encoding 表，{name: value}。"""
    for f in packet_spec["flit"]["header_fields"]:
        if f["name"] == field_name and "encoding" in f:
            return {name: int(v) for v, name in f["encoding"].items()}
    return {}


# ---------- signals domain ----------

def signals_pin_names(signals_spec) -> list:
    """Return list of all non-null pin_name across all signals."""
    out = []
    for iface in signals_spec.get("interfaces", []):
        for ch in iface.get("channels", []):
            for sig in ch.get("signals", []):
                if sig.get("pin_name"):
                    out.append(sig["pin_name"])
        for sig in iface.get("signals", []):
            if sig.get("pin_name"):
                out.append(sig["pin_name"])
    return out


def signals_reset_domains(signals_spec) -> set:
    """Return set of legal reset signal names from meta.reset_signals[]."""
    return set(signals_spec.get("meta", {}).get("reset_signals", []))


def signals_signal_by_pin(signals_spec, pin_name: str) -> dict:
    """Lookup signal entry by RTL-level pin_name. Returns None if not found."""
    for iface in signals_spec.get("interfaces", []):
        for ch in iface.get("channels", []):
            for sig in ch.get("signals", []):
                if sig.get("pin_name") == pin_name:
                    return sig
        for sig in iface.get("signals", []):
            if sig.get("pin_name") == pin_name:
                return sig
    return None


def signals_pins_by_interface(signals_spec) -> dict:
    """Return {interface_name: [signal_dict, ...]} for pin-bundle elaboration.

    Each entry exposes the fields a C++/SV emitter needs to materialise an
    interface bundle struct: pin_name, direction, width_param/width_expr
    (symbolic, to be resolved via ``signal_pin_width``), and reset_behavior.
    Direction for channeled signals is inherited from the channel;
    interface-level (NoC link) signals carry their own ``direction`` field.

    PP-9: per-pin ``default`` is no longer present in JSON. Callers MUST
    resolve widths via ``signal_pin_width`` (which consults the merged
    namespace of packet field_widths + interface port_parameters).
    """
    out: dict = {}
    for iface in signals_spec.get("interfaces", []):
        name = iface["name"]
        pins: list = []
        for ch in iface.get("channels", []):
            ch_dir = ch.get("direction")
            for sig in ch.get("signals", []):
                pins.append({
                    "pin_name":       sig["pin_name"],
                    "direction":      sig.get("direction") or ch_dir,
                    "width_param":    sig.get("width_param"),
                    "width_expr":     sig.get("width_expr"),
                    "reset_behavior": sig.get("reset_behavior"),
                })
        for sig in iface.get("signals", []):
            pins.append({
                "pin_name":       sig["pin_name"],
                "direction":      sig.get("direction"),
                "width_param":    sig.get("width_param"),
                "width_expr":     sig.get("width_expr"),
                "reset_behavior": sig.get("reset_behavior"),
            })
        out[name] = pins
    return out


# ---------- registers domain (Task 4 will implement) ----------

def regs_offsets(regs_spec) -> dict:
    """Return {register_name: offset_int} for kind=register entries."""
    return {r["name"]: int(r["offset"], 16)
            for r in regs_spec.get("registers", [])
            if r.get("kind") == "register"}


def regs_field_mask(regs_spec, reg_name: str, field_name: str) -> int:
    """Return bit mask for a register field. Raises KeyError if not found."""
    for r in regs_spec.get("registers", []):
        if r.get("name") != reg_name:
            continue
        for f in r.get("fields", []):
            if f.get("name") == field_name:
                hi, lo = int(f["bit_high"]), int(f["bit_low"])
                return ((1 << (hi - lo + 1)) - 1) << lo
    raise KeyError(f"{reg_name}.{field_name}")


def regs_access_mode(regs_spec, reg_name: str) -> str:
    """Return access mode (RO/RW/RW1C/WO/WC) for a register. Raises KeyError if not found."""
    for r in regs_spec.get("registers", []):
        if r.get("kind") != "register":
            continue
        if r.get("name") == reg_name:
            return r.get("access")
    raise KeyError(reg_name)


# ---------- pure-parameterization elaborator helpers (PP-2) ----------
#
# These compute the same values currently stored as `derived` / `lsb` / `msb`
# in ni_packet.json. They are introduced here so codegen + tests can switch
# to them in subsequent tasks (PP-3+), at which point the stored fields
# can be removed from the JSON entirely (PP-6).

import ast as _ast
from typing import Mapping as _Mapping
from .exceptions import (
    ExprSyntaxError, ExprNameError, ExprNotAllowedError, FieldNotFoundError,
)


_ALLOWED_BINOPS = {_ast.Add, _ast.Sub, _ast.Mult, _ast.FloorDiv, _ast.Mod}
_ALLOWED_UNARYOPS = {_ast.UAdd, _ast.USub}


def _eval_ast(node, namespace: _Mapping[str, int]) -> int:
    if isinstance(node, _ast.Constant):
        if isinstance(node.value, int):
            return node.value
        raise ExprNotAllowedError(f"only integer literals allowed, got {type(node.value).__name__}")
    if isinstance(node, _ast.Name):
        if node.id in namespace:
            return int(namespace[node.id])
        raise ExprNameError(f"symbol '{node.id}' not found in namespace")
    if isinstance(node, _ast.BinOp):
        if type(node.op) not in _ALLOWED_BINOPS:
            raise ExprNotAllowedError(f"forbidden binop {type(node.op).__name__}")
        l = _eval_ast(node.left,  namespace)
        r = _eval_ast(node.right, namespace)
        op_map = {_ast.Add: int.__add__, _ast.Sub: int.__sub__, _ast.Mult: int.__mul__,
                  _ast.FloorDiv: int.__floordiv__, _ast.Mod: int.__mod__}
        return op_map[type(node.op)](l, r)
    if isinstance(node, _ast.UnaryOp):
        if type(node.op) not in _ALLOWED_UNARYOPS:
            raise ExprNotAllowedError(f"forbidden unaryop {type(node.op).__name__}")
        v = _eval_ast(node.operand, namespace)
        return v if isinstance(node.op, _ast.UAdd) else -v
    raise ExprNotAllowedError(f"forbidden ast node {type(node).__name__}")


def packet_eval_expr(spec: dict, expr) -> int:
    """Evaluate a width_param expression in the packet field_widths namespace.

    Handles:
      - integer literal (returned as-is)
      - the special string "derived" -> caller must handle this case
        before calling packet_eval_expr (raises ExprNotAllowedError otherwise)
      - any other string: parsed with ast and walked with allowlist
    """
    if isinstance(expr, int):
        return expr
    if expr == "derived":
        raise ExprNotAllowedError(
            "width_param='derived' must be resolved by payload_field_width, not packet_eval_expr"
        )
    if not isinstance(expr, str):
        raise ExprSyntaxError(f"width_param must be str or int, got {type(expr).__name__}")
    try:
        tree = _ast.parse(expr, mode="eval")
    except SyntaxError as e:
        raise ExprSyntaxError(f"cannot parse '{expr}': {e}") from e
    namespace = spec.get("flit", {}).get("field_widths", {})
    return _eval_ast(tree.body, namespace)


def packet_param_value(spec: dict, name: str) -> int:
    """Look up a parameter in flit.field_widths."""
    fw = spec.get("flit", {}).get("field_widths", {})
    if name not in fw:
        raise ExprNameError(f"parameter '{name}' not in field_widths")
    return int(fw[name])


def _find_header_field(spec: dict, name: str) -> dict:
    for f in spec["flit"]["header_fields"]:
        if f["name"] == name:
            return f
    raise FieldNotFoundError(f"header field '{name}' not found")


def header_field_width(spec: dict, name: str) -> int:
    """Resolve width by evaluating width_param against field_widths.

    Special case ``width_param == "derived"`` mirrors payload's derived
    handling: the field's width is computed as
    ``HEADER_TOTAL_WIDTH - sum(other fields' widths)``. This anchors a
    fixed-size header layout; the derived field acts as compile-time
    padding to keep HEADER_WIDTH constant regardless of which optional
    fields are enabled. At most one ``derived`` field per header_fields
    (enforced in invariants).
    """
    f = _find_header_field(spec, name)
    wp = f["width_param"]
    if wp == "derived":
        total = packet_param_value(spec, "HEADER_TOTAL_WIDTH")
        others_sum = 0
        for of in spec["flit"]["header_fields"]:
            if of["name"] == name:
                continue
            owp = of["width_param"]
            if owp == "derived":
                continue
            others_sum += packet_eval_expr(spec, owp)
        return total - others_sum
    return packet_eval_expr(spec, wp)


def header_field_position(spec: dict, name: str):
    """(lsb, msb) computed cumulatively in declaration order.
    Returns None for width-0 placeholders."""
    cumulative = 0
    for f in spec["flit"]["header_fields"]:
        w = header_field_width(spec, f["name"])
        if f["name"] == name:
            return None if w == 0 else (cumulative, cumulative + w - 1)
        cumulative += w
    raise FieldNotFoundError(f"header field '{name}' not found")


def _find_channel(spec: dict, channel: str) -> dict:
    for ch in spec["flit"]["payload_channels"]:
        if ch["name"] == channel:
            return ch
    raise FieldNotFoundError(f"channel '{channel}' not found")


def payload_channel_width(spec: dict, channel: str) -> int:
    """Authored channel-level metadata."""
    return int(_find_channel(spec, channel)["payload_width"])


def payload_field_width(spec: dict, channel: str, name: str) -> int:
    """Resolve width. Special case: width_param='derived' ->
    payload_width(channel) - sum of all other fields' widths.

    At most one field per channel may declare width_param='derived'.
    """
    ch = _find_channel(spec, channel)
    derived_count = sum(1 for f in ch["fields"] if f["width_param"] == "derived")
    if derived_count > 1:
        raise ExprNotAllowedError(
            f"channel '{channel}' has multiple 'derived' fields; "
            f"only one allowed per channel"
        )
    target = None
    others_sum = 0
    for f in ch["fields"]:
        if f["name"] == name:
            target = f
            continue
        wp = f["width_param"]
        if wp == "derived":
            # The channel's lone derived field contributes the remainder; it
            # cannot itself appear in the running sum used to compute that
            # remainder. Skip it here.
            continue
        others_sum += packet_eval_expr(spec, wp)
    if target is None:
        raise FieldNotFoundError(f"payload field '{name}' not in channel '{channel}'")
    if target["width_param"] == "derived":
        return payload_channel_width(spec, channel) - others_sum
    return packet_eval_expr(spec, target["width_param"])


def payload_field_position(spec: dict, channel: str, name: str):
    """(lsb, msb) within the channel's payload, cumulative declaration order."""
    ch = _find_channel(spec, channel)
    cumulative = 0
    for f in ch["fields"]:
        w = payload_field_width(spec, channel, f["name"])
        if f["name"] == name:
            return None if w == 0 else (cumulative, cumulative + w - 1)
        cumulative += w
    raise FieldNotFoundError(f"payload field '{name}' not in channel '{channel}'")


# ---------- derived totals (computed from helpers above) ----------
#
# All consumers (codegen, invariants, tests) use these resolved helpers.
# PP-6 dropped flit.derived from JSON; PP-10 removed the legacy thin
# getters that read it. The `_resolved` suffix is kept for explicit
# "computed on demand" semantics.

def header_width_resolved(spec: dict) -> int:
    """Sum of all header field widths (regardless of enabled)."""
    return sum(header_field_width(spec, f["name"])
               for f in spec["flit"]["header_fields"])


def payload_width_resolved(spec: dict) -> int:
    """Max of all payload_channels' payload_width (channels are union-typed
    by axi_ch encoding; flit allocates max channel width)."""
    return max(payload_channel_width(spec, ch["name"])
               for ch in spec["flit"]["payload_channels"])


def flit_width_resolved(spec: dict) -> int:
    return header_width_resolved(spec) + payload_width_resolved(spec)


def link_width_resolved(spec: dict) -> int:
    """LINK_WIDTH = FLIT_WIDTH + 1 (valid) + NUM_VC (per-VC credit return).

    The forward bundle on a NoC link carries the flit plus a valid signal;
    the reverse bundle returns one credit bit per VC. Total wire count for
    one logical link is therefore FLIT_WIDTH + 1 + NUM_VC.

    NUM_VC defaults to 1 (single-VC); override via flit.field_widths.NUM_VC.
    Per specgen/generated/json/ni_packet.json and ni_signals.json NOC_REQ_OUT /
    NOC_RSP_OUT (noc_*_valid + noc_*_flit + noc_*_credit signals)."""
    fw = spec["flit"].get("field_widths", {})
    num_vc = int(fw.get("NUM_VC", 1))
    return flit_width_resolved(spec) + 1 + num_vc


def flit_data_width_resolved(spec: dict) -> int:
    """FLIT_DATA_WIDTH = HEADER_WIDTH - FLIT_ECC_WIDTH + PAYLOAD_WIDTH"""
    fw = spec.get("flit", {}).get("field_widths", {})
    ecc_w = int(fw.get("FLIT_ECC_WIDTH", 0))
    return header_width_resolved(spec) - ecc_w + payload_width_resolved(spec)


def header_data_width_resolved(spec: dict) -> int:
    """HEADER_DATA_WIDTH = HEADER_WIDTH - FLIT_ECC_WIDTH"""
    fw = spec.get("flit", {}).get("field_widths", {})
    return header_width_resolved(spec) - int(fw.get("FLIT_ECC_WIDTH", 0))


def wstrb_width_resolved(spec: dict) -> int:
    """WSTRB_WIDTH = NOC_DATA_WIDTH / 8"""
    fw = spec.get("flit", {}).get("field_widths", {})
    return int(fw.get("NOC_DATA_WIDTH", 0)) // 8


# ---------- signals domain resolvers (PP-7) ----------
#
# Signals reference symbols from two namespaces:
#   1) the interface's own port_parameters (e.g. NUM_VC, ENABLE_AXI_PARITY)
#   2) packet-domain symbols (FLIT_WIDTH, HEADER_WIDTH, AXI_*_WIDTH, ...)
# These resolvers therefore accept BOTH specs and merge namespaces in a
# documented priority order.

def _find_interface(signals_spec: dict, interface: str) -> dict:
    for iface in signals_spec.get("interfaces", []):
        if iface.get("name") == interface:
            return iface
    raise FieldNotFoundError(f"interface '{interface}' not found")


def signal_interfaces(signals_spec: dict) -> list:
    """Return interface name list in declaration order."""
    return [iface["name"] for iface in signals_spec.get("interfaces", [])]


def signal_interface_pins(signals_spec: dict, interface: str) -> list:
    """Return flat list of signal dicts for an interface.

    Handles both interface layouts in ni_signals.json:
      - channeled interfaces (AXI_*, CSR): pins live under channels[].signals[]
      - direct interfaces (NOC_*): pins live under signals[]
    Each returned dict is the raw signal entry from the JSON.
    """
    iface = _find_interface(signals_spec, interface)
    pins: list = []
    for ch in iface.get("channels", []):
        for sig in ch.get("signals", []):
            pins.append(sig)
    for sig in iface.get("signals", []):
        pins.append(sig)
    return pins


def _signal_namespace(signals_spec: dict, packet_spec: dict, interface: str) -> dict:
    """Merged namespace for a signals-domain expression.

    Lookup order (later entries override earlier on key collision):
      1) packet flit.field_widths (AXI_*_WIDTH, NOC_DATA_WIDTH, ...)
      2) packet derived totals (FLIT_WIDTH, HEADER_WIDTH, PAYLOAD_WIDTH, LINK_WIDTH)
      3) the interface's own port_parameters (NUM_VC, ENABLE_AXI_PARITY, ...)
    Interface port_parameters win last because they are the most local scope.
    """
    iface = _find_interface(signals_spec, interface)
    ns: dict = {}
    # 1) packet field_widths (the broad cross-domain pool)
    for k, v in packet_spec.get("flit", {}).get("field_widths", {}).items():
        try:
            ns[k] = int(v)
        except (TypeError, ValueError):
            # non-int entries (shouldn't exist but stay defensive) are skipped
            pass
    # 2) packet derived totals
    ns["FLIT_WIDTH"]    = flit_width_resolved(packet_spec)
    ns["HEADER_WIDTH"]  = header_width_resolved(packet_spec)
    ns["PAYLOAD_WIDTH"] = payload_width_resolved(packet_spec)
    ns["LINK_WIDTH"]    = link_width_resolved(packet_spec)
    # 3) interface-local port_parameters (most specific scope; can shadow above)
    for pp in iface.get("port_parameters", []):
        name = pp.get("name")
        if name is None:
            continue
        # Only int-valued port_parameters enter the integer namespace. Bool/string
        # parameters (e.g. ENABLE_AXI_PARITY) are not numeric and are skipped;
        # if a width_param ever references them, ExprNameError is the right
        # signal that the spec is malformed.
        ptype = pp.get("type")
        if ptype == "int":
            try:
                ns[name] = int(pp.get("default"))
            except (TypeError, ValueError):
                pass
        elif ptype == "derived":
            # Derived port_parameters mirror packet domain (e.g. FLIT_WIDTH).
            # Skip — packet domain entry above is the canonical source.
            pass
    return ns


def signal_eval_expr(signals_spec: dict, packet_spec: dict,
                     interface: str, expr) -> int:
    """Evaluate a width_param/width_expr from the signals domain.

    Uses the same safe AST machinery as packet_eval_expr but with a merged
    namespace (see _signal_namespace).
    """
    if isinstance(expr, int):
        return expr
    if not isinstance(expr, str):
        raise ExprSyntaxError(
            f"width_param must be str or int, got {type(expr).__name__}"
        )
    try:
        tree = _ast.parse(expr, mode="eval")
    except SyntaxError as e:
        raise ExprSyntaxError(f"cannot parse '{expr}': {e}") from e
    ns = _signal_namespace(signals_spec, packet_spec, interface)
    return _eval_ast(tree.body, ns)


def signal_pin_width(signals_spec: dict, packet_spec: dict,
                     interface: str, pin_name: str) -> int:
    """Resolve a pin's width via the signals namespace.

    Priority:
      1) width_param (symbolic; resolved against merged namespace)
      2) width_expr  (symbolic expression; resolved against merged namespace)
      3) default     (legacy stored integer; tolerated during PP-7..PP-9)
      4) 1           (fall-through for valid/strobe-style 1-bit pins)
    """
    for sig in signal_interface_pins(signals_spec, interface):
        if sig.get("pin_name") != pin_name:
            continue
        wp = sig.get("width_param")
        if wp:
            return signal_eval_expr(signals_spec, packet_spec, interface, wp)
        we = sig.get("width_expr")
        if we:
            return signal_eval_expr(signals_spec, packet_spec, interface, we)
        d = sig.get("default")
        if d is not None:
            return int(d)
        return 1
    raise FieldNotFoundError(
        f"pin '{pin_name}' not in interface '{interface}'"
    )

