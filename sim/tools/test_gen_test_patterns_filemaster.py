import glob
import os
import random
import re

import pytest

import address_map
import gen_tb_top
import gen_test_patterns as g


def test_axi_widths_follow_constants_ssot():
    w = g.axi_widths()
    # id is INITIATOR_ID_WIDTH, not ID_WIDTH: stimulus ids are what ONE tile
    # initiator drives, and the tile crossbar appends the master-port index on
    # top of them (constants.yaml axi.INITIATOR_ID_WIDTH).
    # addr=48 per docs/noc-target-spec.md §6 (pre-S2); data=512 is S2 T2d's
    # data-class flip (specgen/source/constants.yaml axi.DATA_WIDTH).
    assert w == {"id": 4, "addr": 48, "data": 512}


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
    """New packed-address_map topology YAML text, every memory tile the same size.

    Builds a temp topology YAML in the packed `tiles:` format for the test.
    The 4 KB config tiles after them are what address_map.pack() requires of
    every node, and are packed last so no memory base moves.
    """
    tiles = "\n".join(
        [f"    - {{ x: {x}, y: {y}, size: {tile_size:#x} }}"
         for y in range(y_dim) for x in range(x_dim)] +
        [f"    - {{ x: {x}, y: {y}, size: 0x1000, space: config }}"
         for y in range(y_dim) for x in range(x_dim)]
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
                            axi_size=5, axi_len=0, data_width=256,
                            id_rng=random.Random(0), bases=bases)
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
                            axi_size=5, axi_len=0, data_width=256,
                            id_rng=random.Random(0), bases=bases)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x1200000040


def test_emit_file_master_node_arbitrary_base_from_bases_dict(tmp_path):
    d = str(tmp_path / "node0")
    bases = {0x12: 0x12 * 0x40000000}
    g.emit_file_master_node(d, src_idx=0, dst_cids=[0x12], n_nodes=1,
                            base_local=0x40, region_bytes=0x40000,
                            axi_size=5, axi_len=0, data_width=256,
                            id_rng=random.Random(0), bases=bases)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x12 * 0x40000000 + 0x40


def test_load_topology_reads_packed_bases_from_address_map(tmp_path):
    topo_path = tmp_path / "t.yaml"
    topo_path.write_text(_uniform_topology_yaml("t", 4, 4, tile_size=0x40000000))
    nodes, x_dim, y_dim, bases, _config_bases, _sizes = g._load_topology(str(topo_path))
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


@pytest.mark.parametrize("span_key,dim_key", [("x_span", "x_dim"), ("y_span", "y_dim")])
def test_check_flit_capacity_rejects_span_wider_than_array_with_no_stated_region(span_key, dim_key):
    """A wider span with no stated tile region must not silently disarm the
    cross-row reachability guard -- the region's extent must equal the array's."""
    import gen_tb_top as gt

    topo = {"topology": {"x_dim": 2, "y_dim": 2, "num_vc": 1, span_key: 3}}
    with pytest.raises(SystemExit, match=f"tile {dim_key[0]} region"):
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
    # transactions-per-node, then the one narrow probe every config-tile owner
    # gets regardless of pattern (main()'s narrow_extra).
    assert len(w) == 2 + 1
    for t in w[:2]:
        assert t["burst"] == 1 and t["size"] == 5 and t["len"] == 0
        assert len(t["beats"]) == 1


def test_emit_beat_exact_node_full_beat_and_walking_strb(tmp_path):
    """Full beat: full 64 B strobe, per-lane-distinct bytes. Walking sweep: each
    of the 8 offsets is a single-byte write with exactly one strb bit set at
    that lane -- the boundary-straddling positions (3/4 and 31/32, the sole
    WSTRB word boundary) land where expected."""
    d = str(tmp_path / "node0")
    dst_base = 0x10000
    probe_base = dst_base + 0x1000  # emit_beat_exact_node's default base_local offset
    g.emit_beat_exact_node(d, src_idx=0, dst_base=dst_base, data_width=512)
    txns = _parse_write(os.path.join(d, "write.txt"))
    assert len(txns) == 1 + len(g._BEAT_EXACT_STRB_OFFSETS)
    full = txns[0]
    assert full["size"] == 6 and full["len"] == 0 and full["addr"] == probe_base
    data_hex, strb_hex, _user = full["beats"][0].split()
    assert strb_hex == "0x" + "f" * 16                    # 64 lanes all active
    data = int(data_hex, 16)
    for j in range(64):
        assert (data >> (8 * j)) & 0xFF == (probe_base + j) & 0xFF
    strb_base = probe_base + 64
    for t, off in zip(txns[1:], g._BEAT_EXACT_STRB_OFFSETS):
        assert t["addr"] == strb_base + off
        assert t["size"] == 0 and t["len"] == 0
        data_hex, strb_hex, _user = t["beats"][0].split()
        assert int(strb_hex, 16) == 1 << (off % 64)       # exactly one bit, at the lane
        assert (int(data_hex, 16) >> (8 * (off % 64))) & 0xFF == t["addr"] & 0xFF


def test_narrow_beat_exact_lines_shape():
    """2-beat INCR burst, AxSIZE=3 (8 B narrow lane), full per-beat strobe
    shifted to the beat's own 8-byte lane."""
    write, _read = g.narrow_beat_exact_lines(axid=2, config_base=0x400000, data_width=512)
    # _parse_write reads a path; write the lines to a temp file instead.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "write.txt")
        with open(p, "w") as f:
            f.write("\n".join(write) + "\n")
        txns = _parse_write(p)
    assert len(txns) == 1
    t = txns[0]
    assert t["id"] == 2 and t["addr"] == 0x400000 and t["len"] == 1 and t["size"] == 3
    assert len(t["beats"]) == 2
    for beat_idx, beat in enumerate(t["beats"]):
        data_hex, strb_hex, _user = beat.split()
        lane0 = (0x400000 + beat_idx * 8) % 64
        assert int(strb_hex, 16) == (0xFF << lane0)


def test_main_beat_exact_routes_both_classes_on_config_topology(tmp_path):
    """End-to-end wiring check (S2 gate deliverable 2): a node owning a
    config-space tile gains one extra narrow transaction after its beat-exact
    data-class sequence, addressed at that node's own config tile base."""
    out = str(tmp_path / "scn")
    topo_path = os.path.join(os.path.dirname(__file__), "..", "topologies",
                              "mesh_2x2_vc1.yaml")
    g.main(["--topology", topo_path, "--out", out, "--pattern", "beat_exact"])
    txns0 = _parse_write(os.path.join(out, "node0", "write.txt"))
    txns1 = _parse_write(os.path.join(out, "node1", "write.txt"))
    n_beat_exact = 1 + len(g._BEAT_EXACT_STRB_OFFSETS)
    # Config tiles pack after the 4 memory tiles (0x100000 each), so node i's
    # config base is 0x400000 + i * 0x1000.
    assert len(txns0) == n_beat_exact + 1                 # + narrow probe
    assert txns0[-1]["addr"] == 0x400000
    assert len(txns1) == n_beat_exact + 1
    assert txns1[-1]["addr"] == 0x401000


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


def _gen_ids(tmp_path, tag, x_dim, y_dim, n_txn, extra_argv):
    """Run main() on an x_dim*y_dim mesh, return {node index: [axi id, ...]} for
    the pattern transactions only -- main() appends one narrow config-space probe
    per node after them, which carries its own id."""
    topo_path = tmp_path / f"{tag}.yaml"
    topo_path.write_text(_uniform_topology_yaml(tag, x_dim, y_dim))
    out = str(tmp_path / tag)
    g.main(["--topology", str(topo_path), "--out", out,
            "--pattern", "uniform_random", "--seed", "1",
            "--transactions-per-node", str(n_txn),
            "--size", "5", "--len", "0"] + extra_argv)
    return {int(re.search(r"node(\d+)$", n).group(1)):
            [t["id"] for t in _parse_write(os.path.join(n, "write.txt"))[:n_txn]]
            for n in sorted(glob.glob(os.path.join(out, "node*")))}


def test_ids_per_initiator_one_is_the_pre_random_stimulus(tmp_path):
    """One id per initiator, = src_idx, and the random draw must not run: it
    shares main()'s rng with the destination stream, so consuming from it would
    move every address in a uniform_random run."""
    ids = _gen_ids(tmp_path, "one", 4, 4, 4, ["--ids-per-initiator", "1"])
    assert all(set(v) == {idx} for idx, v in ids.items())
    dflt = _gen_ids(tmp_path, "dflt", 4, 4, 4, [])
    assert ids == dflt


def test_ids_per_initiator_multi_repeats_and_varies_per_node(tmp_path):
    """axi_test.sv:233 draws ax_id at random and :710 leaves UNIQUE_IDS = 1'b0,
    so one seed must give a repeated id (same-id ordering) and several distinct
    ids (cross-id reordering) on every node, reproducibly."""
    ids = _gen_ids(tmp_path, "multi", 4, 4, 16, ["--ids-per-initiator", "4"])
    for idx, v in ids.items():
        assert len(set(v)) > 1, f"node {idx} drew one id: {v}"
        assert len(set(v)) < len(v), f"node {idx} never reused an id: {v}"
    assert ids == _gen_ids(tmp_path, "multi_replay", 4, 4, 16,
                           ["--ids-per-initiator", "4"])


def test_ids_per_initiator_overlapping_blocks_fit_the_id_width(tmp_path):
    """16 nodes x 4 ids overruns the 2**INITIATOR_ID_WIDTH space, so the blocks
    overlap across tiles. That is legal -- responses route by the flit src_id,
    not the AXI id (nsu/meta_buffer.hpp:21) -- but every emitted id must still
    fit the width the endpoint's a_mst_id_fits assertion checks."""
    ids = _gen_ids(tmp_path, "overlap", 4, 4, 16, ["--ids-per-initiator", "4"])
    id_space = 1 << g.axi_widths()["id"]
    assert 16 * 4 > id_space
    assert all(0 <= i < id_space for v in ids.values() for i in v)


# ---------------------------------------------------------------------------
# address_map.py: packing + validation (mirrors c_model SamTable::packed /
# SamTable::validate, nmu/addr_trans.hpp).
# ---------------------------------------------------------------------------

def test_pack_bases_are_coordinate_derived_with_a_border_column():
    from address_map import pack
    # Tiles at x=1..2, peripherals at x=0, so the route span is 3 and a row
    # strides four slots. Both spaces are declared: pack() requires full
    # coverage of memory AND config, which is a real invariant of every
    # topology and is not relaxed for a test.
    mem = [{"x": x, "y": y, "size": 0x100000} for y in (0, 1) for x in (0, 1, 2)]
    cfg = [{"x": x, "y": y, "size": 0x1000, "space": "config"}
           for y in (0, 1) for x in (0, 1, 2)]
    _, entries = pack({"tiles": mem + cfg}, 3, 2)
    got = {(e["x"], e["y"], e["space"]): e["base"] for e in entries}
    assert got == {
        (0, 0, "memory"): 0x000000, (1, 0, "memory"): 0x100000,
        (2, 0, "memory"): 0x200000, (0, 1, "memory"): 0x400000,
        (1, 1, "memory"): 0x500000, (2, 1, "memory"): 0x600000,
        # config space starts above every slot memory could occupy:
        # (1 << clog2(3)) * 2 * 0x100000 = 0x800000
        (0, 0, "config"): 0x800000, (1, 0, "config"): 0x801000,
        (2, 0, "config"): 0x802000, (0, 1, "config"): 0x804000,
        (1, 1, "config"): 0x805000, (2, 1, "config"): 0x806000,
    }


def test_pack_is_unchanged_for_a_plain_mesh():
    from address_map import pack
    # The regression guard. These are the bases today's list-order accumulator
    # produces, and the coordinate formula must reproduce them exactly --
    # memory at idx * 0x100000, config at 0x400000 + idx * 0x1000, which is
    # what every shipped topology file's own comment states.
    mem = [{"x": x, "y": y, "size": 0x100000} for y in (0, 1) for x in (0, 1)]
    cfg = [{"x": x, "y": y, "size": 0x1000, "space": "config"}
           for y in (0, 1) for x in (0, 1)]
    _, entries = pack({"tiles": mem + cfg}, 2, 2)
    got = {(e["x"], e["y"], e["space"]): e["base"] for e in entries}
    assert got == {
        (0, 0, "memory"): 0x000000, (1, 0, "memory"): 0x100000,
        (0, 1, "memory"): 0x200000, (1, 1, "memory"): 0x300000,
        (0, 0, "config"): 0x400000, (1, 0, "config"): 0x401000,
        (0, 1, "config"): 0x402000, (1, 1, "config"): 0x403000,
    }


def test_pack_places_a_smaller_entry_on_its_own_slot():
    """Mixed sizes are still accepted, bounded by the space's slot (here
    max(0x1000, 0x2000) = 0x2000) -- but a smaller entry no longer drags its
    neighbours down. Every entry sits at its own coordinate's slot, so the
    address's coordinate field reads back correctly no matter which entry is
    smaller. Under the old accumulator a single undersized tile shifted every
    base after it; this is the sharpest case of that in the suite."""
    am = {"tiles": [
        {"x": 0, "y": 0, "size": 0x1000},
        {"x": 1, "y": 0, "size": 0x2000},
        {"x": 0, "y": 1, "size": 0x1000},
        {"x": 1, "y": 1, "size": 0x1000},
    ] + [{"x": x, "y": y, "size": 0x1000, "space": "config"}
         for x, y in [(0, 0), (1, 0), (0, 1), (1, 1)]]}
    bases, entries = address_map.pack(am, x_span=2, y_span=2)
    assert bases == {
        address_map.dst_id(0, 0): 0,
        address_map.dst_id(1, 0): 0x2000,
        address_map.dst_id(0, 1): 0x4000,
        address_map.dst_id(1, 1): 0x6000,
    }
    assert [e["base"] for e in entries] == [0, 0x2000, 0x4000, 0x6000,
                                            0x8000, 0x9000, 0xA000, 0xB000]


def _two_space_tiles(memory_sizes):
    """2x2 memory tiles of the given sizes plus one 4 KB config tile per node."""
    nodes = [(0, 0), (1, 0), (0, 1), (1, 1)]
    return ([{"x": x, "y": y, "size": s, "space": "memory"}
             for (x, y), s in zip(nodes, memory_sizes)] +
            [{"x": x, "y": y, "size": 0x1000, "space": "config"} for x, y in nodes])


def test_node_windows_are_that_node_s_own_regions():
    """The tile crossbar decodes on THIS node's windows, so a local initiator's
    address that belongs to another node misses both rules and falls through to
    the NMU. Sizes are exact -- axi_xbar states a rule as start/end."""
    tiles = _two_space_tiles([0x100000] * 4)
    _bases, entries = address_map.pack({"tiles": tiles}, x_span=2, y_span=2)
    assert address_map.node_windows(entries, address_map.dst_id(0, 0)) == [
        {"space": "config", "base": 0x400000, "size": 0x1000},
        {"space": "memory", "base": 0x0, "size": 0x100000},
    ]
    assert address_map.node_windows(entries, address_map.dst_id(1, 1)) == [
        {"space": "config", "base": 0x403000, "size": 0x1000},
        {"space": "memory", "base": 0x300000, "size": 0x100000},
    ]


def test_node_windows_skip_an_absent_space():
    """A map with no config entries contributes no config window."""
    entries = [{"x": 0, "y": 0, "size": 0x100000, "space": "memory",
                "base": 0x0, "dst_id": address_map.dst_id(0, 0)}]
    assert address_map.node_windows(entries, address_map.dst_id(0, 0)) == [
        {"space": "memory", "base": 0x0, "size": 0x100000},
    ]


def _two_space_topology():
    return {"topology": {"x_dim": 2, "y_dim": 2},
            "address_map": {"tiles": _two_space_tiles([0x100000] * 4)}}


def test_tile_targets_packs_config_first():
    """Port order and field packing are one coupled invariant: target 0 is the
    config window, the last target is the data window. One entry per node, each
    holding that node's own global bases."""
    nodes = [(0, 0, 0, address_map.dst_id(0, 0)), (1, 1, 0, address_map.dst_id(1, 0)),
             (2, 0, 1, address_map.dst_id(0, 1)), (3, 1, 1, address_map.dst_id(1, 1))]
    per_node, _egress = gen_tb_top.tile_targets(_two_space_topology(), nodes)
    assert per_node[0] == [
        {"space": "config", "base": 0x400000, "size": 0x1000},
        {"space": "memory", "base": 0x0, "size": 0x100000},
    ]
    assert per_node[3] == [
        {"space": "config", "base": 0x403000, "size": 0x1000},
        {"space": "memory", "base": 0x300000, "size": 0x100000},
    ]


def test_tile_targets_rejects_a_transposed_space_order(monkeypatch):
    """An address_map.SPACE_ORDER edit must not silently transpose the two
    targets: the endpoint puts the config memory on target 0."""
    monkeypatch.setattr(address_map, "SPACE_ORDER", ("memory", "config"))
    nodes = [(0, 0, 0, address_map.dst_id(0, 0))]
    with pytest.raises(SystemExit, match="config-then-memory"):
        gen_tb_top.tile_targets(_two_space_topology(), nodes)


@pytest.mark.parametrize("profile", sorted(gen_tb_top._MEM_LATENCY_PROFILES))
def test_mem_latency_profile_reaches_the_right_four_parameters(monkeypatch, profile):
    """The profile tuple is (stall_in, stall_out, delay_in, delay_out) but the
    endpoint takes four separately named parameters, so a transposition would
    silently run the wrong latency -- and input/output transposed is invisible on
    a symmetric profile. Emit each one and read the values back off the names."""
    monkeypatch.setattr(gen_tb_top, "_MEM_LATENCY", profile)
    sv = gen_tb_top.emit_tb_top(gen_tb_top.load_topology("mesh_2x2_vc1"))
    stall_in, stall_out, delay_in, delay_out = gen_tb_top._MEM_LATENCY_PROFILES[profile]
    for name, want in (("MEM_STALL_RANDOM_INPUT", f"1'b{stall_in}"),
                       ("MEM_STALL_RANDOM_OUTPUT", f"1'b{stall_out}"),
                       ("MEM_FIXED_DELAY_INPUT", str(delay_in)),
                       ("MEM_FIXED_DELAY_OUTPUT", str(delay_out))):
        assert re.search(rf"localparam\s[^;]*\b{name}\s*=\s*{re.escape(want)};", sv), \
            f"{profile}: {name} != {want}"
        # And it must actually reach the endpoint, not just sit in tb_top.
        assert f".{name}({name})" in sv


def test_watchdog_grows_with_the_memory_latency_profile(monkeypatch):
    """A stalling memory that the watchdog is not sized for reads as a hang."""
    def k(profile):
        monkeypatch.setattr(gen_tb_top, "_MEM_LATENCY", profile)
        sv = gen_tb_top.emit_tb_top(gen_tb_top.load_topology("mesh_2x2_vc1"))
        return int(re.search(r"MEM_CYC_PER_BEAT\s*=\s*(\d+);", sv).group(1))

    assert k("ideal") == 0
    assert k("random") == gen_tb_top._STALL_RANDOM_MAX_CYCLES


def test_all_to_all_never_repeats_a_destination_and_never_targets_self():
    """The whole reason the pattern exists: with one id per initiator, a
    destination change on every transaction forces the NMU's allocating branch,
    where uniform_random's 1/n repeats fall into the same-destination bypass.
    Self is excluded because a tile-local request never reaches the NMU."""
    n = 16
    for src in (0, 7, 15):
        d = g.all_to_all_dsts(src, n, 64)
        assert len(d) == 64
        assert src not in d
        assert all(d[i] != d[i + 1] for i in range(len(d) - 1)), f"repeat at src {src}"
        assert set(d) == {k for k in range(n) if k != src}, "must reach every other node"


def test_all_to_all_is_deterministic():
    """No rng argument at all -- the same call must give the same list, so a run
    replays without carrying a seed for the spatial pattern."""
    assert g.all_to_all_dsts(3, 16, 40) == g.all_to_all_dsts(3, 16, 40)


def test_address_map_pack_rejects_zero_size():
    am = {"tiles": [{"x": 0, "y": 0, "size": 0}]}
    with pytest.raises(ValueError, match="positive"):
        address_map.pack(am, x_span=1, y_span=1)


def test_address_map_pack_rejects_negative_size():
    am = {"tiles": [{"x": 0, "y": 0, "size": -0x1000}]}
    with pytest.raises(ValueError, match="positive"):
        address_map.pack(am, x_span=1, y_span=1)


def test_address_map_pack_rejects_non_4k_aligned_size():
    am = {"tiles": [{"x": 0, "y": 0, "size": 0x1234}]}
    with pytest.raises(ValueError, match="4 KB aligned"):
        address_map.pack(am, x_span=1, y_span=1)


def test_address_map_pack_rejects_node_outside_mesh():
    am = {"tiles": [{"x": 2, "y": 0, "size": 0x1000}]}
    with pytest.raises(ValueError, match="outside mesh"):
        address_map.pack(am, x_span=2, y_span=1)


def test_address_map_pack_rejects_missing_node():
    am = {"tiles": [{"x": 0, "y": 0, "size": 0x1000}]}  # 2x1 mesh needs 2 tiles
    with pytest.raises(ValueError, match="expected 2"):
        address_map.pack(am, x_span=2, y_span=1)


def test_address_map_pack_rejects_missing_config_node():
    """Config coverage is the same rule as memory coverage: a YAML one config
    tile short packs to bases that are no longer a clean wildcard set, which
    used to surface only under PATTERN=multicast."""
    am = {"tiles": [
        {"x": 0, "y": 0, "size": 0x1000},
        {"x": 1, "y": 0, "size": 0x1000},
        {"x": 0, "y": 0, "size": 0x1000, "space": "config"},
    ]}
    with pytest.raises(ValueError, match="config space covers 1 nodes, expected 2"):
        address_map.pack(am, x_span=2, y_span=1)


def test_address_map_pack_rejects_duplicate_node():
    am = {"tiles": [
        {"x": 0, "y": 0, "size": 0x1000},
        {"x": 0, "y": 0, "size": 0x1000},
    ]}
    with pytest.raises(ValueError, match="duplicate mesh node"):
        address_map.pack(am, x_span=2, y_span=1)


def test_address_map_pack_rejects_missing_tiles_key():
    with pytest.raises(ValueError, match="address_map.tiles"):
        address_map.pack({}, x_span=1, y_span=1)
    with pytest.raises(ValueError, match="address_map.tiles"):
        address_map.pack(None, x_span=1, y_span=1)


def test_address_map_pack_real_topologies_at_the_coordinate_formula():
    """Cross-check: every real sim/topologies/*.yaml packs at
    base = space_base + ((y << clog2(x_span)) | x) * slot, spelled out here from
    the YAML keys rather than read back from pack(). This is the Python half of
    the packing agreement; the C++ half is
    SamYaml.RealTopologiesPackedAtTheCoordinateFormula, asserting the same
    formula against SamTable::packed(). List-order accumulation (base += size)
    agrees with the formula only where the span is a power of two and every
    entry in a space is one slot, which is true of every topology shipped today
    and not of a span with a border coordinate."""
    import yaml

    topo_dir = os.path.join(os.path.dirname(__file__), "..", "topologies")
    paths = sorted(glob.glob(os.path.join(topo_dir, "*.yaml")))
    # Guards the glob, not the inventory: an empty or unreachable directory must
    # fail rather than pass vacuously. A count tied to how many topologies ship
    # would need editing on every add or remove.
    assert paths, f"expected the real topology YAMLs in {topo_dir}"
    for path in paths:
        doc = yaml.safe_load(open(path))
        # Same span resolution the testbench generator uses.
        x_span, y_span = gen_tb_top._route_span(doc["topology"])[:2]
        tiles = doc["address_map"]["tiles"]
        _bases, entries = address_map.pack(doc["address_map"], x_span, y_span)
        x_bits = (x_span - 1).bit_length()
        # Slot per space: the largest size declared in it. Config sits above
        # every base memory could take.
        slot = {sp: max((int(t["size"]) for t in tiles if t.get("space", "memory") == sp),
                        default=0)
                for sp in ("memory", "config")}
        space_base = {"memory": 0, "config": (1 << x_bits) * y_span * slot["memory"]}
        for e in entries:
            expected = space_base[e["space"]] + (((e["y"] << x_bits) | e["x"]) * slot[e["space"]])
            assert e["base"] == expected, \
                f"{path}: {e['space']} tile ({e['x']},{e['y']}) base {e['base']:#x} != {expected:#x}"


def test_gen_test_patterns_bases_come_from_the_shared_packer(tmp_path):
    """Cross-site invariant: the stimulus generator's base(dst_id) is
    address_map.pack()'s, not a second packing rule of its own."""
    import yaml

    topo_path = tmp_path / "nonuniform.yaml"
    topo_path.write_text(
        "topology: { name: nonuniform, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
        "address_map:\n"
        "  tiles:\n"
        "    - { x: 0, y: 0, size: 0x1000 }\n"
        "    - { x: 1, y: 0, size: 0x2000 }\n"
        "    - { x: 0, y: 1, size: 0x1000 }\n"
        "    - { x: 1, y: 1, size: 0x4000 }\n"
        "    - { x: 0, y: 0, size: 0x1000, space: config }\n"
        "    - { x: 1, y: 0, size: 0x1000, space: config }\n"
        "    - { x: 0, y: 1, size: 0x1000, space: config }\n"
        "    - { x: 1, y: 1, size: 0x1000, space: config }\n"
    )
    _nodes, _x, _y, bases_from_patterns, _config_bases, _sizes = g._load_topology(
        str(topo_path))
    topo = yaml.safe_load(topo_path.read_text())
    packed_bases, _entries = address_map.pack(topo["address_map"], x_span=2, y_span=2)
    assert bases_from_patterns == packed_bases


def _mcast_topology(tmp_path, config_size, dim=8):
    """dim x dim mesh, 1 MB memory + one config tile per node. Rows are a power
    of two so the multicast mask stays wildcard-clean."""
    tiles = [f"    - {{ x: {x}, y: {y}, size: 0x100000 }}"
             for y in range(dim) for x in range(dim)]
    tiles += [f"    - {{ x: {x}, y: {y}, size: {config_size:#x}, space: config }}"
              for y in range(dim) for x in range(dim)]
    path = tmp_path / f"mcast_cfg{config_size:x}.yaml"
    path.write_text(f"topology: {{ name: t, x_dim: {dim}, y_dim: {dim}, num_vc: 1 }}\n"
                    "address_map:\n  tiles:\n" + "\n".join(tiles) + "\n")
    return path


def _emit_mcast(tmp_path, config_size):
    topo_path = _mcast_topology(tmp_path, config_size)
    nodes, x_dim, y_dim, bases, config_bases, sizes = g._load_topology(str(topo_path))
    g.emit_multicast_pattern(str(tmp_path / f"out{config_size:x}"), nodes, x_dim, y_dim,
                             bases, config_bases, sizes, "row", 2, 5, 0, 512,
                             0x1000, len(nodes) * 2 * g._SLOT_STRIDE, len(nodes))


def test_config_probe_window_is_bounded_by_the_config_entry(tmp_path):
    """The cross-node config probe must stay inside the config SAM entry. An
    overrun does not fault -- it lands in the next entry, routes to another
    node's config RAM, rebases to a legal offset and reads back consistently,
    so the crossbar's DECERR gate never sees it.

    8x8 discriminates the bound from base_local (0x1000), which the two happen
    to share on every shipped topology: the probe window is
    0x800 + 64*0x40 = 0x1800, legal in a 0x2000 config entry and an overrun in
    a 0x1000 one."""
    _emit_mcast(tmp_path, 0x2000)
    with pytest.raises(SystemExit, match="overruns the 0x1000 B config entry"):
        _emit_mcast(tmp_path, 0x1000)


def test_noc_egress_aperture_sits_above_every_window():
    """A collective whose address names the issuing node's own region would be answered
    by the tile crossbar and never reach the NI. The endpoint offsets it into
    this aperture, which is the first power of two at or above the map's top --
    so no node window can ever reach it, however the map grows."""
    tiles = _two_space_tiles([0x100000] * 4)
    _bases, entries = address_map.pack({"tiles": tiles}, x_span=2, y_span=2)
    base = address_map.noc_egress_base(entries)
    top = max(e["base"] + e["size"] for e in entries)
    assert base >= top
    assert base & (base - 1) == 0          # power of two
    # The aperture is [base, 2*base), so the whole map offset into it still fits.
    assert top <= base


@pytest.mark.parametrize("pattern", ["bit_complement", "bit_reverse", "shuffle",
                                     "bit_rotation", "tornado"])
def test_permutation_patterns_are_bijections(pattern):
    """A permutation traffic pattern must map the node set onto itself: every
    node sends exactly one stream and receives exactly one. A formula that is
    not a bijection silently overloads some nodes and starves others, which
    reads as a fabric result rather than a stimulus bug."""
    nodes = [(x, y) for y in range(4) for x in range(4)]
    dsts = [g._dst_for(pattern, x, y, 4, 4) for (x, y) in nodes]
    assert sorted(dsts) == sorted(nodes)


def test_permutation_patterns_match_booksim():
    """Spot values against the booksim2 formulas the ports cite, on a 4x4 whose
    row-major index is idx = y*4 + x."""
    # bitcomp: ~idx & 15. idx(1,2) = 9 -> 6 -> (2,1)
    assert g._dst_for("bit_complement", 1, 2, 4, 4) == (2, 1)
    # bitrev: idx 9 = 0b1001 reversed is 0b1001 -> itself
    assert g._dst_for("bit_reverse", 1, 2, 4, 4) == (1, 2)
    # shuffle: rotate left. idx 9 -> 0b0011 = 3 -> (3,0)
    assert g._dst_for("shuffle", 1, 2, 4, 4) == (3, 0)
    # bit_rotation: rotate right. idx 9 odd -> 9//2 + 8 = 12 -> (0,3)
    assert g._dst_for("bit_rotation", 1, 2, 4, 4) == (0, 3)
    # tornado at k=4: shift (4+1)//2 - 1 = 1 in both dimensions
    assert g._dst_for("tornado", 1, 2, 4, 4) == (2, 3)


def test_every_deterministic_pattern_is_dispatched():
    """The emission branch and _dst_for read one list. A pattern in _dst_for but
    missing from that list used to fall through to hotspot and emit the wrong
    traffic without saying so."""
    for pattern in g._DETERMINISTIC_PATTERNS:
        assert g._dst_for(pattern, 1, 2, 4, 4) is not None


def test_bit_permutation_guard_rejects_a_non_power_of_two_mesh():
    """Booksim BitPermutationTrafficPattern exit(-1)s unless the node count is a
    power of two -- a permutation of the id bits is a bijection only then."""
    with pytest.raises(SystemExit, match="power-of-two node count"):
        g._check_bit_permutation_guard("shuffle", 3, 2)


def test_tornado_guard_rejects_a_non_uniform_radix():
    with pytest.raises(SystemExit, match="uniform radix"):
        g._check_tornado_guard(4, 2)


def test_peripheral_slots_do_not_land_on_a_multicast_address(tmp_path):
    """Every address the run writes has exactly one writer.

    The multicast window opens at base_local + region_bytes, and both that and
    every alloc_unique_offset band count ENDPOINTS. Counted in router nodes, a
    peripheral's slot walks past the window into the collective addresses -- on
    mesh_2x2_vc1_periph 0x1100 is both endpoint 4's slot and node0's multicast
    address.
    Nothing else holds the two apart: reverting the band fails this test.
    """
    out = tmp_path / "mc"
    g.main(["--pattern", "multicast", "--topology", "mesh_2x2_vc1_periph",
            "--out", str(out), "--transactions-per-node", "2"])
    addrs = [t["addr"] for node in sorted(out.iterdir())
             for t in _parse_write(node / "write.txt")]
    assert len(addrs) == len(set(addrs))


_HOTSPOT_PERIPH_TXNS = 2


def _emit_hotspot_peripherals(tmp_path):
    """mesh_2x2_vc1_periph under --hotspot-peripherals; returns (out dir, topology)."""
    out = tmp_path / "hp"
    g.main(["--pattern", "hotspot", "--hotspot-peripherals",
            "--topology", "mesh_2x2_vc1_periph", "--out", str(out),
            "--transactions-per-node", str(_HOTSPOT_PERIPH_TXNS),
            "--size", "5", "--len", "0"])
    return out, g._load_topology("mesh_2x2_vc1_periph")


def test_hotspot_peripherals_sends_every_tile_to_its_own_row_peripheral(tmp_path):
    """The peripheral on a tile's own row is the only destination it may have.

    A peripheral off the x face is reached by running out of x hops, which
    happens on the source's row (check_dst_reachable), so a tile that targeted
    the other row's peripheral would be aborted by the NMU at packetize time.
    """
    out, (nodes, _x_dim, _y_dim, bases, _config_bases, sizes) = _emit_hotspot_peripherals(tmp_path)
    periph_of_row = {g.coord_xy(c)[1]: c
                     for c in bases if c not in {n[3] for n in nodes}}
    for (idx, _x, _y, cid) in nodes:
        w = _parse_write(out / f"node{idx}" / "write.txt")
        # The pattern's own transactions come first; the tail is the narrow
        # config probe every config-tile owner gets regardless of pattern.
        assert len(w) == _HOTSPOT_PERIPH_TXNS + 1
        periph = periph_of_row[g.coord_xy(cid)[1]]
        for t in w[:_HOTSPOT_PERIPH_TXNS]:
            assert bases[periph] <= t["addr"] < bases[periph] + sizes["memory"][periph]


def test_hotspot_peripherals_keeps_the_peripheral_s_own_traffic(tmp_path):
    """A peripheral endpoint that completes zero transactions fails the run as
    vacuous (gen_tb_top's PASS guard counts endpoints, not router nodes), so its
    initiator traffic toward its partner tile has to survive the new stimulus."""
    out, (nodes, _x_dim, _y_dim, bases, _config_bases, _sizes) = _emit_hotspot_peripherals(tmp_path)
    periph_cids = [c for c in bases if c not in {n[3] for n in nodes}]
    for ep_idx, _cid in enumerate(periph_cids, start=len(nodes)):
        assert len(_parse_write(out / f"node{ep_idx}" / "write.txt")) == _HOTSPOT_PERIPH_TXNS


def test_hotspot_peripherals_writes_every_address_once(tmp_path):
    """Each slot has exactly one writer. The partner tile draws the peripheral's
    extras from the same (src, seq) band as its own hotspot traffic, so leaving
    both in would write one set of addresses twice."""
    out, _topo = _emit_hotspot_peripherals(tmp_path)
    addrs = [t["addr"] for node in sorted(out.iterdir())
             for t in _parse_write(node / "write.txt")]
    assert len(addrs) == len(set(addrs))


def test_hotspot_peripherals_is_rejected_on_another_pattern(tmp_path):
    """The selector reaches two places and only one is inside the pattern
    dispatch: it steers destinations for `hotspot` alone, but suppresses the
    partner tile's inbound lines for whatever pattern runs. On any other pattern
    that leaves nothing aimed at the peripheral, and the run still passes because
    the peripheral's own initiator stream keeps it non-vacuous.
    """
    with pytest.raises(SystemExit):
        g.main(["--pattern", "neighbor", "--hotspot-peripherals",
                "--topology", "mesh_2x2_vc1_periph", "--out", str(tmp_path / "np"),
                "--transactions-per-node", str(_HOTSPOT_PERIPH_TXNS),
                "--size", "5", "--len", "0"])


def test_hotspot_peripherals_rejects_a_source_that_reaches_none():
    """A row with no peripheral has no hotspot; the source is named and the run
    stops, rather than the tile silently falling back to another row's."""
    nodes, _x_dim, _y_dim, _bases, _config_bases, _sizes = g._load_topology("mesh_2x2_vc1_periph")
    row0_only = [(len(nodes), g.coord_id(0, 0))]
    src_on_row1 = next(cid for (_i, _x, _y, cid) in nodes if g.coord_xy(cid)[1] == 1)
    with pytest.raises(SystemExit, match="found 0"):
        g.peripheral_hotspot(src_on_row1, row0_only, nodes)


def test_hotspot_peripherals_rejects_a_source_that_reaches_two():
    """Two peripherals on one row (west and east face) leave the selection
    ambiguous. Weighting them would change booksim's distribution, so it stops."""
    nodes, _x_dim, _y_dim, _bases, _config_bases, _sizes = g._load_topology("mesh_2x2_vc1_periph")
    both_faces = [(len(nodes), g.coord_id(0, 0)), (len(nodes) + 1, g.coord_id(3, 0))]
    src_on_row0 = next(cid for (_i, _x, _y, cid) in nodes if g.coord_xy(cid)[1] == 0)
    with pytest.raises(SystemExit, match="found 2"):
        g.peripheral_hotspot(src_on_row0, both_faces, nodes)
