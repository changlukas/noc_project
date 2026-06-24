"""Tests for check_mesh_within_flit — L2 invariant binding mesh dims to flit capacity."""
from __future__ import annotations
import copy
from pathlib import Path

import pytest

from ni_spec.loader import load_doc
from ni_spec.handshake_schema import load_constants
from ni_spec.invariants import check_mesh_within_flit

SPECGEN_ROOT = Path(__file__).resolve().parent.parent
PACKET_JSON = SPECGEN_ROOT / "generated" / "json" / "ni_packet.json"
CONSTANTS_YAML = SPECGEN_ROOT / "source" / "constants.yaml"


@pytest.fixture
def packet_spec():
    return load_doc(PACKET_JSON)


@pytest.fixture
def constants():
    return load_constants(CONSTANTS_YAML)


# ---------------------------------------------------------------------------
# Valid: default config passes with no ERROR
# ---------------------------------------------------------------------------

def test_default_config_no_errors(packet_spec, constants):
    """Default MESH_X_DIM=4, MESH_Y_DIM=4, NUM_VC=1 must produce no ERROR."""
    issues = check_mesh_within_flit(packet_spec, constants)
    errors = [i for i in issues if i.severity == "ERROR"]
    assert not errors, f"unexpected errors: {[i.message for i in errors]}"


# ---------------------------------------------------------------------------
# Over-limit: MESH_X_DIM=32 > 2^X_WIDTH=16 must fire L2-MESH-FLIT ERROR
# ---------------------------------------------------------------------------

def _make_constants_with_noc_override(base_constants, **overrides):
    """Return a deep-copied constants dict with noc param defaults patched."""
    c = copy.deepcopy(base_constants)
    for key, value in overrides.items():
        c["noc"][key]["default"] = value
    return c


def test_over_limit_mesh_x_dim_fires_error(packet_spec, constants):
    """MESH_X_DIM=32 > 2^X_WIDTH(4)=16 must produce an L2-MESH-FLIT ERROR."""
    bad = _make_constants_with_noc_override(constants, MESH_X_DIM=32)
    issues = check_mesh_within_flit(packet_spec, bad)
    errors = [i for i in issues if i.severity == "ERROR" and i.check == "L2-MESH-FLIT"]
    assert errors, "expected an L2-MESH-FLIT ERROR for MESH_X_DIM=32 but got none"
    assert any("MESH_X_DIM" in i.message for i in errors)


def test_over_limit_mesh_y_dim_fires_error(packet_spec, constants):
    """MESH_Y_DIM=32 > 2^Y_WIDTH(4)=16 must produce an L2-MESH-FLIT ERROR."""
    bad = _make_constants_with_noc_override(constants, MESH_Y_DIM=32)
    issues = check_mesh_within_flit(packet_spec, bad)
    errors = [i for i in issues if i.severity == "ERROR" and i.check == "L2-MESH-FLIT"]
    assert errors, "expected an L2-MESH-FLIT ERROR for MESH_Y_DIM=32 but got none"
    assert any("MESH_Y_DIM" in i.message for i in errors)


def test_over_limit_nvc_fires_error(packet_spec, constants):
    """NUM_VC=16 > 2^VC_ID_WIDTH(3)=8 must produce an L2-MESH-FLIT ERROR."""
    bad = _make_constants_with_noc_override(constants, NUM_VC=16)
    issues = check_mesh_within_flit(packet_spec, bad)
    errors = [i for i in issues if i.severity == "ERROR" and i.check == "L2-MESH-FLIT"]
    assert errors, "expected an L2-MESH-FLIT ERROR for NUM_VC=16 but got none"
    assert any("NOC_NUM_VC" in i.message for i in errors)
