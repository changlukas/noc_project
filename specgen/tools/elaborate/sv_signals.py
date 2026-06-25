"""SV emitter for signals domain.

Produces rtl_pkg/ni_signals_pkg.sv: reset constants + the packed-struct
typedefs (noc_chan_t / axi_req_t / axi_rsp_t) used on every wrap/fabric/tb
port. Uses localparam int unsigned for reset constants (design doc 6.2).

SV interface blocks (axi4_intf / noc_intf) were removed in the FlooNoC struct
refactor — the field/channel order of the structs is still derived from
``source/interface_handshake.json`` + constants.yaml, but no interface is
emitted.
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
    """Emit noc_chan_t typedef line (in-package).

    The credit typedef is per-topology (width depends on num_vc) and lives in
    noc_types_pkg_vc{N}.sv, generated via --domain noc_types --num-vc N.
    """
    out: list[str] = [
        "  // NoC link packed-struct typedefs (replaced noc_intf; widths fixed-default).",
        "  typedef struct packed {",
        "    logic                                            valid;",
        "    logic [ni_params_pkg::NOC_FLIT_WIDTH_DFLT-1:0] flit;",
        "  } noc_chan_t;",
    ]
    return out


def emit_noc_types_pkg(num_vc: int, spec_version: str) -> str:
    """Return SV package body for noc_types_pkg with credit width baked in.

    Package name is fixed 'noc_types_pkg' regardless of num_vc.
    Width is a literal [num_vc-1:0] (no unresolved symbols).
    Caller prepends the provenance banner.
    """
    width = f"[{num_vc - 1}:0]"
    guard = "NOC_TYPES_PKG_SVH"
    out: list[str] = [
        "`timescale 1ns/1ps",
        "",
        f"`ifndef {guard}",
        f"`define {guard}",
        "",
        "package noc_types_pkg;",
        "",
        f"  // noc_credit_t: credit width baked at generate time (num_vc={num_vc}).",
        "  // Use noc_types_pkg_vc{N}.sv matching your topology's NUM_VC.",
        "  typedef struct packed {",
        f"    logic {width} credit;",
        "  } noc_credit_t;",
        "",
        "endpackage",
        "",
        f"`endif // {guard}",
    ]
    return "\n".join(out) + "\n"


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
        "  // AXI packed-struct typedefs (replaced axi4_intf; widths fixed-default).",
        "  typedef struct packed {",
    ]
    out.extend(_fields_to_sv(req_fields))
    out.append("  } axi_req_t;")
    out.append("  typedef struct packed {")
    out.extend(_fields_to_sv(rsp_fields))
    out.append("  } axi_rsp_t;")
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

    # SV interfaces (axi4_intf / noc_intf) were removed in the FlooNoC struct
    # refactor: the wraps + fabric + tb now use packed-struct typedefs
    # (noc_chan_t / axi_req_t / axi_rsp_t above) on every port. The
    # interface_handshake.json source still drives the struct field/channel
    # order via load_interfaces() above; only the SV interface emission is gone.
    out.append("`endif // NI_SIGNALS_PKG_SVH")
    return "\n".join(out) + "\n"
