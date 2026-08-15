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


def _uniform_topology_yaml(name, x_dim, y_dim, num_vc=1, tile_size=0x100000000, block_size=None):
    """New packed-address_map topology YAML text, every memory tile the same size.

    Builds a temp topology YAML in the packed `tiles:` format for the test.
    The 4 KB config tile after each node's memory tile is what
    address_map.pack() requires of every node. block_size=None omits the key
    (the shipped-topology default: next power of two above what the spaces
    occupy); pass it explicitly to pin the node stride, as every shipped
    topology now does.
    """
    tiles = "\n".join(
        [f"    - {{ x: {x}, y: {y}, size: {tile_size:#x} }}"
         for y in range(y_dim) for x in range(x_dim)] +
        [f"    - {{ x: {x}, y: {y}, size: 0x1000, space: config }}"
         for y in range(y_dim) for x in range(x_dim)]
    )
    block_line = f"  block_size: {block_size:#x}\n" if block_size is not None else ""
    return (
        f"topology: {{ name: {name}, x_dim: {x_dim}, y_dim: {y_dim}, num_vc: {num_vc} }}\n"
        f"address_map:\n{block_line}  tiles:\n{tiles}\n"
    )


def test_emit_file_master_node_format_and_partition(tmp_path):
    d = str(tmp_path / "node0")
    g.emit_file_master_node(d, src_idx=0, dst_bases=[0x100000000] * 2, n_nodes=16,
                            base_local=0x1000, region_bytes=0x40000,
                            axi_size=5, axi_len=0, data_width=256,
                            id_rng=random.Random(0))
    w = _parse_write(os.path.join(d, "write.txt"))
    assert len(w) == 2
    for t in w:
        assert t["burst"] == 1 and t["size"] == 5 and t["len"] == 0
        assert (t["addr"] >> 32) == 1               # dst tile in addr[63:32]
        assert len(t["beats"]) == 1
    assert w[0]["addr"] != w[1]["addr"]             # disjoint offsets
    rlines = [l for l in open(os.path.join(d, "read.txt")).read().split("\n") if l != ""]
    assert len(rlines) == 2 * 11                    # 11 ax fields, no atop, no beats


def test_emit_file_master_node_addr_is_the_destination_window(tmp_path):
    """addr = dst_base + local_off -- base 0x12*4GB, offset 0x40 ->
    0x1200000040 (byte-for-byte the legacy dst_cid<<32 layout)."""
    d = str(tmp_path / "node0")
    g.emit_file_master_node(d, src_idx=0, dst_bases=[0x12 * 0x100000000], n_nodes=1,
                            base_local=0x40, region_bytes=0x40000,
                            axi_size=5, axi_len=0, data_width=256,
                            id_rng=random.Random(0))
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x1200000040


def test_emit_file_master_node_takes_an_arbitrary_window_base(tmp_path):
    d = str(tmp_path / "node0")
    g.emit_file_master_node(d, src_idx=0, dst_bases=[0x12 * 0x40000000], n_nodes=1,
                            base_local=0x40, region_bytes=0x40000,
                            axi_size=5, axi_len=0, data_width=256,
                            id_rng=random.Random(0))
    w = _parse_write(os.path.join(d, "write.txt"))
    assert w[0]["addr"] == 0x12 * 0x40000000 + 0x40


def test_load_topology_reads_packed_bases_from_address_map(tmp_path):
    # block_size declared explicitly, as every shipped topology does: node
    # stride is 2x the memory tile so the node's config tile fits inside it.
    topo_path = tmp_path / "t.yaml"
    topo_path.write_text(_uniform_topology_yaml("t", 4, 4, tile_size=0x40000000,
                                                block_size=0x80000000))
    nodes, x_dim, y_dim, bases, _config_bases, _sizes, _periph = g._load_topology(str(topo_path))
    assert (x_dim, y_dim) == (4, 4)
    # packed in raster (y, x) order, matching _uniform_topology_yaml's emit order
    assert bases[g.coord_id(0, 0)] == 0
    assert bases[g.coord_id(1, 0)] == 0x80000000
    assert bases[g.coord_id(0, 1)] == 4 * 0x80000000


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


@pytest.mark.parametrize("x_dim,y_dim", [(3, 2), (2, 3), (6, 4)])
def test_check_flit_capacity_rejects_non_power_of_two_dims(x_dim, y_dim):
    """Mirrors sam_yaml.hpp's load-time assert: a collective mask wildcards a
    clog2(dim)-bit field, so a non-power-of-two dim names a coordinate with no
    node. Caught at generate time rather than after elaboration."""
    import gen_tb_top as gt

    topo = {"topology": {"x_dim": x_dim, "y_dim": y_dim, "num_vc": 1}}
    with pytest.raises(SystemExit, match="not a power of two"):
        gt._check_flit_capacity(topo, "dummy_path.yaml")


def test_main_sources_tile_base_from_address_map(tmp_path):
    """End-to-end: main() threads the packed address_map base into the emitted address."""
    tile_size = 0x40000000
    block_size = 2 * tile_size  # declared explicitly, room for the node's config tile
    topo_path = tmp_path / "custom.yaml"
    topo_path.write_text(_uniform_topology_yaml("custom", 2, 2, tile_size=tile_size,
                                                block_size=block_size))
    out = str(tmp_path / "scn")
    g.main(["--topology", str(topo_path), "--out", out,
            "--pattern", "neighbor", "--transactions-per-node", "1",
            "--size", "5", "--len", "0"])
    w = _parse_write(os.path.join(out, "node0", "write.txt"))
    # node0 = (x=0,y=0); neighbor wraps to (1,1) on a 2x2 mesh -> coord_id (1<<4)|1 = 0x11
    # raster order (0,0),(1,0),(0,1),(1,1) -> (1,1) is the 4th packed tile -> base 3*block_size
    expected_base = 3 * block_size
    assert expected_base <= w[0]["addr"] < expected_base + tile_size


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
    # Each node's config aperture sits inside that node's own block, above its
    # memory aperture: idx * block_size + 0x2000000.
    assert txns0[-1]["addr"] == 0x2000000
    assert len(txns1) == n_beat_exact + 1
    assert txns1[-1]["addr"] == 0x100000000 + 0x2000000


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

def test_tile_major_packs_each_node_into_one_block():
    """A node's regions are contiguous inside its own block, and the stride is
    declared rather than taken from the largest region size."""
    topo = gen_tb_top.load_topology("mesh_2x2_vc1")
    _bases, entries = address_map.pack(topo["address_map"], 2, 2)
    got = {(e["space"], e["x"], e["y"]): e["base"] for e in entries}
    block = 0x100000000
    for idx, (x, y) in enumerate([(0, 0), (1, 0), (0, 1), (1, 1)]):
        assert got[("memory", x, y)] == idx * block
        assert got[("config", x, y)] == idx * block + 0x2000000


def test_pack_bases_are_coordinate_derived_on_a_non_power_of_two_row():
    from address_map import pack
    # x_dim 3, so clog2(3) = 2 index bits and a row strides four slots, one of
    # them unused. Both spaces are declared: pack() requires full coverage of
    # memory AND config, which is a real invariant of every topology and is not
    # relaxed for a test.
    mem = [{"x": x, "y": y, "size": 0x100000} for y in (0, 1) for x in (0, 1, 2)]
    cfg = [{"x": x, "y": y, "size": 0x1000, "space": "config"}
           for y in (0, 1) for x in (0, 1, 2)]
    _, entries = pack({"tiles": mem + cfg}, 3, 2)
    got = {(e["x"], e["y"], e["space"]): e["base"] for e in entries}
    assert got == {
        # No declared block_size: it defaults to the next power of two above
        # what one node's memory + config need, 0x101000 -> 0x200000. Node
        # stride is therefore 0x200000, not the 0x100000 memory slot alone.
        (0, 0, "memory"): 0x000000, (1, 0, "memory"): 0x200000,
        (2, 0, "memory"): 0x400000, (0, 1, "memory"): 0x800000,
        (1, 1, "memory"): 0xA00000, (2, 1, "memory"): 0xC00000,
        # config sits inside each node's own block, above its memory tile:
        # idx * 0x200000 + 0x100000.
        (0, 0, "config"): 0x100000, (1, 0, "config"): 0x300000,
        (2, 0, "config"): 0x500000, (0, 1, "config"): 0x900000,
        (1, 1, "config"): 0xB00000, (2, 1, "config"): 0xD00000,
    }


def test_pack_default_block_size_doubles_the_stride_to_fit_config():
    """The regression guard for a plain mesh, updated for tile-major packing.
    No declared block_size, so it defaults to the next power of two above one
    node's memory + config: 0x101000 -> 0x200000. That is double the memory
    tile alone -- the stride is no longer just the memory slot, the way it was
    before every space shared one node's block."""
    from address_map import pack
    mem = [{"x": x, "y": y, "size": 0x100000} for y in (0, 1) for x in (0, 1)]
    cfg = [{"x": x, "y": y, "size": 0x1000, "space": "config"}
           for y in (0, 1) for x in (0, 1)]
    _, entries = pack({"tiles": mem + cfg}, 2, 2)
    got = {(e["x"], e["y"], e["space"]): e["base"] for e in entries}
    assert got == {
        (0, 0, "memory"): 0x000000, (1, 0, "memory"): 0x200000,
        (0, 1, "memory"): 0x400000, (1, 1, "memory"): 0x600000,
        (0, 0, "config"): 0x100000, (1, 0, "config"): 0x300000,
        (0, 1, "config"): 0x500000, (1, 1, "config"): 0x700000,
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
    bases, entries = address_map.pack(am, x_dim=2, y_dim=2)
    # No declared block_size: memory slot 0x2000 + config slot 0x1000 ->
    # extent 0x3000 -> next power of two 0x4000. Node stride is 0x4000.
    assert bases == {
        address_map.dst_id(0, 0): 0,
        address_map.dst_id(1, 0): 0x4000,
        address_map.dst_id(0, 1): 0x8000,
        address_map.dst_id(1, 1): 0xC000,
    }
    assert [e["base"] for e in entries] == [0, 0x4000, 0x8000, 0xC000,
                                            0x2000, 0x6000, 0xA000, 0xE000]


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
    # No declared block_size: memory slot 0x100000 + config slot 0x1000 ->
    # next power of two 0x200000. Node stride is 0x200000.
    tiles = _two_space_tiles([0x100000] * 4)
    _bases, entries = address_map.pack({"tiles": tiles}, x_dim=2, y_dim=2)
    assert address_map.node_windows(entries, address_map.dst_id(0, 0), 0) == [
        {"space": "config", "base": 0x100000, "size": 0x1000},
        {"space": "memory", "base": 0x0, "size": 0x100000},
    ]
    assert address_map.node_windows(entries, address_map.dst_id(1, 1), 0) == [
        {"space": "config", "base": 0x700000, "size": 0x1000},
        {"space": "memory", "base": 0x600000, "size": 0x100000},
    ]


def test_node_windows_skip_an_absent_space():
    """A map with no config entries contributes no config window."""
    entries = [{"x": 0, "y": 0, "size": 0x100000, "space": "memory",
                "base": 0x0, "dst_id": address_map.dst_id(0, 0)}]
    assert address_map.node_windows(entries, address_map.dst_id(0, 0), 0) == [
        {"space": "memory", "base": 0x0, "size": 0x100000},
    ]


def test_node_windows_key_on_the_port_not_the_coordinate():
    """A peripheral shares its router's coordinate, so a coordinate-only match
    would stamp the peripheral's window into that router's crossbar decode as if
    it were the tile's -- and the tile would answer the peripheral's addresses
    locally, so nothing would ever reach the peripheral. The port is what tells
    the two endpoints apart."""
    topo = gen_tb_top.load_topology("mesh_2x2_vc1_periph")
    _bases, entries = address_map.pack(topo["address_map"], 2, 2)
    cid = address_map.dst_id(0, 0)
    tile = address_map.node_windows(entries, cid, 0)
    periph = address_map.node_windows(entries, cid, 1)
    assert [w["space"] for w in tile] == ["config", "memory"]
    assert [w["space"] for w in periph] == ["peripheral"]
    # Disjoint: the peripheral's region sits above the whole tile array.
    assert periph[0]["base"] >= max(w["base"] + w["size"] for w in tile)
    # The y face at this coordinate carries nothing on this topology.
    assert address_map.node_windows(entries, cid, 2) == []


def _two_space_topology():
    return {"topology": {"x_dim": 2, "y_dim": 2},
            "address_map": {"tiles": _two_space_tiles([0x100000] * 4)}}


def test_tile_targets_packs_config_first():
    """Port order and field packing are one coupled invariant: target 0 is the
    config window, the last target is the data window. One entry per endpoint,
    each holding that endpoint's own global bases."""
    nodes = [(0, 0, 0, address_map.dst_id(0, 0), 0), (1, 1, 0, address_map.dst_id(1, 0), 0),
             (2, 0, 1, address_map.dst_id(0, 1), 0), (3, 1, 1, address_map.dst_id(1, 1), 0)]
    per_node, _egress = gen_tb_top.tile_targets(_two_space_topology(), nodes)
    assert per_node[0] == [
        {"space": "config", "base": 0x100000, "size": 0x1000},
        {"space": "memory", "base": 0x0, "size": 0x100000},
    ]
    assert per_node[3] == [
        {"space": "config", "base": 0x700000, "size": 0x1000},
        {"space": "memory", "base": 0x600000, "size": 0x100000},
    ]


def test_tile_targets_rejects_a_transposed_space_order(monkeypatch):
    """An address_map.SPACE_ORDER edit must not silently transpose the two
    targets: the endpoint puts the config memory on target 0."""
    monkeypatch.setattr(address_map, "SPACE_ORDER", ("memory", "config", "peripheral"))
    nodes = [(0, 0, 0, address_map.dst_id(0, 0), 0)]
    with pytest.raises(SystemExit, match=r"must be \['config', 'memory'\]"):
        gen_tb_top.tile_targets(_two_space_topology(), nodes)


def test_tile_targets_pads_a_peripheral_row_to_the_widest():
    """The emitted TILE_BASE_ADDR / TILE_SIZE are RECTANGULAR, so a one-window
    peripheral row beside a two-window tile row would not elaborate. Short rows
    are padded to the widest.

    A pad is a REAL range parked above the egress aperture, never an empty one.
    A zero-size pad at base 0 looks inert and is the exact opposite:
    addr_decode_dync.sv:110-112 matches on

        addr >= start_addr && (addr < end_addr || end_addr == '0)

    and `end_addr == '0` is the decoder's END-OF-ADDRESS-SPACE WILDCARD
    (documented at :56-57), so start = end = 0 matches EVERY address. That is
    the trap: the check that rejects a bad rule (check_start) passes it happily,
    because check_start also exempts the wildcard.
    """
    topo = gen_tb_top.load_topology("mesh_2x2_vc1_periph")
    endpoints = gen_tb_top._endpoints(gen_tb_top._nodes(topo)[0], gen_tb_top._peripherals(topo))
    per_ep, egress = gen_tb_top.tile_targets(topo, endpoints)
    assert len(per_ep) == 6                      # 4 routers + 2 peripherals
    assert len({len(w) for w in per_ep.values()}) == 1, "rows must be rectangular"
    assert [w["space"] for w in per_ep[0]] == ["config", "memory"]
    # Endpoint 4 is the peripheral at (0,0): its own window, then the pad.
    assert per_ep[4][0]["space"] == "peripheral"
    assert per_ep[4][1]["space"] is None

    # DECODE SEMANTICS, not just shape. Rectangularity is what elaboration would
    # have caught; this is what it would NOT have -- a wildcard pad elaborates
    # fine and silently swallows the map. Every pad in every row:
    real_top = max(w["base"] + w["size"]
                   for row in per_ep.values() for w in row if w["space"] is not None)
    pads = [w for row in per_ep.values() for w in row if w["space"] is None]
    assert pads, "the peripheral rows must actually be padded"
    for pad in pads:
        start, end = pad["base"], pad["base"] + pad["size"]
        # Not the wildcard: end_addr == 0 matches every address.
        assert end != 0, f"pad {pad} is the end-of-address-space wildcard"
        # Non-empty: check_start fatals on start == end unless end is the
        # wildcard, so an "empty" rule is either a fatal or a wildcard, never
        # inert.
        assert start < end, f"pad {pad} is empty; addr_decode has no inert rule"
        # Above every real window AND above the egress aperture
        # [egress, 2*egress), so it can never shadow a live target.
        assert start >= 2 * egress >= real_top, f"pad {pad} overlaps the live map"


def test_dma_refuses_a_peripheral_topology(tmp_path, monkeypatch):
    """The DMA flavour cannot run a peripheral topology (known-limitations):
    gen_dma_jobs walks the router array, so a peripheral endpoint gets no
    jobs.txt and its driver $fatals on the missing file. It used to GENERATE --
    and the top it produced was wrong a second way, because _dma_check's
    MEM_TARGET is the last target, which on a padded peripheral row is the
    zero-size pad. Refusing beats emitting either defect.

    gen_tb_top.main() reads sys.argv rather than taking an argv list, so the
    command line is monkeypatched instead of passed.
    """
    import sys as _sys

    out = str(tmp_path / "tb.sv")

    def _run(topology):
        monkeypatch.setattr(_sys, "argv",
                            ["gen_tb_top.py", "--topology", topology, "--dma", "--out", out])
        return gen_tb_top.main()

    with pytest.raises(SystemExit, match="--dma does not support"):
        _run("mesh_2x2_vc1_periph")
    # Not a blanket ban on --dma: the same call on a peripheral-free topology
    # still emits, or the guard would be indistinguishable from deleting --dma.
    assert _run("mesh_2x2_vc1") == 0
    assert os.path.isfile(out)


def test_tile_targets_rejects_a_peripheral_whose_order_is_not_its_own_space(monkeypatch):
    """The order cross-check is per PORT and runs BEFORE the padding. Compared
    after, every short row would read as ragged-then-padded and the check would
    say nothing at all."""
    monkeypatch.setattr(address_map, "SPACE_ORDER", ("config", "memory"))
    topo = gen_tb_top.load_topology("mesh_2x2_vc1_periph")
    endpoints = gen_tb_top._endpoints(gen_tb_top._nodes(topo)[0], gen_tb_top._peripherals(topo))
    with pytest.raises(SystemExit, match=r"\(port 1\) window order \[\] must be \['peripheral'\]"):
        gen_tb_top.tile_targets(topo, endpoints)


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


def test_the_dma_top_alone_takes_ideal_master_backpressure():
    """A random 0-to-15-cycle stall on every response beat costs the directed top
    nothing -- it replays transactions this project chose -- but it decides what
    the DMA top measures, because a real AXI manager that cannot retire a beat
    cannot issue the next one.  At "random" the busiest link of a 4x4 write run
    carried 0.101 flits per cycle; at "ideal" the same run carried 0.912.  The
    two tops therefore take different profiles, and this pins the split: one
    top's profile is not the other's, and neither is a hardcoded literal."""
    topo = gen_tb_top.load_topology("mesh_2x2_vc1")
    for dma, profile in ((False, gen_tb_top._MST_BACKPRESSURE),
                         (True, gen_tb_top._MST_BACKPRESSURE_DMA)):
        stall_out, delay_out = gen_tb_top._MST_BACKPRESSURE_PROFILES[profile]
        sv = gen_tb_top.emit_tb_top(topo, dma=dma)
        assert re.search(
            rf"MST_STALL_RANDOM_OUTPUT\s*=\s*1'b{stall_out};", sv), f"dma={dma}"
        assert re.search(rf"MST_FIXED_DELAY_OUTPUT\s*=\s*{delay_out};", sv)
        # The watchdog budget follows the profile, or an "ideal" run would carry
        # the stalling profile's slack and a real hang would sit undetected.
        want = gen_tb_top._STALL_RANDOM_MAX_CYCLES if stall_out else delay_out
        assert int(re.search(r"MST_CYC_PER_BEAT\s*=\s*(\d+);", sv).group(1)) == want


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


def test_address_map_pack_real_topologies_at_the_coordinate_formula():
    """Cross-check: every real sim/topologies/*.yaml packs at
    base = ((y << clog2(x_dim)) | x) * block_size + offset[space], spelled out
    here from the YAML keys rather than read back from pack(). This is the Python half of
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
        x_dim = int(doc["topology"]["x_dim"])
        y_dim = int(doc["topology"]["y_dim"])
        tiles = doc["address_map"]["tiles"]
        _bases, entries = address_map.pack(doc["address_map"], x_dim, y_dim)
        x_bits = (x_dim - 1).bit_length()
        # Slot per space: the largest size declared in it, bounding the
        # aperture. block_size is the declared node stride.
        slot = {sp: max((int(t["size"]) for t in tiles if t.get("space", "memory") == sp),
                        default=0)
                for sp in ("memory", "config")}
        block = int(doc["address_map"]["block_size"])
        offset = {"memory": 0,
                  "config": ((slot["memory"] + slot["config"] - 1) // slot["config"])
                            * slot["config"]}
        for e in entries:
            # The tile spaces only. A peripheral region is placed in declaration
            # order above the tile array, so the coordinate formula says nothing
            # about it -- it shares its host router's coordinate, which the
            # router's own tile already packs at.
            if e["space"] == "peripheral":
                continue
            expected = (((e["y"] << x_bits) | e["x"]) * block) + offset[e["space"]]
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
    _nodes, _x, _y, bases_from_patterns, _config_bases, _sizes, _periph = g._load_topology(
        str(topo_path))
    topo = yaml.safe_load(topo_path.read_text())
    packed_bases, _entries = address_map.pack(topo["address_map"], x_dim=2, y_dim=2)
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
    nodes, x_dim, y_dim, bases, config_bases, sizes, _periph = g._load_topology(str(topo_path))
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


def test_footprint_guard_rejects_a_region_bytes_overrun_of_the_memory_tile(tmp_path, capsys):
    """region_bytes is a formula over node/transaction count and burst footprint;
    it never looks at the destination tile's real size. On a shrunk tile the
    old behaviour was a co-sim DECERR, not a generator error -- this asserts
    the generator now catches it itself.

    2x2 (n_slots=4), transactions_per_node=10, default size/len -> stride=0x40,
    region_bytes=4*10*0x40=0xa00. base_local+region_bytes=0x1000+0xa00=0x1a00,
    which overruns a 0x1000 B (4 KB aligned) tile."""
    topo_path = tmp_path / "small.yaml"
    topo_path.write_text(_uniform_topology_yaml("small", 2, 2, tile_size=0x1000))
    with pytest.raises(SystemExit):
        g.main(["--pattern", "neighbor", "--topology", str(topo_path),
                "--out", str(tmp_path / "out"), "--transactions-per-node", "10"])
    # ap.error() prints to stderr and exits with a bare status code, so the
    # computed numbers (not a mock) are checked in the captured message.
    err = capsys.readouterr().err
    assert "extent(0xa00)" in err
    assert "overruns the smallest addressable window 0x1000" in err


def test_footprint_guard_accounts_for_the_multicast_pattern_s_wider_extent(tmp_path, capsys):
    """The multicast pattern stacks its collective window on top of region_bytes
    (mcast_base = base_local + region_bytes, emit_multicast_pattern), so the
    same tile can hold region_bytes alone yet still be too small once the
    multicast window is counted.

    2x2, transactions_per_node=16, default size/len -> stride=0x40,
    region_bytes=4*16*0x40=0x1000. A 0x2000 B (4 KB aligned) tile exactly holds
    base_local+region_bytes=0x2000 (neighbor passes) but not
    base_local+region_bytes+txn*stride=0x2400 (multicast)."""
    topo_path = tmp_path / "small.yaml"
    topo_path.write_text(_uniform_topology_yaml("small", 2, 2, tile_size=0x2000))
    g.main(["--pattern", "neighbor", "--topology", str(topo_path),
            "--out", str(tmp_path / "out_neighbor"), "--transactions-per-node", "16"])
    with pytest.raises(SystemExit):
        g.main(["--pattern", "multicast", "--topology", str(topo_path),
                "--out", str(tmp_path / "out_mcast"), "--transactions-per-node", "16"])
    err = capsys.readouterr().err
    assert "extent(0x1400)" in err
    assert "overruns the smallest addressable window 0x2000" in err


_SMALL_PERIPHERAL_TOPOLOGY = """\
topology: { name: smallperiph, x_dim: 2, y_dim: 2, num_vc: 1 }
address_map:
  block_size: 0x100000000
  tiles:
    - { x: 0, y: 0, size: 0x2000000 }
    - { x: 1, y: 0, size: 0x2000000 }
    - { x: 0, y: 1, size: 0x2000000 }
    - { x: 1, y: 1, size: 0x2000000 }
    - { x: 0, y: 0, size: 0x1000, space: config }
    - { x: 1, y: 0, size: 0x1000, space: config }
    - { x: 0, y: 1, size: 0x1000, space: config }
    - { x: 1, y: 1, size: 0x1000, space: config }
  peripherals:
    - { x: 0, y: 0, face: x, size: %s }
    - { x: 0, y: 1, face: x, size: %s }
"""


def test_footprint_guard_covers_a_peripheral_window(tmp_path, capsys):
    """Fault injection on the guard's peripheral arm, which is the one that went
    missing: peripherals used to be memory tiles and were inside this min, and
    when they became their own space nothing bounded them.

    An unbounded peripheral window does not fault. The slot walks past the
    region into the NEXT peripheral's, the SAM routes it to that endpoint, and
    the readback of the same address agrees -- so the run passes with every
    peripheral transaction delivered to the wrong place. The tiles here are
    0x2000000, so only the peripheral can trip the guard.

    2x2 + 2 peripherals (n_slots=6), transactions_per_node=4, default size/len
    -> stride=0x40, region_bytes=6*4*0x40=0x600. base_local+region_bytes=0x1600
    overruns a 0x1000 peripheral and fits a 0x2000 one.
    """
    small = tmp_path / "small_periph.yaml"
    small.write_text(_SMALL_PERIPHERAL_TOPOLOGY % ("0x1000", "0x1000"))
    with pytest.raises(SystemExit):
        g.main(["--pattern", "neighbor", "--topology", str(small),
                "--out", str(tmp_path / "out_small"), "--transactions-per-node", "4"])
    err = capsys.readouterr().err
    assert "extent(0x600)" in err
    assert "overruns the smallest addressable window 0x1000" in err

    # Widened, the same run passes -- so the guard is reading the peripheral
    # window and not failing for some other reason.
    big = tmp_path / "big_periph.yaml"
    big.write_text(_SMALL_PERIPHERAL_TOPOLOGY % ("0x2000", "0x2000"))
    g.main(["--pattern", "neighbor", "--topology", str(big),
            "--out", str(tmp_path / "out_big"), "--transactions-per-node", "4"])


def test_noc_egress_aperture_sits_above_every_window():
    """A collective whose address names the issuing node's own region would be answered
    by the tile crossbar and never reach the NI. The endpoint offsets it into
    this aperture, which is the first power of two at or above the map's top --
    so no node window can ever reach it, however the map grows."""
    tiles = _two_space_tiles([0x100000] * 4)
    _bases, entries = address_map.pack({"tiles": tiles}, x_dim=2, y_dim=2)
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

# Four peripherals, one per face, so the target set is large enough for the
# weighted select to be doing something and every face is represented.
_PERIPH4 = "mesh_4x4_vc1_periph4"


def _emit_hotspot_peripherals(tmp_path):
    """_PERIPH4 under --hotspot-peripherals; returns (out dir, loaded topology)."""
    out = tmp_path / "hp"
    g.main(["--pattern", "hotspot", "--hotspot-peripherals",
            "--topology", _PERIPH4, "--out", str(out),
            "--transactions-per-node", str(_HOTSPOT_PERIPH_TXNS),
            "--size", "5", "--len", "0"])
    return out, g._load_topology(_PERIPH4)


def test_hotspot_peripherals_sends_every_tile_into_a_peripheral_region(tmp_path):
    """Every tile reaches every peripheral now, so what a destination must
    satisfy is no longer "the one on my row" but "a peripheral region at all".

    Two ways this goes wrong and both are caught here: a coordinate-keyed base
    lookup sends the traffic to the host ROUTER'S TILE (a peripheral shares its
    coordinate), and a peripheral window too small to hold the slot band walks
    the address past its region. Neither faults in co-sim -- the first is a
    legal tile and the second is the next peripheral's window.
    """
    out, (nodes, _x, _y, _bases, _config_bases, _sizes, peripherals) = \
        _emit_hotspot_peripherals(tmp_path)
    hit = set()
    for (idx, _x, _y, _cid) in nodes:
        w = _parse_write(out / f"node{idx}" / "write.txt")
        # The pattern's own transactions come first; the tail is the narrow
        # config probe every config-tile owner gets regardless of pattern.
        assert len(w) == _HOTSPOT_PERIPH_TXNS + 1
        for t in w[:_HOTSPOT_PERIPH_TXNS]:
            inside = [p for p in peripherals
                      if p["base"] <= t["addr"] < p["base"] + p["size"]]
            assert len(inside) == 1, f"node{idx} addr {t['addr']:#x} is in no peripheral region"
            hit.add(inside[0]["base"])
    # A target set that silently dropped a peripheral would satisfy every
    # assertion above and leave that endpoint with no inbound traffic.
    assert hit == {p["base"] for p in peripherals}


def test_hotspot_peripherals_keeps_the_peripheral_s_own_traffic(tmp_path):
    """A peripheral endpoint that completes zero transactions fails the run as
    vacuous (gen_tb_top's PASS guard counts endpoints, not router nodes), so its
    initiator traffic toward its partner tile has to survive the new stimulus."""
    out, (nodes, _x, _y, _bases, _config_bases, _sizes, peripherals) = \
        _emit_hotspot_peripherals(tmp_path)
    for p in range(len(peripherals)):
        ep_idx = len(nodes) + p
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
                "--topology", _PERIPH4, "--out", str(tmp_path / "np"),
                "--transactions-per-node", str(_HOTSPOT_PERIPH_TXNS),
                "--size", "5", "--len", "0"])


def test_hotspot_peripherals_rejects_a_topology_with_no_peripherals(tmp_path, capsys):
    """The empty target set is what is left of "this source can reach nothing"
    once every tile reaches every peripheral. Unguarded it reaches hotspot_dsts,
    which raises about --hotspot node ids -- a message about the argument the
    caller did not pass."""
    with pytest.raises(SystemExit):
        g.main(["--pattern", "hotspot", "--hotspot-peripherals",
                "--topology", "mesh_2x2_vc1", "--out", str(tmp_path / "none"),
                "--transactions-per-node", str(_HOTSPOT_PERIPH_TXNS)])
    assert "needs a topology that declares peripherals" in capsys.readouterr().err


def test_hotspot_peripherals_weights_the_target_set(tmp_path):
    """--hotspot-rates reaches the peripheral draw, which is the whole reason
    this reuses booksim's weighted select (traffic.cpp:514-525) instead of
    naming one target per source. A rates argument that never arrived would
    still produce a legal, uniform run."""
    _out, (_nodes, _x, _y, _b, _cb, _sizes, peripherals) = _emit_hotspot_peripherals(tmp_path)
    rng = random.Random(0)
    skewed = g.peripheral_hotspot_dsts(0, peripherals, 400, rng, rates=[7, 1, 1, 1])
    counts = [sum(1 for p in skewed if p["base"] == q["base"]) for q in peripherals]
    assert counts[0] > sum(counts[1:])
