import glob
import os

import pytest

import address_map
import gen_test_patterns as g


def test_axi_widths_follow_constants_ssot():
    w = g.axi_widths()
    assert w == {"id": 8, "addr": 64, "data": 256}


def test_encode_write_beats_full_width_addr_in_data():
    # DW=256 -> 32 bytes/beat. One full-width beat at 0x1000.
    beats = g.encode_write_beats(0x1000, axi_size=5, axi_len=0, data_width=256)
    assert len(beats) == 1
    data_hex, strb_hex, user = beats[0].split()
    assert user == "0"
    assert strb_hex == "0x" + "f" * 8              # 32 lanes all active
    # byte j (little-endian) == (0x1000 + j) & 0xff
    data = int(data_hex, 16)
    for j in range(32):
        assert (data >> (8 * j)) & 0xFF == (0x1000 + j) & 0xFF


def test_encode_write_beats_multibeat_incr():
    beats = g.encode_write_beats(0x2000, axi_size=5, axi_len=3, data_width=256)
    assert len(beats) == 4                          # len+1 beats
    # beat 2 starts at 0x2000 + 2*32
    data = int(beats[2].split()[0], 16)
    assert data & 0xFF == (0x2000 + 2 * 32) & 0xFF


def test_encode_write_beats_rejects_oversize():
    import pytest
    with pytest.raises(ValueError):
        g.encode_write_beats(0x1000, axi_size=6, axi_len=0, data_width=256)  # 64B > 32B bus


def _parse_write(path):
    """Parse a write.txt back into txns. Mirrors axi_file_master.parse_write field order."""
    toks = open(path).read().split("\n")
    it = iter([t for t in toks if t != ""])
    txns = []
    while True:
        try:
            axid = int(next(it))
        except StopIteration:
            break
        addr = int(next(it), 16)
        length = int(next(it)); size = int(next(it)); burst = int(next(it))
        for _ in range(6):  # lock cache prot qos region atop
            next(it)
        next(it)            # user
        beats = [next(it) for _ in range(length + 1)]
        txns.append({"id": axid, "addr": addr, "len": length, "size": size,
                     "burst": burst, "beats": beats})
    return txns


def _uniform_topology_yaml(name, x_dim, y_dim, num_vc=1, tile_size=0x100000000):
    """New packed-address_map topology YAML text, every tile the same size.

    Builds a temp topology YAML in the packed `tiles:` format for the test.
    """
    tiles = "\n".join(
        f"    - {{ x: {x}, y: {y}, size: {tile_size:#x} }}"
        for y in range(y_dim) for x in range(x_dim)
    )
    return (
        f"topology: {{ name: {name}, x_dim: {x_dim}, y_dim: {y_dim}, num_vc: {num_vc} }}\n"
        f"address_map:\n  tiles:\n{tiles}\n"
    )


def test_emit_file_master_node_format_and_partition(tmp_path):
    d = str(tmp_path / "node0")
    bases = {1: 0x100000000}
    g.emit_file_master_node(d, src_idx=0, dst_cids=[1, 1], n_nodes=16,
                            base_local=0x1000, region_bytes=0x40000,
                            axi_size=5, axi_len=0, data_width=256, bases=bases)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert len(w) == 2
    for t in w:
        assert t["burst"] == 1 and t["size"] == 5 and t["len"] == 0
        assert (t["addr"] >> 32) == 1               # dst tile in addr[63:32]
        assert len(t["beats"]) == 1
    assert w[0]["addr"] != w[1]["addr"]             # disjoint offsets
    rlines = [l for l in open(os.path.join(d, "read.txt")).read().split("\n") if l != ""]
    assert len(rlines) == 2 * 11                    # 11 ax fields, no atop, no beats


def test_emit_file_master_node_addr_from_bases_dict(tmp_path):
    """addr = bases[dst_cid] + local_off -- dst coord_id 0x12, base 0x12*4GB,
    offset 0x40 -> 0x1200000040 (byte-for-byte the legacy dst_cid<<32 layout)."""
    d = str(tmp_path / "node0")
    bases = {0x12: 0x12 * 0x100000000}
    g.emit_file_master_node(d, src_idx=0, dst_cids=[0x12], n_nodes=1,
                            base_local=0x40, region_bytes=0x40000,
                            axi_size=5, axi_len=0, data_width=256, bases=bases)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x1200000040


def test_emit_file_master_node_arbitrary_base_from_bases_dict(tmp_path):
    d = str(tmp_path / "node0")
    bases = {0x12: 0x12 * 0x40000000}
    g.emit_file_master_node(d, src_idx=0, dst_cids=[0x12], n_nodes=1,
                            base_local=0x40, region_bytes=0x40000,
                            axi_size=5, axi_len=0, data_width=256, bases=bases)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x12 * 0x40000000 + 0x40


def test_load_topology_reads_packed_bases_from_address_map(tmp_path):
    topo_path = tmp_path / "t.yaml"
    topo_path.write_text(_uniform_topology_yaml("t", 4, 4, tile_size=0x40000000))
    nodes, x_dim, y_dim, bases = g._load_topology(str(topo_path))
    assert (x_dim, y_dim) == (4, 4)
    # packed in raster (y, x) order, matching _uniform_topology_yaml's emit order
    assert bases[g.coord_id(0, 0)] == 0
    assert bases[g.coord_id(1, 0)] == 0x40000000
    assert bases[g.coord_id(0, 1)] == 4 * 0x40000000


def test_load_topology_raises_when_address_map_missing(tmp_path):
    topo_path = tmp_path / "t.yaml"
    topo_path.write_text("topology: { name: t, x_dim: 4, y_dim: 4, num_vc: 1 }\n")
    with pytest.raises(ValueError, match="address_map.tiles"):
        g._load_topology(str(topo_path))


@pytest.mark.parametrize("x_dim,y_dim", [(1, 4), (4, 1), (1, 1)])
def test_check_mesh_capacity_rejects_dim_below_minimum(x_dim, y_dim):
    """Mesh dim minimum is 2 per dimension (1x1/1xN meshes are illegal)."""
    with pytest.raises(SystemExit, match="< 2"):
        g._check_mesh_capacity(x_dim, y_dim)


@pytest.mark.parametrize("x_dim,y_dim", [(1, 4), (4, 1), (1, 1)])
def test_check_flit_capacity_rejects_dim_below_minimum(x_dim, y_dim):
    """gen_tb_top's own topology-load gate must reject the same illegal dims."""
    import gen_tb_top as gt

    topo = {"topology": {"x_dim": x_dim, "y_dim": y_dim, "num_vc": 1}}
    with pytest.raises(SystemExit, match="< 2"):
        gt._check_flit_capacity(topo, "dummy_path.yaml")


def test_main_sources_tile_base_from_address_map(tmp_path):
    """End-to-end: main() threads the packed address_map base into the emitted address."""
    topo_path = tmp_path / "custom.yaml"
    topo_path.write_text(_uniform_topology_yaml("custom", 2, 2, tile_size=0x40000000))
    out = str(tmp_path / "scn")
    g.main(["--topology", str(topo_path), "--out", out,
            "--pattern", "neighbor", "--transactions-per-node", "1",
            "--size", "5", "--len", "0"])
    w = _parse_write(os.path.join(out, "node0", "write.txt"))
    # node0 = (x=0,y=0); neighbor wraps to (1,1) on a 2x2 mesh -> coord_id (1<<4)|1 = 0x11
    # raster order (0,0),(1,0),(0,1),(1,1) -> (1,1) is the 4th packed tile -> base 3*tile_size
    expected_base = 3 * 0x40000000
    assert expected_base <= w[0]["addr"] < expected_base + 0x40000000


PATTERNS = [
    ["--pattern", "neighbor"],
    ["--pattern", "transpose"],
    ["--pattern", "uniform_random", "--seed", "1"],
    ["--pattern", "hotspot", "--hotspot", "5", "--seed", "1"],
]


@pytest.mark.parametrize("pat", PATTERNS, ids=lambda p: p[1])
def test_main_file_master_all_patterns(tmp_path, pat):
    topo_path = tmp_path / "mesh_4x4.yaml"
    topo_path.write_text(_uniform_topology_yaml("mesh_4x4", 4, 4))
    out = str(tmp_path / "scn")
    g.main(["--topology", str(topo_path),
            "--out", out, "--transactions-per-node", "2",
            "--size", "5", "--len", "0"] + pat)
    nodes = sorted(glob.glob(os.path.join(out, "node*")))
    assert len(nodes) == 16
    for n in nodes:
        assert os.path.isfile(os.path.join(n, "write.txt"))
        assert os.path.isfile(os.path.join(n, "read.txt"))
    w = _parse_write(os.path.join(nodes[0], "write.txt"))
    assert len(w) == 2                              # transactions-per-node
    for t in w:
        assert t["burst"] == 1 and t["size"] == 5 and t["len"] == 0
        assert len(t["beats"]) == 1


def test_injection_mode_burst_hotspot_no_overflow_and_disjoint(tmp_path):
    """Root-cause regression: injection-mode transactions_per_node (200) with a
    non-zero burst footprint on a 16-node topology used to overflow the old fixed
    0x40000 window (ValueError). region_bytes is now auto-derived, so this must
    generate cleanly, and every hotspot-converging slot must stay disjoint."""
    topo_path = tmp_path / "mesh_4x4.yaml"
    topo_path.write_text(_uniform_topology_yaml("mesh_4x4", 4, 4))
    out = str(tmp_path / "scn")
    g.main(["--topology", str(topo_path), "--out", out,
            "--pattern", "hotspot", "--hotspot", "5", "--seed", "1",
            "--transactions-per-node", "200",
            "--size", "5", "--len", "3"])
    nodes = sorted(glob.glob(os.path.join(out, "node*")))
    assert len(nodes) == 16
    intervals = []
    for n in nodes:
        for t in _parse_write(os.path.join(n, "write.txt")):
            footprint = (t["len"] + 1) * (1 << t["size"])
            intervals.append((t["addr"], t["addr"] + footprint))
    intervals.sort()
    for (s0, e0), (s1, e1) in zip(intervals, intervals[1:]):
        assert e0 <= s1, f"overlapping slots: [{s0:#x},{e0:#x}) vs [{s1:#x},{e1:#x})"


# ---------------------------------------------------------------------------
# address_map.py: packing + validation (mirrors c_model SamTable::packed /
# SamTable::validate, nmu/addr_trans.hpp).
# ---------------------------------------------------------------------------

def test_address_map_pack_accumulates_bases_in_list_order():
    am = {"tiles": [
        {"x": 0, "y": 0, "size": 0x1000},
        {"x": 1, "y": 0, "size": 0x2000},
        {"x": 0, "y": 1, "size": 0x1000},
        {"x": 1, "y": 1, "size": 0x1000},
    ]}
    bases, entries = address_map.pack(am, x_dim=2, y_dim=2)
    assert bases == {
        address_map.dst_id(0, 0): 0,
        address_map.dst_id(1, 0): 0x1000,
        address_map.dst_id(0, 1): 0x3000,
        address_map.dst_id(1, 1): 0x4000,
    }
    assert [e["base"] for e in entries] == [0, 0x1000, 0x3000, 0x4000]


def test_address_map_pack_rejects_zero_size():
    am = {"tiles": [{"x": 0, "y": 0, "size": 0}]}
    with pytest.raises(ValueError, match="positive"):
        address_map.pack(am, x_dim=1, y_dim=1)


def test_address_map_pack_rejects_negative_size():
    am = {"tiles": [{"x": 0, "y": 0, "size": -0x1000}]}
    with pytest.raises(ValueError, match="positive"):
        address_map.pack(am, x_dim=1, y_dim=1)


def test_address_map_pack_rejects_non_4k_aligned_size():
    am = {"tiles": [{"x": 0, "y": 0, "size": 0x1234}]}
    with pytest.raises(ValueError, match="4 KB aligned"):
        address_map.pack(am, x_dim=1, y_dim=1)


def test_address_map_pack_rejects_node_outside_mesh():
    am = {"tiles": [{"x": 2, "y": 0, "size": 0x1000}]}
    with pytest.raises(ValueError, match="outside mesh"):
        address_map.pack(am, x_dim=2, y_dim=1)


def test_address_map_pack_rejects_missing_node():
    am = {"tiles": [{"x": 0, "y": 0, "size": 0x1000}]}  # 2x1 mesh needs 2 tiles
    with pytest.raises(ValueError, match="expected 2"):
        address_map.pack(am, x_dim=2, y_dim=1)


def test_address_map_pack_rejects_duplicate_node():
    am = {"tiles": [
        {"x": 0, "y": 0, "size": 0x1000},
        {"x": 0, "y": 0, "size": 0x1000},
    ]}
    with pytest.raises(ValueError, match="duplicate mesh node"):
        address_map.pack(am, x_dim=2, y_dim=1)


def test_address_map_pack_rejects_missing_tiles_key():
    with pytest.raises(ValueError, match="address_map.tiles"):
        address_map.pack({}, x_dim=1, y_dim=1)
    with pytest.raises(ValueError, match="address_map.tiles"):
        address_map.pack(None, x_dim=1, y_dim=1)


def test_address_map_pack_real_topologies_gap_free():
    """Cross-check: every real sim/topologies/*.yaml packs cleanly and the
    resulting bases are gap-free contiguous (base(0)=0, base(i)=base(i-1)+size(i-1)).
    Proves the Python loader accepts the migrated real YAMLs (see also the C++
    SamYaml.RealTopologies test loading the same files)."""
    import yaml

    topo_dir = os.path.join(os.path.dirname(__file__), "..", "topologies")
    paths = sorted(glob.glob(os.path.join(topo_dir, "*.yaml")))
    assert len(paths) >= 6, f"expected the real topology YAMLs, found {paths}"
    for path in paths:
        topo = yaml.safe_load(open(path))["topology"]
        _bases, entries = address_map.pack(
            yaml.safe_load(open(path))["address_map"], topo["x_dim"], topo["y_dim"])
        expected_base = 0
        for e in entries:
            assert e["base"] == expected_base, f"{path}: gap at dst_id {e['dst_id']:#x}"
            expected_base += e["size"]


def test_gen_test_patterns_and_gen_tb_top_agree_on_packed_bases(tmp_path):
    """Cross-site invariant: both generators must compute the same base(dst_id)
    from the same address_map (they share address_map.pack())."""
    import yaml

    import gen_tb_top as gt

    topo_path = tmp_path / "nonuniform.yaml"
    topo_path.write_text(
        "topology: { name: nonuniform, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
        "address_map:\n"
        "  tiles:\n"
        "    - { x: 0, y: 0, size: 0x1000 }\n"
        "    - { x: 1, y: 0, size: 0x2000 }\n"
        "    - { x: 0, y: 1, size: 0x1000 }\n"
        "    - { x: 1, y: 1, size: 0x4000 }\n"
    )
    _nodes, _x, _y, bases_from_patterns = g._load_topology(str(topo_path))
    topo = yaml.safe_load(topo_path.read_text())
    bases_from_tb_top = gt._address_map(topo)
    assert bases_from_patterns == bases_from_tb_top
