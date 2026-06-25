"""SV emitter for signals domain.

Produces rtl_pkg/ni_signals_pkg.sv.
Uses localparam int unsigned for reset constants (design doc 6.2).
After ``endpackage`` emits one SV ``interface`` block per entry in
``source/interface_handshake.json`` (SV interfaces cannot be declared
inside packages). The interface shapes (AXI4 channels and NoC link
modports) are derived from the handshake schema + constants.yaml.
"""
from __future__ import annotations
from pathlib import Path
import sys

SPECGEN_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(SPECGEN_ROOT))

from ni_spec import constants as C
from ni_spec.loader import load_doc
from ni_spec.handshake_schema import load_constants, load_interfaces


# ---------------------------------------------------------------------------
# AXI4 channel signal matrix (IHI 0022 §A2 + §A3).
# Each entry is (signal_name, sv_width_field). Width tokens reference the
# axi4_intf parameters (ID_WIDTH / ADDR_WIDTH / DATA_WIDTH) and the
# interface-local localparam WSTRB_WIDTH = DATA_WIDTH / 8.
# ---------------------------------------------------------------------------
_AXI_CHANNEL_SIGNALS: dict[str, list[tuple[str, str]]] = {
    "AW": [
        ("awid",     "[ID_WIDTH-1:0]"),
        ("awaddr",   "[ADDR_WIDTH-1:0]"),
        ("awlen",    "[7:0]"),
        ("awsize",   "[2:0]"),
        ("awburst",  "[1:0]"),
        ("awlock",   ""),
        ("awcache",  "[3:0]"),
        ("awprot",   "[2:0]"),
        ("awqos",    "[3:0]"),
        ("awregion", "[3:0]"),
        ("awvalid",  ""),
        ("awready",  ""),
    ],
    "W": [
        ("wdata",  "[DATA_WIDTH-1:0]"),
        ("wstrb",  "[WSTRB_WIDTH-1:0]"),
        ("wlast",  ""),
        ("wvalid", ""),
        ("wready", ""),
    ],
    "B": [
        ("bid",    "[ID_WIDTH-1:0]"),
        ("bresp",  "[1:0]"),
        ("bvalid", ""),
        ("bready", ""),
    ],
    "AR": [
        ("arid",     "[ID_WIDTH-1:0]"),
        ("araddr",   "[ADDR_WIDTH-1:0]"),
        ("arlen",    "[7:0]"),
        ("arsize",   "[2:0]"),
        ("arburst",  "[1:0]"),
        ("arlock",   ""),
        ("arcache",  "[3:0]"),
        ("arprot",   "[2:0]"),
        ("arqos",    "[3:0]"),
        ("arregion", "[3:0]"),
        ("arvalid",  ""),
        ("arready",  ""),
    ],
    "R": [
        ("rid",    "[ID_WIDTH-1:0]"),
        ("rdata",  "[DATA_WIDTH-1:0]"),
        ("rresp",  "[1:0]"),
        ("rlast",  ""),
        ("rvalid", ""),
        ("rready", ""),
    ],
}

# AXI signals driven by the master (manager) side. Everything else in the
# channel matrix is slave-driven. Used to compose modport direction lists.
_MASTER_DRIVES_AXI: frozenset[str] = frozenset({
    "awid", "awaddr", "awlen", "awsize", "awburst", "awlock", "awcache",
    "awprot", "awqos", "awregion", "awvalid",
    "wdata", "wstrb", "wlast", "wvalid",
    "bready",
    "arid", "araddr", "arlen", "arsize", "arburst", "arlock", "arcache",
    "arprot", "arqos", "arregion", "arvalid",
    "rready",
})


def _emit_param_header(name: str, iface_spec: dict, constants: dict) -> list[str]:
    """Emit the ``interface NAME #( ... );`` header.

    Parameter names are column-aligned. The default expression on each line
    is ``ni_params_pkg::<sv_symbol>`` taken from constants.yaml via the
    ``constants_yaml_key`` ref on every handshake parameter entry.
    """
    params = iface_spec.get("parameters", [])
    out: list[str] = [f"interface {name} #("]
    name_col = max(len(p["name"]) for p in params)
    for i, p in enumerate(params):
        domain, _, key = p["constants_yaml_key"].partition(".")
        sv_symbol = constants[domain][key]["sv_symbol"]
        sep = "," if i < len(params) - 1 else ""
        out.append(
            f"  parameter int unsigned {p['name']:<{name_col}} = "
            f"ni_params_pkg::{sv_symbol}{sep}"
        )
    out.append(");")
    return out


def _emit_axi4_intf(name: str, iface_spec: dict, constants: dict) -> list[str]:
    """Emit a full ``interface axi4_intf #(...) ... endinterface`` block."""
    out: list[str] = _emit_param_header(name, iface_spec, constants)
    # AXI requires the WSTRB_WIDTH localparam (depends on DATA_WIDTH).
    out.append("  localparam int unsigned WSTRB_WIDTH = DATA_WIDTH / 8;")
    out.append("")

    # Modport names come from JSON so AXI keeps master/slave while NoC can
    # use mosi/miso (or any future role names) without code changes here.
    modports = iface_spec.get("modports", ["master", "slave"])
    mp_out, mp_in = modports[0], modports[1]

    master_sigs: list[str] = []
    slave_sigs: list[str] = []
    channels = iface_spec.get("channels", list(_AXI_CHANNEL_SIGNALS.keys()))
    for ch in channels:
        sigs = _AXI_CHANNEL_SIGNALS[ch]
        out.append(f"  // {ch} channel")
        width_col = max(len(w) for _, w in sigs)
        for sig_name, width in sigs:
            out.append(f"  logic {width:<{width_col}} {sig_name};")
            if sig_name in _MASTER_DRIVES_AXI:
                master_sigs.append(sig_name)
            else:
                slave_sigs.append(sig_name)
        out.append("")

    # Two modports. mp_out drives master_sigs (output) and receives slave_sigs
    # (input); mp_in is the mirror.
    out.append(f"  modport {mp_out} (")
    out.append(f"    output {', '.join(master_sigs)},")
    out.append(f"    input  {', '.join(slave_sigs)}")
    out.append("  );")
    out.append("")
    out.append(f"  modport {mp_in} (")
    out.append(f"    input  {', '.join(master_sigs)},")
    out.append(f"    output {', '.join(slave_sigs)}")
    out.append("  );")
    out.append(f"endinterface : {name}")
    return out


def _emit_noc_intf(name: str, iface_spec: dict, constants: dict) -> list[str]:
    """Emit a NoC-link interface block (one set of signals + 2 modports).

    Modport names + per-signal ``driven_by`` strings come from JSON. The
    first modport in the list is treated as the producer side (its signals
    are ``output``); the second is the consumer side (its signals are
    ``input``). For the canonical noc_intf entry the modports are
    ``["mosi", "miso"]`` and ``driven_by`` tags signals as ``mosi`` or
    ``miso`` accordingly.
    """
    out: list[str] = _emit_param_header(name, iface_spec, constants)
    signals = iface_spec.get("signals", [])

    # Translate width_expr -> SV bit-vector field. ``1`` becomes empty (1-bit
    # scalar uses bare ``logic``); other expressions become ``[EXPR-1:0]``.
    width_fields: list[str] = []
    for s in signals:
        expr = s["width_expr"].strip()
        width_fields.append("" if expr == "1" else f"[{expr}-1:0]")
    width_col = max(len(w) for w in width_fields)

    for s, width in zip(signals, width_fields):
        out.append(f"  logic {width:<{width_col}} {s['name']};")
    out.append("")

    modports = iface_spec.get("modports", ["master", "slave"])
    mp_out, mp_in = modports[0], modports[1]
    out_names = [s["name"] for s in signals if s["driven_by"] == mp_out]
    in_names = [s["name"] for s in signals if s["driven_by"] == mp_in]
    out.append(
        f"  modport {mp_out} ( output {', '.join(out_names)}, "
        f"input  {', '.join(in_names)} );"
    )
    out.append(
        f"  modport {mp_in} ( input  {', '.join(out_names)}, "
        f"output {', '.join(in_names)} );"
    )
    out.append(f"endinterface : {name}")
    return out


# ---------------------------------------------------------------------------
# Width token translation for packed-struct typedefs (parameters not allowed).
# Interface uses parameterized widths; struct must use fully-qualified pkg refs.
# ---------------------------------------------------------------------------
_IFACE_WIDTH_TO_STRUCT: dict[str, str] = {
    "[ID_WIDTH-1:0]":   "[ni_params_pkg::AXI_ID_WIDTH_DFLT-1:0]",
    "[ADDR_WIDTH-1:0]": "[ni_params_pkg::AXI_ADDR_WIDTH_DFLT-1:0]",
    "[DATA_WIDTH-1:0]": "[ni_params_pkg::AXI_DATA_WIDTH_DFLT-1:0]",
    "[WSTRB_WIDTH-1:0]": "[ni_params_pkg::AXI_DATA_WIDTH_DFLT/8-1:0]",
    "[7:0]":  "[7:0]",
    "[2:0]":  "[2:0]",
    "[1:0]":  "[1:0]",
    "[3:0]":  "[3:0]",
    "":       "",
}


def _emit_noc_structs() -> list[str]:
    """Emit noc_chan_t and noc_credit_t typedef lines (in-package)."""
    out: list[str] = [
        "  // NoC link packed-struct typedefs (coexist with noc_intf; widths fixed-default).",
        "  typedef struct packed {",
        "    logic                                            valid;",
        "    logic [ni_params_pkg::NOC_FLIT_WIDTH_DFLT-1:0] flit;",
        "  } noc_chan_t;",
        "  typedef struct packed {",
        "    logic [ni_params_pkg::NOC_NUM_VC_DFLT-1:0] credit;",
        "  } noc_credit_t;",
    ]
    return out


def _emit_axi_structs(channels: list[str]) -> list[str]:
    """Emit axi_req_t (master-driven) and axi_rsp_t (slave-driven) typedef lines.

    Iterates _AXI_CHANNEL_SIGNALS in channel order (same as interface), separates
    signals by direction using _MASTER_DRIVES_AXI, translates width tokens to
    fully-qualified ni_params_pkg::*_DFLT references.
    """
    req_fields: list[tuple[str, str]] = []
    rsp_fields: list[tuple[str, str]] = []
    for ch in channels:
        for sig_name, width_tok in _AXI_CHANNEL_SIGNALS[ch]:
            sv_width = _IFACE_WIDTH_TO_STRUCT[width_tok]
            if sig_name in _MASTER_DRIVES_AXI:
                req_fields.append((sig_name, sv_width))
            else:
                rsp_fields.append((sig_name, sv_width))

    def _fields_to_sv(fields: list[tuple[str, str]]) -> list[str]:
        if not fields:
            return []
        width_col = max(len(w) for _, w in fields)
        return [
            f"    logic {w:<{width_col}} {name};" for name, w in fields
        ]

    out: list[str] = [
        "  // AXI packed-struct typedefs (coexist with axi4_intf; widths fixed-default).",
        "  typedef struct packed {",
    ]
    out.extend(_fields_to_sv(req_fields))
    out.append("  } axi_req_t;")
    out.append("  typedef struct packed {")
    out.extend(_fields_to_sv(rsp_fields))
    out.append("  } axi_rsp_t;")
    return out


def _emit_interfaces_from_handshake_schema(
    interfaces_doc: dict, constants: dict
) -> list[str]:
    """Emit every interface declared in interface_handshake.json.

    One blank line separates consecutive interface blocks.
    """
    out: list[str] = []
    items = list(interfaces_doc["interfaces"].items())
    for i, (name, iface_spec) in enumerate(items):
        kind = iface_spec["kind"]
        if kind == "axi4":
            out.extend(_emit_axi4_intf(name, iface_spec, constants))
        elif kind == "noc_link":
            out.extend(_emit_noc_intf(name, iface_spec, constants))
        else:  # handshake_schema already validates the kind enum.
            raise ValueError(f"sv_signals: unsupported interface kind {kind!r}")
        # Trailing blank line after every interface (including the last) so the
        # caller can append ``endif`` directly.
        out.append("")
    return out


def emit(signals_json: Path, spec_version: str) -> str:
    """Return SV package body (no provenance banner -- caller prepends it).

    Reset-constant section is derived from ni_signals.json. The interface
    blocks after ``endpackage`` are derived from interface_handshake.json
    (parameter defaults sourced from constants.yaml).
    """
    spec = load_doc(signals_json)

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
    out.append("`timescale 1ns/1ps")
    out.append("")
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

    # Packed-struct typedefs go INSIDE the package so they are usable as
    # cross-module port types via "import ni_signals_pkg::*".
    constants = load_constants(SPECGEN_ROOT / "source" / "constants.yaml")
    interfaces_doc = load_interfaces(
        SPECGEN_ROOT / "source" / "interface_handshake.json", constants
    )
    axi_spec = interfaces_doc["interfaces"].get("axi4_intf", {})
    axi_channels: list[str] = axi_spec.get("channels", list(_AXI_CHANNEL_SIGNALS.keys()))
    out.extend(_emit_noc_structs())
    out.append("")
    out.extend(_emit_axi_structs(axi_channels))
    out.append("")

    out.append("endpackage")
    out.append("")

    # SV interface blocks live OUTSIDE the package (SV LRM: interface
    # declarations are top-level, not allowed inside packages).
    out.extend(_emit_interfaces_from_handshake_schema(interfaces_doc, constants))

    out.append("`endif // NI_SIGNALS_PKG_SVH")
    return "\n".join(out) + "\n"
