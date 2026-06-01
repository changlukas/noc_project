"""Unit tests for signals-domain resolvers (PP-7 + PP-9).

Signals reference symbols from two namespaces:
  - the interface's port_parameters (local: NUM_VC, ENABLE_AXI_PARITY,
    AXI_DATA_WIDTH, AXI_STRB_WIDTH, AXI_QOS_WIDTH, AXI_*USER_WIDTH)
  - packet domain (cross-domain: FLIT_WIDTH, AXI_ID_WIDTH, AXI_ADDR_WIDTH, ...)

Tests pull real interface/pin names from generated/ni_signals.json to
keep the suite aligned with the authored spec. PP-9 closed the AXI_*_WIDTH
namespace gap, so every pin in the spec must resolve to an int.
"""
from __future__ import annotations
import pytest
from pathlib import Path
from ni_spec.loader import load_doc
from ni_spec import constants as C
from ni_spec.exceptions import ExprNameError, FieldNotFoundError

SPECGEN_ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture(scope="module")
def signals_spec():
    return load_doc(SPECGEN_ROOT / "generated" / "json" / "ni_signals.json")


@pytest.fixture(scope="module")
def packet_spec():
    return load_doc(SPECGEN_ROOT / "generated" / "json" / "ni_packet.json")


# -- accessors -----------------------------------------------------

def test_signal_interfaces_lists_all_in_order(signals_spec):
    ifaces = C.signal_interfaces(signals_spec)
    # All 7 NI top-level interfaces present, in JSON declaration order.
    assert ifaces == [
        "AXI_SLAVE_PORT", "NOC_REQ_OUT", "NOC_RSP_IN", "CSR",
        "NOC_REQ_IN", "AXI_MASTER_PORT", "NOC_RSP_OUT",
    ]


def test_signal_interface_pins_handles_channeled_iface(signals_spec):
    """AXI_SLAVE_PORT pins live under channels[].signals[]; resolver must flatten."""
    pins = C.signal_interface_pins(signals_spec, "AXI_SLAVE_PORT")
    assert len(pins) > 0
    pin_names = {p["pin_name"] for p in pins}
    # AW channel sample
    assert "axi_awid_i"   in pin_names
    assert "axi_awaddr_i" in pin_names
    # W channel sample (proves multi-channel flattening)
    assert "axi_wdata_i"  in pin_names


def test_signal_interface_pins_handles_direct_iface(signals_spec):
    """NOC_REQ_OUT has direct signals[] (no channels)."""
    pins = C.signal_interface_pins(signals_spec, "NOC_REQ_OUT")
    pin_names = {p["pin_name"] for p in pins}
    assert pin_names == {"noc_req_valid_o", "noc_req_flit_o", "noc_req_credit_i"}


def test_signal_interface_pins_unknown_iface_raises(signals_spec):
    with pytest.raises(FieldNotFoundError):
        C.signal_interface_pins(signals_spec, "BOGUS_IFACE_NAME")


# -- pin width resolution -----------------------------------------

def test_pin_width_from_packet_field_widths(signals_spec, packet_spec):
    """axi_awid_i.width_param = AXI_ID_WIDTH; resolved from packet field_widths."""
    expected = packet_spec["flit"]["field_widths"]["AXI_ID_WIDTH"]
    actual = C.signal_pin_width(signals_spec, packet_spec,
                                "AXI_SLAVE_PORT", "axi_awid_i")
    assert actual == expected


def test_pin_width_cross_domain_flit_width(signals_spec, packet_spec):
    """noc_req_flit_o.width_param = FLIT_WIDTH — the cross-domain edge.

    FLIT_WIDTH is NOT in packet.flit.field_widths (PP-6 removed it as a
    stored value). The resolver must compute it via flit_width_resolved.
    """
    expected = C.flit_width_resolved(packet_spec)
    actual = C.signal_pin_width(signals_spec, packet_spec,
                                "NOC_REQ_OUT", "noc_req_flit_o")
    assert actual == expected
    # Sanity: this is the real composed width, not just the legacy default.
    assert actual == 402


def test_pin_width_from_interface_port_parameter(signals_spec, packet_spec):
    """noc_req_credit_i.width_param = NUM_VC; NUM_VC lives in port_parameters.

    NUM_VC is interface-local — not in packet field_widths. Tests namespace
    priority #3 (interface scope).
    """
    actual = C.signal_pin_width(signals_spec, packet_spec,
                                "NOC_REQ_OUT", "noc_req_credit_i")
    assert actual == 1  # NUM_VC default = 1


def test_pin_width_no_width_param_falls_back_to_width_expr(signals_spec, packet_spec):
    """noc_req_valid_o has width_param=null and width_expr='1' (literal);
    resolver returns the literal int from width_expr."""
    actual = C.signal_pin_width(signals_spec, packet_spec,
                                "NOC_REQ_OUT", "noc_req_valid_o")
    assert actual == 1


def test_pin_width_unknown_pin_raises(signals_spec, packet_spec):
    with pytest.raises(FieldNotFoundError):
        C.signal_pin_width(signals_spec, packet_spec,
                           "AXI_SLAVE_PORT", "definitely_not_a_pin_xyz")


def test_signal_eval_expr_bogus_symbol_raises(signals_spec, packet_spec):
    with pytest.raises(ExprNameError):
        C.signal_eval_expr(signals_spec, packet_spec,
                           "NOC_REQ_OUT", "BOGUS_UNKNOWN_SYMBOL")


def test_signal_eval_expr_arithmetic(signals_spec, packet_spec):
    """Arithmetic over packet symbols composes in signals namespace."""
    # AXI_ID_WIDTH + AXI_LEN_WIDTH = 8 + 8 = 16
    expected = (packet_spec["flit"]["field_widths"]["AXI_ID_WIDTH"]
                + packet_spec["flit"]["field_widths"]["AXI_LEN_WIDTH"])
    actual = C.signal_eval_expr(signals_spec, packet_spec,
                                "AXI_SLAVE_PORT", "AXI_ID_WIDTH + AXI_LEN_WIDTH")
    assert actual == expected


# -- AXI_*_WIDTH resolution via per-interface port_parameters (PP-9) -

# Expected widths for every AXI scalar symbol. These match the defaults
# carried by the merged namespace (signal_interface.md §Parameters +
# packet_format.md §1.2 Group 3 + AMBA IHI 0022 fixed AXI_QOS_WIDTH).
_EXPECTED_AXI_WIDTHS = {
    "AXI_DATA_WIDTH":    256,
    "AXI_STRB_WIDTH":    32,
    "AXI_QOS_WIDTH":     4,
    "AXI_AWUSER_WIDTH":  8,
    "AXI_WUSER_WIDTH":   8,
    "AXI_BUSER_WIDTH":   8,
    "AXI_ARUSER_WIDTH":  8,
    "AXI_RUSER_WIDTH":   8,
}

# (interface, pin) -> AXI_*_WIDTH symbol exercised. Picks representative pins
# from both AXI_SLAVE_PORT (host-facing inputs) and AXI_MASTER_PORT (outputs).
_AXI_WIDTH_PIN_SAMPLES = [
    # AXI_DATA_WIDTH
    ("AXI_SLAVE_PORT",  "axi_wdata_i",  "AXI_DATA_WIDTH"),
    ("AXI_MASTER_PORT", "axi_rdata_i",  "AXI_DATA_WIDTH"),
    # AXI_STRB_WIDTH
    ("AXI_SLAVE_PORT",  "axi_wstrb_i",  "AXI_STRB_WIDTH"),
    ("AXI_MASTER_PORT", "axi_wstrb_o",  "AXI_STRB_WIDTH"),
    # AXI_QOS_WIDTH (per-channel: AW/AR carry awqos/arqos)
    ("AXI_SLAVE_PORT",  "axi_awqos_i",  "AXI_QOS_WIDTH"),
    ("AXI_SLAVE_PORT",  "axi_arqos_i",  "AXI_QOS_WIDTH"),
    # AXI_*USER_WIDTH (per-channel per AMBA IHI 0022)
    ("AXI_SLAVE_PORT",  "axi_awuser_i", "AXI_AWUSER_WIDTH"),
    ("AXI_SLAVE_PORT",  "axi_wuser_i",  "AXI_WUSER_WIDTH"),
    ("AXI_SLAVE_PORT",  "axi_buser_o",  "AXI_BUSER_WIDTH"),
    ("AXI_SLAVE_PORT",  "axi_aruser_i", "AXI_ARUSER_WIDTH"),
    ("AXI_SLAVE_PORT",  "axi_ruser_o",  "AXI_RUSER_WIDTH"),
]


@pytest.mark.parametrize("iface,pin,symbol", _AXI_WIDTH_PIN_SAMPLES,
                         ids=[f"{i}/{p}::{s}" for i, p, s in _AXI_WIDTH_PIN_SAMPLES])
def test_axi_width_pin_resolves(signals_spec, packet_spec, iface, pin, symbol):
    """Each AXI scalar-width symbol resolves via the interface's port_parameters.

    PP-9 closed the namespace gap by adding AXI_DATA_WIDTH / AXI_STRB_WIDTH /
    AXI_QOS_WIDTH / AXI_*USER_WIDTH (8 symbols) as port_parameters on both
    AXI_SLAVE_PORT and AXI_MASTER_PORT. The resolver must answer with the
    expected width without falling through to any legacy stored default.
    """
    assert symbol in _EXPECTED_AXI_WIDTHS, f"missing expected width for {symbol}"
    actual = C.signal_pin_width(signals_spec, packet_spec, iface, pin)
    assert actual == _EXPECTED_AXI_WIDTHS[symbol]


@pytest.mark.parametrize("iface_name", ["AXI_SLAVE_PORT", "AXI_MASTER_PORT"])
def test_axi_interface_carries_eight_width_port_parameters(signals_spec, iface_name):
    """Both AXI ports MUST declare the 8 AXI_*_WIDTH symbols as port_parameters.

    Convention is per-AMBA: each width is an instance-level knob of the AXI
    port, not a global NoC parameter (which would conflate two distinct
    width domains — see _build_params_namespace docstring).
    """
    iface = next(i for i in signals_spec["interfaces"] if i["name"] == iface_name)
    pp_names = {p["name"] for p in iface.get("port_parameters", [])}
    for sym in _EXPECTED_AXI_WIDTHS:
        assert sym in pp_names, (
            f"{iface_name} is missing AXI scalar-width port_parameter {sym}"
        )


def test_every_pin_resolves_to_int(signals_spec, packet_spec):
    """PP-9 hard invariant: every pin in ni_signals.json MUST resolve to an int.

    The transition-guard skip that tolerated ExprNameError on AXI_*_WIDTH
    symbols is removed — the resolver now answers every pin via the merged
    namespace. Any ExprNameError here is a real spec bug.
    """
    failures: list[str] = []
    checked = 0
    for iface_name in C.signal_interfaces(signals_spec):
        for sig in C.signal_interface_pins(signals_spec, iface_name):
            pin = sig.get("pin_name")
            if not pin:
                continue
            try:
                w = C.signal_pin_width(signals_spec, packet_spec, iface_name, pin)
                assert isinstance(w, int) and w >= 1, f"{iface_name}/{pin} -> {w!r}"
                checked += 1
            except ExprNameError as e:  # noqa: PERF203
                failures.append(f"{iface_name}/{pin}: ExprNameError({e})")
    assert not failures, "pins failed to resolve:\n  " + "\n  ".join(failures)
    # All 5 channels x ~7-11 sigs x 2 AXI ports + NoC + CSR.
    assert checked >= 100, f"only checked {checked} pins; expected ≥ 100"
