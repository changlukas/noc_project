"""Tests for check_flit_width_matches_packet — L2 invariant binding constants.yaml
noc.FLIT_WIDTH to the packet-domain HEADER_WIDTH + max(payload_width)."""
from __future__ import annotations
import copy
from pathlib import Path

import pytest

from ni_spec.loader import load_doc
from ni_spec.handshake_schema import load_constants
from ni_spec.invariants import check_flit_width_matches_packet

SPECGEN_ROOT = Path(__file__).resolve().parent.parent
PACKET_JSON = SPECGEN_ROOT / "generated" / "json" / "ni_packet.json"
CONSTANTS_YAML = SPECGEN_ROOT / "source" / "constants.yaml"


@pytest.fixture
def packet_spec():
    return load_doc(PACKET_JSON)


@pytest.fixture
def constants():
    return load_constants(CONSTANTS_YAML)


def test_default_config_no_errors(packet_spec, constants):
    """Committed constants.yaml noc.FLIT_WIDTH must match the packet spec's
    resolved HEADER_WIDTH + max(payload_width) with no ERROR."""
    issues = check_flit_width_matches_packet(packet_spec, constants)
    errors = [i for i in issues if i.severity == "ERROR"]
    assert not errors, f"unexpected errors: {[i.message for i in errors]}"


def test_drifted_yaml_value_fires_error(packet_spec, constants):
    """noc.FLIT_WIDTH hand-edited out of sync with the packet spec must fire
    L2-FLIT-WIDTH-SRC ERROR (the two-source drift this invariant exists to catch)."""
    bad = copy.deepcopy(constants)
    bad["noc"]["FLIT_WIDTH"]["default"] = 396
    issues = check_flit_width_matches_packet(packet_spec, bad)
    errors = [i for i in issues if i.severity == "ERROR" and i.check == "L2-FLIT-WIDTH-SRC"]
    assert errors, "expected an L2-FLIT-WIDTH-SRC ERROR for drifted FLIT_WIDTH but got none"
    assert any("FLIT_WIDTH" in i.message for i in errors)
