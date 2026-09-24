import json
from pathlib import Path
import pytest
import yaml
from tools.elaborate.profile import resolve
from ni_spec import constants as C

ROOT = Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("width", [1, 3, 4, 8])
def test_id_profile_layout(width):
    packet = json.loads((ROOT / "generated/json/ni_packet.json").read_text())
    constants = yaml.safe_load((ROOT / "source/constants.yaml").read_text())
    resolved, params = resolve(packet, constants, width)
    assert packet["flit"]["field_widths"]["NOC_ID_WIDTH"] == 3
    for channel, field in [("AW", "awid"), ("AR", "arid"), ("B", "bid"),
                           ("NARROW_R", "rid"), ("DATA_R", "rid")]:
        assert C.payload_field_width(resolved, channel, field) == width
        ch = next(x for x in resolved["flit"]["payload_channels"] if x["name"] == channel)
        assert ch["payload_width"] == sum(C.payload_field_width(resolved, channel, f["name"]) for f in ch["fields"])
    assert params["noc"]["REQ_FLIT_WIDTH"]["default"] == 133 + width
    assert params["noc"]["RSP_FLIT_WIDTH"]["default"] == 123 + width
    assert params["noc"]["DAT_FLIT_WIDTH"]["default"] == 633
    assert params["nsu"]["META_BUFFER_MAX_UNIQUE_IDS"]["default"] == 1 << width
    if width == 3:
        assert resolved == packet
        assert params == constants

@pytest.mark.parametrize("width", [0, 9, True, 3.5])
def test_invalid_id_width(width):
    with pytest.raises(ValueError):
        resolve({}, {}, width)
