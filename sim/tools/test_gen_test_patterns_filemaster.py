import glob
import os

import pytest

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


def test_emit_file_master_node_format_and_partition(tmp_path):
    d = str(tmp_path / "node0")
    g.emit_file_master_node(d, src_idx=0, dst_cids=[1, 1], n_nodes=16,
                            base_local=0x1000, memory_size=0x40000,
                            axi_size=5, axi_len=0, data_width=256)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert len(w) == 2
    for t in w:
        assert t["burst"] == 1 and t["size"] == 5 and t["len"] == 0
        assert (t["addr"] >> 32) == 1               # dst tile in addr[63:32]
        assert len(t["beats"]) == 1
    assert w[0]["addr"] != w[1]["addr"]             # disjoint offsets
    rlines = [l for l in open(os.path.join(d, "read.txt")).read().split("\n") if l != ""]
    assert len(rlines) == 2 * 11                    # 11 ax fields, no atop, no beats


def test_emit_file_master_node_default_tile_size_matches_legacy_4gb(tmp_path):
    """Regression pin: default tile_size (unspecified) reproduces the legacy
    dst_cid<<32 layout byte-for-byte -- dst coord_id 0x12, offset 0x40 -> 0x1200000040."""
    d = str(tmp_path / "node0")
    g.emit_file_master_node(d, src_idx=0, dst_cids=[0x12], n_nodes=1,
                            base_local=0x40, memory_size=0x40000,
                            axi_size=5, axi_len=0, data_width=256)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x1200000040


def test_emit_file_master_node_non_4gb_tile_size_shifts_base(tmp_path):
    d = str(tmp_path / "node0")
    g.emit_file_master_node(d, src_idx=0, dst_cids=[0x12], n_nodes=1,
                            base_local=0x40, memory_size=0x40000,
                            axi_size=5, axi_len=0, data_width=256,
                            tile_size=0x40000000)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x12 * 0x40000000 + 0x40


def test_load_topology_reads_tile_size_from_address_map(tmp_path):
    topo_path = tmp_path / "t.yaml"
    topo_path.write_text(
        "topology: { name: t, x_dim: 4, y_dim: 4, num_vc: 1 }\n"
        "address_map:\n"
        "  tile_size: 0x40000000\n"
    )
    nodes, x_dim, y_dim, tile_size = g._load_topology(str(topo_path))
    assert tile_size == 0x40000000


def test_load_topology_defaults_tile_size_when_address_map_absent(tmp_path):
    topo_path = tmp_path / "t.yaml"
    topo_path.write_text("topology: { name: t, x_dim: 4, y_dim: 4, num_vc: 1 }\n")
    nodes, x_dim, y_dim, tile_size = g._load_topology(str(topo_path))
    assert tile_size == 0x100000000


def test_main_sources_tile_base_from_address_map(tmp_path):
    """End-to-end: main() threads address_map.tile_size into the emitted address."""
    topo_path = tmp_path / "custom.yaml"
    topo_path.write_text(
        "topology: { name: custom, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
        "address_map:\n"
        "  tile_size: 0x40000000\n"
    )
    out = str(tmp_path / "scn")
    g.main(["--topology", str(topo_path), "--out", out,
            "--pattern", "neighbor", "--transactions-per-node", "1",
            "--size", "5", "--len", "0", "--memory-size", "0x40000"])
    w = _parse_write(os.path.join(out, "node0", "write.txt"))
    # node0 = (x=0,y=0); neighbor wraps to (1,1) on a 2x2 mesh -> coord_id (1<<4)|1 = 0x11
    dst_cid = (1 << 4) | 1
    expected_base = dst_cid * 0x40000000
    assert expected_base <= w[0]["addr"] < expected_base + 0x40000000


PATTERNS = [
    ["--pattern", "neighbor"],
    ["--pattern", "transpose"],
    ["--pattern", "uniform_random", "--seed", "1"],
    ["--pattern", "hotspot", "--hotspot", "5", "--seed", "1"],
]


@pytest.mark.parametrize("pat", PATTERNS, ids=lambda p: p[1])
def test_main_file_master_all_patterns(tmp_path, pat):
    out = str(tmp_path / "scn")
    g.main(["--topology", "mesh_4x4_vc1",
            "--out", out, "--transactions-per-node", "2",
            "--size", "5", "--len", "0", "--memory-size", "0x40000"] + pat)
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
