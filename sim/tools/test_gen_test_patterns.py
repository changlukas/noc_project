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

from gen_test_patterns import (  # noqa: E402
    alloc_unique_offset,
    coord_id,
    neighbor_dst,
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
