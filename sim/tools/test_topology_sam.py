"""Semantic checks for the generated elaboration-time SAM package."""

import re

import pytest
import yaml

import gen_tb_top


ROOT = gen_tb_top.ROOT


def _package(name):
    topo = yaml.safe_load((ROOT / "sim" / "configs" / f"{name}.yml").read_text())
    return gen_tb_top.emit_topology_pkg(topo)


def _rule(pkg, generated_index):
    pattern = (
        rf"{generated_index}: '\{{idx: '\{{dst_id: "
        rf"ni_flit_pkg::DST_ID_WIDTH'\((?P<dst_id>\d+)\), "
        rf"dst_port_id: ni_flit_pkg::DST_PORT_ID_WIDTH'\((?P<port>\d+)\), "
        rf"is_data: 1'b(?P<is_data>[01]), "
        rf"collective_en: 1'b(?P<collective>[01]), "
        rf"mask_x: '\{{offset: SAM_MASK_SEL_WIDTH'\((?P<x_offset>\d+)\), "
        rf"len: SAM_MASK_SEL_WIDTH'\((?P<x_len>\d+)\)\}}, "
        rf"mask_y: '\{{offset: SAM_MASK_SEL_WIDTH'\((?P<y_offset>\d+)\), "
        rf"len: SAM_MASK_SEL_WIDTH'\((?P<y_len>\d+)\)\}}\}}, "
        rf"start_addr: ADDR_WIDTH'\(64'h(?P<start>[0-9A-F]+)\), "
        rf"end_addr: ADDR_WIDTH'\(64'h(?P<end>[0-9A-F]+)\)\}}")
    match = re.search(pattern, pkg)
    assert match, f"SAM[{generated_index}] not found"
    return {key: int(value, 16) if key in ("start", "end") else int(value)
            for key, value in match.groupdict().items()}


@pytest.mark.parametrize(
    "name,rule_count,first,memory_index,memory_dst,memory_start,config_index,selector_len",
    [
        ("mesh_2x2", 8, 7, 7, 0, 0, 3, 1),
        ("mesh_4x4", 32, 31, 25, 0x12, 0x600000000, 15, 2),
    ],
)
def test_generated_sam_preserves_authored_metadata_and_reverses_priority(
        name, rule_count, first, memory_index, memory_dst, memory_start, config_index,
        selector_len):
    pkg = _package(name)

    assert f"localparam int unsigned SAM_NUM_RULES = {rule_count};" in pkg
    assert "localparam int unsigned ADDR_WIDTH = ni_flit_pkg::AXI_ADDR_WIDTH;" in pkg
    assert "typedef struct packed" in pkg
    assert "sam_mask_sel_t mask_x;" in pkg
    assert "sam_mask_sel_t mask_y;" in pkg
    assert "base_id" not in pkg

    first_rule = _rule(pkg, first)
    assert first_rule == {
        "dst_id": 0,
        "port": 0,
        "is_data": 1,
        "collective": 1,
        "x_offset": 32,
        "x_len": selector_len,
        "y_offset": 32 + selector_len,
        "y_len": selector_len,
        "start": 0,
        "end": 0x2000000,
    }

    memory_rule = _rule(pkg, memory_index)
    assert memory_rule["dst_id"] == memory_dst
    assert memory_rule["port"] == 0
    assert memory_rule["is_data"] == 1
    assert memory_rule["start"] == memory_start
    assert memory_rule["end"] == memory_start + 0x2000000

    config_rule = _rule(pkg, config_index)
    assert config_rule["dst_id"] == 0
    assert config_rule["is_data"] == 0
    assert config_rule["start"] == 0x2000000
    assert config_rule["end"] == 0x2001000


@pytest.mark.parametrize("collective_value", [False, None])
def test_generated_sam_is_deterministic_and_unicast_selectors_are_zero(collective_value):
    topo = yaml.safe_load((ROOT / "sim" / "configs" / "mesh_2x2.yml").read_text())
    if collective_value is None:
        del topo["endpoints"][0]["addr_range"][0]["en_collective"]
    else:
        topo["endpoints"][0]["addr_range"][0]["en_collective"] = collective_value
    pkg_a = gen_tb_top.emit_topology_pkg(topo)
    pkg_b = gen_tb_top.emit_topology_pkg(topo)

    assert pkg_a == pkg_b
    rule = _rule(pkg_a, 7)
    assert rule["collective"] == 0
    assert rule["x_offset"] == rule["x_len"] == 0
    assert rule["y_offset"] == rule["y_len"] == 0


@pytest.mark.parametrize(
    "range_update, message",
    [
        ({"size": 0}, "positive and 4 KB aligned"),
        ({"size": -0x1000}, "positive and 4 KB aligned"),
        ({"base": 1}, "base and stride must be 4 KB aligned"),
        ({"stride": 1}, "base and stride must be 4 KB aligned"),
        ({"base": 1 << 48}, "does not fit"),
    ],
)
def test_generated_sam_rejects_invalid_ranges(range_update, message):
    topo = yaml.safe_load((ROOT / "sim" / "configs" / "mesh_2x2.yml").read_text())
    topo["endpoints"][0]["addr_range"][0].update(range_update)

    with pytest.raises(SystemExit, match=message):
        gen_tb_top.emit_topology_pkg(topo)


def test_collective_request_with_no_representable_space_layout_fails():
    topo = yaml.safe_load((ROOT / "sim" / "configs" / "mesh_2x2.yml").read_text())
    topo["endpoints"][0]["addr_range"][0]["stride"] = 0x30000000

    with pytest.raises(SystemExit, match="collective layout"):
        gen_tb_top.emit_topology_pkg(topo)
