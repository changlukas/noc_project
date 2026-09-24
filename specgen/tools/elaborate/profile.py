"""Resolve an ID-width profile into consistent packet and parameter sources."""
from copy import deepcopy
import json
from pathlib import Path
import yaml
from ni_spec import constants as C
from tools.elaborate import cpp_packet, cpp_params, cpp_signals, sv_packet, sv_params, sv_signals


def resolve(packet, constants, noc_id_width):
    if type(noc_id_width) is not int or not 1 <= noc_id_width <= 8:
        raise ValueError("noc_id_width must be in [1, 8] for the model/DPI ID carrier")
    previous_width = packet["flit"]["field_widths"]["NOC_ID_WIDTH"]
    packet, constants = deepcopy(packet), deepcopy(constants)
    packet["flit"]["field_widths"]["NOC_ID_WIDTH"] = noc_id_width
    constants["axi"]["NOC_ID_WIDTH"]["default"] = noc_id_width
    # The reference NSU supports collapse or full passthrough, not a partial remap.
    nsu_ids = constants["nsu"]["META_BUFFER_MAX_UNIQUE_IDS"]
    nsu_ids["allowed"] = [1, 1 << noc_id_width]
    nsu_ids["description"] = nsu_ids["description"].replace(
        "8 passes through", f"{1 << noc_id_width} passes through")
    if nsu_ids["default"] != 1:
        nsu_ids["default"] = 1 << noc_id_width
    for channel in packet["flit"]["payload_channels"]:
        channel["payload_width"] = sum(C.payload_field_width(packet, channel["name"], field["name"])
                                       for field in channel["fields"])
    for network in ("REQ", "RSP", "DAT"):
        param = constants["noc"][network + "_FLIT_WIDTH"]
        param["default"] = C.network_flit_width_resolved(packet, network)
        if noc_id_width != previous_width:
            param["description"] = f"{network} flit width derived from the resolved packet profile"
    return packet, constants


def emit(root, out, constants, noc_id_width):
    root, out = Path(root), Path(out)
    packet = json.loads((root / "specgen/generated/json/ni_packet.json").read_text())
    packet, constants = resolve(packet, constants, noc_id_width)
    sources = out / "profile_sources"
    sources.mkdir(parents=True, exist_ok=True)
    packet_path = sources / "ni_packet.json"
    packet_path.write_text(json.dumps(packet, indent=2) + "\n")
    (sources / "ni_signals.json").write_bytes((root / "specgen/generated/json/ni_signals.json").read_bytes())
    constants_path = out / "constants.yml"
    constants_path.write_text(yaml.safe_dump(constants, sort_keys=False))
    for language, domain, emitter, name in (
        ("sv", "packet", sv_packet, "ni_flit_pkg.sv"),
        ("cpp", "packet", cpp_packet, "ni_flit_constants.h"),
        ("sv", "params", sv_params, "ni_params_pkg.sv"),
        ("cpp", "params", cpp_params, "ni_params.h"),
        ("sv", "signals", sv_signals, "ni_signals_pkg.sv"),
        ("cpp", "signals", cpp_signals, "ni_signals.h"),
    ):
        source = constants_path if domain == "params" else sources / ("ni_packet.json" if domain == "packet" else "ni_signals.json")
        target = out / "repo/specgen/generated" / language / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("// Generated from profile_sources and constants.yml\n" + emitter.emit(source, "profile"))
    return constants
