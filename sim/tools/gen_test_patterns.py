#!/usr/bin/env python3
"""Generate per-node scenario variants from a base scenario.yaml + a traffic pattern.

Usage:
    gen_test_patterns.py --from <base.yaml> --pattern neighbor \\
        --topology <name> --out <dir>

Writes <out>/node<i>/scenario.yaml for each node i. Each variant:
  - Transaction addr = dst_coord<<32 + local_offset  (bit 32+ selects destination tile)
  - config.memory_base = coord(i)<<32 + base.memory_base  (slave at i serves its own tile)
  - data_file/dump_file/strb_file rewritten to relative paths from the variant subdir

Patterns
--------
neighbor  (ported from booksim2 NeighborTrafficPattern::dest, src/traffic.cpp:316)
    Per dimension: digit += 1 mod k.  For (x,y): dst = ((x+1)%x_dim, (y+1)%y_dim).
    Deterministic bijection; non-self when dim > 1.

Address allocation
------------------
alloc_unique_offset(dst_node, src_node, seq, base_offset, stride, n_nodes)
    Assigns a globally unique local offset within the dst node's memory window so
    converging sources never collide on an absolute address.  The neighbor pattern
    is a bijection (no convergence), but the allocator contract is shared with
    future patterns (T2/T3 synthetic/uniform/hotspot/transpose).

        offset = base_offset + (src_node * stride_per_src + seq * stride)
    where stride_per_src = n_nodes * stride  (at most n_nodes sources per dst).

Constants
---------
X_WIDTH = 4  -- mirrors c_model addr_trans.hpp / ni_flit_constants.h
ADDR_DST_SHIFT = 32  -- addr[63:32] carries dst_id (LOCAL_ADDR_BITS = 32)
"""

import argparse
import copy
import os
import sys

import yaml

# Must mirror c_model addr_trans.hpp:
#   dst_id = (y << X_WIDTH) | x
#   addr[LOCAL_ADDR_BITS + DST_ID_BITS - 1 : LOCAL_ADDR_BITS] = y
#   addr[LOCAL_ADDR_BITS + X_WIDTH - 1 : LOCAL_ADDR_BITS] = x
X_WIDTH = 4
ADDR_DST_SHIFT = 32  # LOCAL_ADDR_BITS
_FILE_KEYS = ("data_file", "dump_file", "strb_file")

# Per-transaction slot stride for the unique-offset allocator.  Must be at least
# as large as the max transaction data payload (one cache-line = 64 B = 0x40).
_SLOT_STRIDE = 0x40


# ---------------------------------------------------------------------------
# Coordinate helpers
# ---------------------------------------------------------------------------

def coord_id(x, y):
    """Coordinate-encoded node id = (y << X_WIDTH) | x.  Mirrors addr_trans.xy_route."""
    return (y << X_WIDTH) | x


def neighbor_dst(x, y, x_dim, y_dim):
    """Booksim2 NeighborTrafficPattern::dest (traffic.cpp:316): +1 per dimension, wrap.

    Returns (dst_x, dst_y).  Deterministic bijection; non-self when x_dim > 1 and
    y_dim > 1.
    """
    return (x + 1) % x_dim, (y + 1) % y_dim


# ---------------------------------------------------------------------------
# Global unique-offset allocator
# ---------------------------------------------------------------------------

def alloc_unique_offset(dst_node, src_node, seq, base_offset, n_nodes,
                        stride=_SLOT_STRIDE):
    """Return a local offset that is globally unique across all (src_node, seq) pairs.

    Layout within the dst node's memory window (row-major in seq, column in src):
        offset = base_offset + src_node * stride + seq * (n_nodes * stride)

    This guarantees uniqueness: the value src_node + seq * n_nodes is injective
    for all (src_node ∈ [0, n_nodes), seq ∈ [0, n_nodes)).  The maximum offset
    above base_offset is (n_nodes-1)*stride + (n_nodes-1)*(n_nodes*stride) which
    is bounded by n_nodes^2 * stride.  For n_nodes=16 and stride=64 (=0x40) the
    max spread is 16*16*64 = 16384 = 0x4000, which fits comfortably in the
    scenarios' default memory_size=0x1000 per slot class when only a small number
    of (src, seq) pairs are active per dst.

    Practical bound: the formula fits within memory_size when
        n_nodes * max_seq_per_src * stride <= memory_size.
    For the default mesh_4x4 (n_nodes=16) with memory_size=0x1000=4096 and
    stride=0x40=64, this allows up to 4 transactions per node without overflow:
        16 * 4 * 64 = 4096.  AX4-BAS-005 (4 writes) is the tightest current case.

    Args:
        dst_node: linear node index of the destination (informational only — not
                  used in the formula; the caller encodes it in addr bits 32+).
        src_node: linear index of the sending node.
        seq:      0-based transaction-pair index within this src_node's sequence.
        base_offset: base local address from the scenario (memory_base & 0xFFFFFFFF).
        n_nodes:  total node count in the topology (upper bound on src_node and seq).
        stride:   byte step between adjacent slots (default _SLOT_STRIDE = 0x40).
    """
    _ = dst_node  # unused in formula; kept for caller clarity and T2/T3 reuse
    return base_offset + src_node * stride + seq * (n_nodes * stride)


# ---------------------------------------------------------------------------
# Topology loader
# ---------------------------------------------------------------------------

def _load_topology(name):
    """Return (nodes, x_dim, y_dim) where nodes = [(idx, x, y, cid), ...]."""
    here = os.path.dirname(os.path.abspath(__file__))
    topo_path = os.path.join(here, "..", "topologies", f"{name}.yaml")
    with open(topo_path) as f:
        topo = yaml.safe_load(f)
    x_dim = topo["topology"]["x_dim"]
    y_dim = topo["topology"]["y_dim"]
    nodes = []
    idx = 0
    for y in range(y_dim):
        for x in range(x_dim):
            nodes.append((idx, x, y, coord_id(x, y)))
            idx += 1
    return nodes, x_dim, y_dim


# ---------------------------------------------------------------------------
# Pattern dispatch
# ---------------------------------------------------------------------------

def _dst_for(pattern, x, y, x_dim, y_dim):
    """Return (dst_x, dst_y) for the given pattern and source coordinates."""
    if pattern == "neighbor":
        return neighbor_dst(x, y, x_dim, y_dim)
    raise ValueError(f"Unknown pattern: {pattern!r}")


# ---------------------------------------------------------------------------
# Per-node variant emitter
# ---------------------------------------------------------------------------

def _as_int(v):
    return v if isinstance(v, int) else int(str(v), 0)


def _emit_node(base_sc, src_dir, out_dir, src_idx, dst_cid, src_cid,
               n_nodes, base_local):
    """Emit <out_dir>/scenario.yaml for one node.

    base_sc     -- base scenario dict (read-only; deep-copied internally)
    src_dir     -- directory of the base scenario.yaml (for relative file rewrites)
    out_dir     -- destination directory for this node's variant
    src_idx     -- linear index of the source node (for alloc_unique_offset)
    dst_cid     -- coord_id of the destination tile (encodes into addr bits 32+)
    src_cid     -- coord_id of the source node (sets config.memory_base)
    n_nodes     -- total node count (allocator domain)
    base_local  -- base local address from config.memory_base & 0xFFFF_FFFF
    """
    sc = copy.deepcopy(base_sc)

    # Assign unique offsets to write/read pairs.  Pair identity = same original addr.
    # Two-pass: first build a mapping from original addr -> allocated offset, then
    # rewrite all transactions sharing that addr to the same new offset.
    orig_addrs = [_as_int(t["addr"]) & 0xFFFFFFFF
                  for t in sc.get("transactions", [])]
    seen_orig: dict = {}  # orig_local_addr -> seq index
    pair_offset: dict = {}  # orig_local_addr -> allocated local_offset
    seq = 0
    for oa in orig_addrs:
        if oa not in seen_orig:
            seen_orig[oa] = seq
            # dst_node arg is informational; pass dst_cid as a proxy (not used in formula)
            pair_offset[oa] = alloc_unique_offset(dst_cid, src_idx, seq, base_local,
                                                  n_nodes)
            seq += 1

    for t in sc.get("transactions", []):
        orig = _as_int(t["addr"]) & 0xFFFFFFFF
        local_off = pair_offset[orig]
        t["addr"] = (dst_cid << ADDR_DST_SHIFT) + local_off

        for k in _FILE_KEYS:
            if t.get(k) and not os.path.isabs(t[k]):
                abs_src = os.path.join(src_dir, t[k])
                try:
                    rel = os.path.relpath(abs_src, out_dir)
                except ValueError:
                    # Different Windows drives: emit absolute path instead.
                    rel = os.path.abspath(abs_src)
                t[k] = rel.replace(os.sep, "/")

    cfg = sc.setdefault("config", {})
    cfg["memory_base"] = (src_cid << ADDR_DST_SHIFT) + base_local

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "scenario.yaml"), "w") as f:
        yaml.safe_dump(sc, f, sort_keys=False)


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate per-node scenario variants from a base scenario + traffic pattern."
    )
    ap.add_argument("--from", dest="base", required=True,
                    help="Base scenario.yaml path")
    ap.add_argument("--pattern", required=True,
                    choices=["neighbor"],
                    help="Traffic pattern")
    ap.add_argument("--topology", default="mesh_4x4_vc1",
                    help="Topology name (matches sim/topologies/<name>.yaml)")
    ap.add_argument("--out", required=True,
                    help="Output directory; writes <out>/node<i>/scenario.yaml")
    a = ap.parse_args(argv)

    src_path = os.path.abspath(a.base)
    src_dir = os.path.dirname(src_path)
    with open(src_path) as f:
        base_sc = yaml.safe_load(f)

    nodes, x_dim, y_dim = _load_topology(a.topology)
    n_nodes = len(nodes)
    base_local = _as_int(base_sc.get("config", {}).get("memory_base", 0)) & 0xFFFFFFFF

    for (idx, x, y, src_cid) in nodes:
        dst_x, dst_y = _dst_for(a.pattern, x, y, x_dim, y_dim)
        dst_cid = coord_id(dst_x, dst_y)
        out_dir = os.path.join(a.out, f"node{idx}")
        _emit_node(base_sc, src_dir, out_dir, idx, dst_cid, src_cid,
                   n_nodes, base_local)


if __name__ == "__main__":
    main(sys.argv[1:])
