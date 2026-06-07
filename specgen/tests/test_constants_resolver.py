"""Unit tests for pure-parameterization elaborator helpers."""
from __future__ import annotations
import pytest
from pathlib import Path
from ni_spec.loader import load_doc
from ni_spec import constants as C
from ni_spec.exceptions import (
    ExprSyntaxError, ExprNameError, ExprNotAllowedError, FieldNotFoundError,
)

SPECGEN_ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture
def packet_spec():
    return load_doc(SPECGEN_ROOT / "generated" / "json" / "ni_packet.json")


# -- packet_eval_expr -----------------------------------------------
def test_eval_integer_literal(packet_spec):
    assert C.packet_eval_expr(packet_spec, "0") == 0
    assert C.packet_eval_expr(packet_spec, "42") == 42


def test_eval_single_symbol(packet_spec):
    assert C.packet_eval_expr(packet_spec, "X_WIDTH") == 4


def test_eval_addition(packet_spec):
    assert C.packet_eval_expr(packet_spec, "X_WIDTH + Y_WIDTH") == 8


def test_eval_subtraction_and_mul(packet_spec):
    assert C.packet_eval_expr(packet_spec, "X_WIDTH - 1") == 3
    assert C.packet_eval_expr(packet_spec, "X_WIDTH * 2") == 8


def test_eval_unknown_symbol(packet_spec):
    with pytest.raises(ExprNameError, match="MISSING_WIDTH"):
        C.packet_eval_expr(packet_spec, "MISSING_WIDTH")


def test_eval_forbidden_function_call(packet_spec):
    with pytest.raises(ExprNotAllowedError):
        C.packet_eval_expr(packet_spec, "max(X_WIDTH, Y_WIDTH)")


def test_eval_forbidden_attribute(packet_spec):
    with pytest.raises(ExprNotAllowedError):
        C.packet_eval_expr(packet_spec, "X_WIDTH.bit_length")


def test_eval_forbidden_subscript(packet_spec):
    with pytest.raises(ExprNotAllowedError):
        C.packet_eval_expr(packet_spec, "X_WIDTH[0]")


def test_eval_derived_literal_rejected(packet_spec):
    """packet_eval_expr rejects 'derived' -- must be handled by payload_field_width."""
    with pytest.raises(ExprNotAllowedError, match="derived"):
        C.packet_eval_expr(packet_spec, "derived")


def test_eval_syntax_error(packet_spec):
    with pytest.raises(ExprSyntaxError):
        C.packet_eval_expr(packet_spec, "X_WIDTH +")


# -- header_field_width / position / enabled ------------------------
def test_header_field_width_basic(packet_spec):
    assert C.header_field_width(packet_spec, "axi_ch") == 3


def test_header_field_width_expression(packet_spec):
    assert C.header_field_width(packet_spec, "src_id") == 8


def test_header_field_position_cumulative(packet_spec):
    # Post fixed-56b refactor (noc_qos enabled, width=4): all positions shift +4.
    assert C.header_field_position(packet_spec, "axi_ch") == (4, 6)
    assert C.header_field_position(packet_spec, "src_id") == (7, 14)


def test_header_field_position_disabled_still_positioned(packet_spec):
    # rsvd is the lone disabled (derived padding) field; sits at the end.
    pos = C.header_field_position(packet_spec, "rsvd")
    assert pos is not None
    # width=2 (HEADER_TOTAL_WIDTH 56 - sum of enabled 54), so MSB-LSB == 1.
    assert pos[1] - pos[0] == 1


def test_header_field_enabled(packet_spec):
    assert C.header_field_enabled(packet_spec, "src_id") is True
    assert C.header_field_enabled(packet_spec, "rsvd") is False


def test_header_field_not_found(packet_spec):
    with pytest.raises(FieldNotFoundError):
        C.header_field_width(packet_spec, "nonexistent_field")


# -- payload_field_width / position (incl. "derived") ---------------
def test_payload_field_width_basic(packet_spec):
    assert C.payload_field_width(packet_spec, "AW", "awid") == 8


def test_payload_field_width_derived(packet_spec):
    w = C.payload_field_width(packet_spec, "AW", "aw_rsvd")
    ch = next(c for c in packet_spec["flit"]["payload_channels"] if c["name"] == "AW")
    other_sum = sum(
        C.payload_field_width(packet_spec, "AW", f["name"])
        for f in ch["fields"] if f["name"] != "aw_rsvd"
    )
    assert w == C.payload_channel_width(packet_spec, "AW") - other_sum


def test_payload_field_position(packet_spec):
    assert C.payload_field_position(packet_spec, "AW", "awid") == (0, 7)


# -- payload field positions (added with codegen extension) -----------
def test_payload_field_position_aw(packet_spec):
    assert C.payload_field_position(packet_spec, "AW", "awid") == (0, 7)
    assert C.payload_field_position(packet_spec, "AW", "awaddr") == (8, 71)


def test_payload_field_position_w_with_reorder(packet_spec):
    # wstrb comes before wdata per ni_packet.json layout
    assert C.payload_field_position(packet_spec, "W", "wlast") == (0, 0)
    assert C.payload_field_position(packet_spec, "W", "wuser") == (1, 8)
    assert C.payload_field_position(packet_spec, "W", "wstrb") == (9, 40)
    assert C.payload_field_position(packet_spec, "W", "wdata") == (41, 296)


def test_payload_field_position_b_after_rsvd_mc_status_removed(packet_spec):
    assert C.payload_field_position(packet_spec, "B", "bid")   == (0, 7)
    assert C.payload_field_position(packet_spec, "B", "bresp") == (8, 9)
    assert C.payload_field_position(packet_spec, "B", "buser") == (10, 17)
    # b_rsvd absorbs the removed rsvd_mc_status (46-bit derived)
    assert C.payload_field_position(packet_spec, "B", "b_rsvd") == (18, 63)


# -- derived totals -------------------------------------------------
# PP-6: flit.derived no longer exists in JSON. These tests now assert
# self-consistency between the resolved helpers and the symbolic source
# (field_widths + width_param + per-channel payload_width). When the spec
# numbers change, these assertions remain valid by construction.

def test_header_width_is_sum_of_field_widths(packet_spec):
    """header_width_resolved == sum of every header field's resolved width."""
    expected = sum(C.header_field_width(packet_spec, f["name"])
                   for f in packet_spec["flit"]["header_fields"])
    assert C.header_width_resolved(packet_spec) == expected


def test_payload_width_is_max_of_channel_widths(packet_spec):
    """payload_width_resolved == max authored payload_width across channels."""
    expected = max(C.payload_channel_width(packet_spec, ch["name"])
                   for ch in packet_spec["flit"]["payload_channels"])
    assert C.payload_width_resolved(packet_spec) == expected


def test_flit_width_is_header_plus_payload(packet_spec):
    """flit_width_resolved == header_width_resolved + payload_width_resolved."""
    assert (C.flit_width_resolved(packet_spec)
            == C.header_width_resolved(packet_spec)
               + C.payload_width_resolved(packet_spec))


def test_link_width_includes_valid_and_credit(packet_spec):
    """LINK_WIDTH == FLIT_WIDTH + 1 (valid) + NUM_VC (per-VC credit return).

    The forward bundle is valid + flit; the reverse bundle is NUM_VC credit
    bits. NUM_VC defaults to 1 (single-VC) when not present in field_widths.
    Per ni_signals.json NOC_REQ_OUT / NOC_RSP_OUT signal definitions.
    """
    fw = packet_spec["flit"].get("field_widths", {})
    num_vc = int(fw.get("NUM_VC", 1))
    assert (C.link_width_resolved(packet_spec)
            == C.flit_width_resolved(packet_spec) + 1 + num_vc)
