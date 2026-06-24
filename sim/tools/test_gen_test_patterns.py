"""Unit tests for gen_test_patterns.

Run:  python3 -m pytest sim/tools/test_gen_test_patterns.py -v
      (or: cd sim/tools && python3 -m pytest test_gen_test_patterns.py -v)
"""
import os
import sys
import tempfile

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import random

from gen_test_patterns import (  # noqa: E402
    DST_ID_WIDTH,
    alloc_unique_offset,
    coord_id,
    hotspot_dsts,
    neighbor_dst,
    uniform_random_dsts,
)


# ---------------------------------------------------------------------------
# neighbor_dst: ported from booksim2 traffic.cpp:316
# ---------------------------------------------------------------------------

def test_neighbor_plus1_wrap():
    # Basic +1 per dimension, no wrap needed.
    assert neighbor_dst(1, 1, 4, 4) == (2, 2)
    assert neighbor_dst(0, 0, 4, 4) == (1, 1)


def test_neighbor_wraps_at_dim_boundary():
    # x=3, y=3 in a 4x4 mesh wraps to (0, 0).
    assert neighbor_dst(3, 3, 4, 4) == (0, 0)
    # x=3 only wraps in x.
    assert neighbor_dst(3, 1, 4, 4) == (0, 2)
    # y=3 only wraps in y.
    assert neighbor_dst(1, 3, 4, 4) == (2, 0)


def test_neighbor_is_bijection_4x4():
    """neighbor forms a permutation over all 16 nodes of a 4x4 mesh."""
    dsts = [neighbor_dst(i % 4, i // 4, 4, 4) for i in range(16)]
    assert len(set(dsts)) == 16, "neighbor must be a bijection (all dst unique)"


def test_neighbor_is_non_self_4x4():
    """neighbor dst != src for all nodes when dim > 1."""
    for i in range(16):
        src = (i % 4, i // 4)
        dst = neighbor_dst(src[0], src[1], 4, 4)
        assert dst != src, f"neighbor of {src} must differ from src in a 4x4 mesh"


# ---------------------------------------------------------------------------
# alloc_unique_offset: globally-unique absolute write addresses
# ---------------------------------------------------------------------------

def test_alloc_unique_per_src_seq():
    """Different (src_node, seq) pairs must yield different offsets."""
    n = 16
    base = 0x1000
    big = n * n * 0x40 + 0x40  # ample window so the bounds assertion never fires here
    offsets = set()
    for src in range(n):
        for seq in range(n):
            off = alloc_unique_offset(0, src, seq, base, n, big)
            offsets.add(off)
    assert len(offsets) == n * n, "alloc_unique_offset must produce distinct values"


def test_alloc_same_src_seq_is_stable():
    """Same arguments always return the same offset."""
    v1 = alloc_unique_offset(5, 3, 2, 0x1000, 16, 0x10000)
    v2 = alloc_unique_offset(5, 3, 2, 0x1000, 16, 0x10000)
    assert v1 == v2


def test_alloc_neighbor_no_absolute_collision():
    """Under neighbor on a 4x4 mesh, all write absolute addresses are globally unique.

    Simulates what gen_test_patterns produces: for each src_node, the write addr
    is dst_coord<<32 + alloc_unique_offset(dst_node, src_node, seq, base, n_nodes, mem).
    """
    x_dim, y_dim = 4, 4
    n_nodes = x_dim * y_dim
    base_local = 0x1000
    mem = 0x1000
    # Simulate a single-transaction base scenario (seq=0 for every node).
    abs_addrs = set()
    for i in range(n_nodes):
        x, y = i % x_dim, i // x_dim
        dx, dy = neighbor_dst(x, y, x_dim, y_dim)
        dst_cid = coord_id(dx, dy)
        local_off = alloc_unique_offset(dst_cid, i, 0, base_local, n_nodes, mem)
        abs_addr = (dst_cid << 32) + local_off
        abs_addrs.add(abs_addr)
    assert len(abs_addrs) == n_nodes, (
        "neighbor distribution: every write must target a globally unique absolute address"
    )


def test_alloc_neighbor_multi_txn_no_absolute_collision():
    """Neighbor on 4x4 with N_TXN transactions per node: all absolute addrs unique."""
    x_dim, y_dim = 4, 4
    n_nodes = x_dim * y_dim
    base_local = 0x1000
    mem = 0x1000
    n_txn = 4  # simulates AX4-BAS-005 (4 write transactions); 16*4*64 = 4096 = mem exact
    abs_addrs = set()
    for i in range(n_nodes):
        x, y = i % x_dim, i // x_dim
        dx, dy = neighbor_dst(x, y, x_dim, y_dim)
        dst_cid = coord_id(dx, dy)
        for seq in range(n_txn):
            local_off = alloc_unique_offset(dst_cid, i, seq, base_local, n_nodes, mem)
            abs_addr = (dst_cid << 32) + local_off
            abs_addrs.add(abs_addr)
    assert len(abs_addrs) == n_nodes * n_txn


def test_alloc_raises_on_memory_size_overflow():
    """Fault injection: an offset that would exceed memory_size must raise ValueError.

    With n_nodes=16, stride=0x40, memory_size=0x1000, the tightest fit is 4 txn/node
    (16*4*64 = 4096 = memory_size).  A 5th transaction (seq=4) overflows and MUST be
    caught rather than corrupting the neighbouring dst tile.
    """
    n_nodes = 16
    base_local = 0x1000
    mem = 0x1000
    # seq=4 for the last source: offset-base = 15*64 + 4*16*64 = 960 + 4096 > 4096.
    raised = False
    try:
        alloc_unique_offset(0, 15, 4, base_local, n_nodes, mem)
    except ValueError:
        raised = True
    assert raised, "allocator must raise ValueError when offset exceeds memory_size"


def test_alloc_reserved_burst_tail_overflow_raises():
    """A slot that fits its base but whose reserved burst tail overruns must raise."""
    n_nodes = 16
    base_local = 0x1000
    mem = 0x1000
    # src=15, seq=3: offset-base = 15*64 + 3*16*64 = 4032; +0x40 reserved = 4096 == mem (OK).
    # Now demand a 0x80 burst tail: 4032 + 128 = 4160 > 4096 -> must raise.
    ok = alloc_unique_offset(0, 15, 3, base_local, n_nodes, mem, reserved=0x40)
    assert ok == base_local + 4032
    raised = False
    try:
        alloc_unique_offset(0, 15, 3, base_local, n_nodes, mem, reserved=0x80)
    except ValueError:
        raised = True
    assert raised, "allocator must account for the slot's reserved burst bytes"


# ---------------------------------------------------------------------------
# Integration: gen_test_patterns CLI (neighbor, 4x4 topology)
# ---------------------------------------------------------------------------

def _repo_root():
    return os.path.abspath(os.path.join(HERE, "..", ".."))


def _base_scenario():
    return os.path.join(_repo_root(), "sim", "test_patterns",
                        "AX4-BAS-003_single_write_read_aligned", "scenario.yaml")


def test_neighbor_variant_memory_base_at_src_coord():
    """Node i's variant has memory_base = coord(i)<<32 + base_local."""
    import subprocess
    with tempfile.TemporaryDirectory(dir=_repo_root()) as out:
        subprocess.run(
            [sys.executable,
             os.path.join(HERE, "gen_test_patterns.py"),
             "--from", _base_scenario(),
             "--pattern", "neighbor",
             "--topology", "mesh_4x4_vc1",
             "--out", out],
            check=True
        )
        for i in range(16):
            sc = yaml.safe_load(
                open(os.path.join(out, f"node{i}", "scenario.yaml"))
            )
            x, y = i % 4, i // 4
            src_cid = coord_id(x, y)
            expected_base = (src_cid << 32) + 0x1000
            actual_base = int(str(sc["config"]["memory_base"]), 0) \
                if isinstance(sc["config"]["memory_base"], str) \
                else sc["config"]["memory_base"]
            assert actual_base == expected_base, (
                f"node{i}: memory_base={actual_base:#x} != expected {expected_base:#x}"
            )


def test_neighbor_variant_addr_at_dst_coord():
    """Node i's write addr has bits[39:32] == coord(neighbor(i))."""
    import subprocess
    with tempfile.TemporaryDirectory(dir=_repo_root()) as out:
        subprocess.run(
            [sys.executable,
             os.path.join(HERE, "gen_test_patterns.py"),
             "--from", _base_scenario(),
             "--pattern", "neighbor",
             "--topology", "mesh_4x4_vc1",
             "--out", out],
            check=True
        )
        for i in range(16):
            sc = yaml.safe_load(
                open(os.path.join(out, f"node{i}", "scenario.yaml"))
            )
            x, y = i % 4, i // 4
            dx, dy = neighbor_dst(x, y, 4, 4)
            dst_cid = coord_id(dx, dy)
            for t in sc.get("transactions", []):
                addr = t["addr"]
                if isinstance(addr, str):
                    addr = int(addr, 0)
                actual_dst = (addr >> 32) & 0xFF
                assert actual_dst == dst_cid, (
                    f"node{i}: addr={addr:#x} dst_bits={actual_dst} != "
                    f"expected {dst_cid} (neighbor of ({x},{y}) = ({dx},{dy}))"
                )


# ---------------------------------------------------------------------------
# uniform_random_dsts: ported from booksim2 traffic.cpp:386-390
# ---------------------------------------------------------------------------

def test_uniform_excludes_self():
    """uniform_random must never return dst == src (default allow_self=False)."""
    rng = random.Random(1)
    n_nodes = 16
    for src in range(n_nodes):
        dsts = uniform_random_dsts(src, n_nodes, 200, rng)
        assert src not in dsts, f"src={src} appeared in its own dst list"


def test_uniform_covers_many_dsts():
    """uniform_random with 200 draws should cover well more than 5 distinct dsts."""
    rng = random.Random(1)
    dsts = uniform_random_dsts(5, 16, 200, rng)
    assert len(set(dsts)) > 5, "uniform_random must cover many distinct destinations"


def test_uniform_seeded_reproducible():
    """Same seed + args must produce identical dst sequence."""
    dsts_a = uniform_random_dsts(0, 16, 50, random.Random(42))
    dsts_b = uniform_random_dsts(0, 16, 50, random.Random(42))
    assert dsts_a == dsts_b


def test_uniform_allow_self():
    """--allow-self: dst == src must be possible."""
    rng = random.Random(7)
    found_self = False
    for _ in range(500):
        dsts = uniform_random_dsts(0, 2, 1, rng, allow_self=True)
        if dsts[0] == 0:
            found_self = True
            break
    assert found_self, "allow_self=True should eventually produce dst==src for a 2-node mesh"


# ---------------------------------------------------------------------------
# hotspot_dsts: ported from booksim2 traffic.cpp:506-526
# ---------------------------------------------------------------------------

def test_hotspot_single_always_returns_hotspot():
    """Single hotspot: every packet must go to that node (booksim fast path)."""
    rng = random.Random(1)
    dsts = hotspot_dsts(src_node=0, n_nodes=16, n_txn=50, rng=rng, hotspots=[7])
    assert all(d == 7 for d in dsts), "single hotspot must always produce that node"


def test_hotspot_concentrates_traffic():
    """With a single hotspot, traffic is 100% concentrated there regardless of n_txn."""
    rng = random.Random(99)
    hotspot = 3
    dsts = hotspot_dsts(0, 16, 100, rng, hotspots=[hotspot])
    counts = {d: dsts.count(d) for d in set(dsts)}
    assert counts[hotspot] == 100


def test_hotspot_multi_weighted():
    """Multi-hotspot: weighted rates should bias distribution accordingly."""
    rng = random.Random(42)
    # hotspot 0 has rate 9, hotspot 1 has rate 1 => ~90% to node 0
    dsts = hotspot_dsts(5, 16, 1000, rng, hotspots=[0, 1], rates=[9, 1])
    frac_0 = dsts.count(0) / len(dsts)
    assert frac_0 > 0.80, f"expected ~90% to hotspot 0, got {frac_0:.2%}"


def test_hotspot_equal_rates_default():
    """Default equal rates: both hotspots should each get ~50%."""
    rng = random.Random(13)
    dsts = hotspot_dsts(5, 16, 1000, rng, hotspots=[0, 2])
    frac_0 = dsts.count(0) / len(dsts)
    assert 0.40 < frac_0 < 0.60, f"expected ~50% to hotspot 0, got {frac_0:.2%}"


def test_hotspot_excludes_self_when_hotspot_is_src():
    """If the hotspot happens to be the source, self-exclusion must re-sample."""
    rng = random.Random(1)
    # hotspots=[0] but src=0 → must raise (no valid dst)
    import pytest
    with pytest.raises(ValueError, match="could not find a non-self dst"):
        hotspot_dsts(src_node=0, n_nodes=4, n_txn=1, rng=rng, hotspots=[0])


# ---------------------------------------------------------------------------
# Global write-address uniqueness (uniform_random + synthetic payload)
# ---------------------------------------------------------------------------

def _gen_all_synthetic(pattern, x_dim, y_dim, txn_per_node, seed,
                       hotspots=None, rates=None, allow_self=False,
                       base_local=0x1000, memory_size=None):
    """Helper: run gen_test_patterns CLI in synthetic mode and return parsed scenarios."""
    import subprocess
    import tempfile
    repo = os.path.abspath(os.path.join(HERE, "..", ".."))
    topo_name = f"mesh_{x_dim}x{y_dim}_vc1"
    if memory_size is None:
        # Use a large enough window for the given load.
        n_nodes = x_dim * y_dim
        memory_size = n_nodes * txn_per_node * 0x40 * 2  # 2x headroom
    with tempfile.TemporaryDirectory(dir=repo) as out:
        cmd = [
            sys.executable,
            os.path.join(HERE, "gen_test_patterns.py"),
            "--pattern", pattern,
            "--topology", topo_name,
            "--out", out,
            "--transactions-per-node", str(txn_per_node),
            "--seed", str(seed),
        ]
        if hotspots:
            cmd += ["--hotspot"] + [str(h) for h in hotspots]
        if rates:
            cmd += ["--hotspot-rates"] + [str(r) for r in rates]
        if allow_self:
            cmd.append("--allow-self")
        subprocess.run(cmd, check=True)
        n_nodes = x_dim * y_dim
        scenarios = []
        for i in range(n_nodes):
            sc = yaml.safe_load(open(os.path.join(out, f"node{i}", "scenario.yaml")))
            scenarios.append(sc)
    return scenarios


def test_global_addr_uniqueness_uniform():
    """All write addresses across all nodes must be globally unique for uniform_random."""
    scenarios = _gen_all_synthetic("uniform_random", 4, 4, txn_per_node=2, seed=1)
    write_addrs = [t["addr"] for sc in scenarios for t in sc["transactions"]
                   if t["op"] == "write"]
    assert len(write_addrs) == len(set(write_addrs)), (
        "uniform_random: two writes share the same absolute address"
    )


def test_write_read_pairs_share_addr():
    """For each pair, write and read must target the same address."""
    scenarios = _gen_all_synthetic("uniform_random", 4, 4, txn_per_node=2, seed=7)
    for sc in scenarios:
        txns = sc["transactions"]
        # Transactions are interleaved write/read pairs
        assert len(txns) % 2 == 0
        for i in range(0, len(txns), 2):
            assert txns[i]["op"] == "write"
            assert txns[i + 1]["op"] == "read"
            assert txns[i]["addr"] == txns[i + 1]["addr"], (
                "write/read pair must share the same address"
            )


# ---------------------------------------------------------------------------
# Guard: mesh capacity
# ---------------------------------------------------------------------------

def test_guard_mesh_exceeds_dst_capacity():
    """A 20x20 mesh exceeds DST_ID_WIDTH=8 (max 256 nodes); gen must exit(1)."""
    import subprocess
    import tempfile
    repo = os.path.abspath(os.path.join(HERE, "..", ".."))
    # Create a temporary topology YAML for a 20x20 mesh
    with tempfile.TemporaryDirectory(dir=repo) as tmp:
        topo_yaml = os.path.join(tmp, "mesh_20x20_vc1.yaml")
        with open(topo_yaml, "w") as f:
            yaml.safe_dump({"topology": {"name": "mesh_20x20_vc1",
                                          "x_dim": 20, "y_dim": 20, "num_vc": 1}}, f)
        # Temporarily symlink / copy to topologies dir so _load_topology finds it
        topo_dir = os.path.join(HERE, "..", "topologies")
        dest = os.path.join(topo_dir, "mesh_20x20_vc1.yaml")
        import shutil
        shutil.copy(topo_yaml, dest)
        try:
            result = subprocess.run(
                [sys.executable,
                 os.path.join(HERE, "gen_test_patterns.py"),
                 "--pattern", "uniform_random",
                 "--topology", "mesh_20x20_vc1",
                 "--out", os.path.join(tmp, "out"),
                 "--transactions-per-node", "1"],
                capture_output=True, text=True
            )
            assert result.returncode != 0, (
                "Expected non-zero exit for mesh exceeding DST_ID_WIDTH capacity"
            )
            assert "DST_ID_WIDTH" in result.stderr or "exceeds" in result.stderr, (
                f"Expected clear error message; stderr={result.stderr!r}"
            )
        finally:
            if os.path.exists(dest):
                os.remove(dest)


def test_guard_dst_id_width_value():
    """DST_ID_WIDTH exported constant must equal 8 (mirrors ni_flit_constants.h)."""
    assert DST_ID_WIDTH == 8


# ---------------------------------------------------------------------------
# Allocator memory_size overflow via CLI (too many transactions)
# ---------------------------------------------------------------------------

def test_allocator_overflow_via_cli():
    """Passing more --transactions-per-node than the memory window can hold must fail."""
    import subprocess
    import tempfile
    repo = os.path.abspath(os.path.join(HERE, "..", ".."))
    # mesh_4x4_vc1: n_nodes=16, default memory_size=0x1000, stride=0x40
    # max txn/node = 4 (16*4*0x40 == 0x1000 exact); 5 overflows
    with tempfile.TemporaryDirectory(dir=repo) as out:
        result = subprocess.run(
            [sys.executable,
             os.path.join(HERE, "gen_test_patterns.py"),
             "--pattern", "uniform_random",
             "--topology", "mesh_4x4_vc1",
             "--out", out,
             "--transactions-per-node", "5",
             "--seed", "0"],
            capture_output=True, text=True
        )
    assert result.returncode != 0, (
        "Expected non-zero exit when transactions-per-node exceeds memory window"
    )
    assert "memory" in result.stderr.lower() or "ValueError" in result.stderr, (
        f"Expected memory overflow error; stderr={result.stderr!r}"
    )
