#!/usr/bin/env python3
"""Generate per-node scenario variants from a base scenario.yaml + a traffic pattern.

Usage:
    gen_test_patterns.py --from <base.yaml> --pattern neighbor \\
        --topology <name> --out <dir>

    gen_test_patterns.py --pattern uniform_random \\
        --topology mesh_4x4_vc1 --out <dir> \\
        --transactions-per-node 4 --seed 42

Writes <out>/node<i>/scenario.yaml for each node i. Each variant:
  - Transaction addr = dst_coord<<32 + local_offset  (bit 32+ selects destination tile)
  - config.memory_base = coord(i)<<32 + base.memory_base  (slave at i serves its own tile)
  - data_file/dump_file/strb_file rewritten to relative paths from the variant subdir

Patterns
--------
neighbor  (ported from booksim2 NeighborTrafficPattern::dest, src/traffic.cpp:316)
    Per dimension: digit += 1 mod k.  For (x,y): dst = ((x+1)%x_dim, (y+1)%y_dim).
    Deterministic bijection; non-self when dim > 1.

uniform_random  (ported from booksim2 UniformRandomTrafficPattern::dest,
                 src/traffic.cpp:386-390)
    Each packet independently draws a uniformly random destination from [0, nodes-1].
    Self-traffic is PERMITTED by default (booksim-faithful); --exclude-self opts out.
    Uses random.Random(seed) for reproducibility.

hotspot  (ported from booksim2 HotSpotTrafficPattern::dest, src/traffic.cpp:506-526)
    Directs traffic to one or more hotspot nodes (--hotspot <linear-node-ids>).
    Single hotspot: all packets go to that node.
    Multiple hotspots: weighted selection by --hotspot-rates (default: equal weight).
    Uses random.Random(seed) for reproducibility.

Address allocation
------------------
alloc_unique_offset(dst_node, src_node, seq, base_offset, n_nodes, memory_size, ...)
    Assigns a globally unique local offset within the dst node's memory window so
    converging sources never collide on an absolute address.  The neighbor pattern
    is a bijection (no convergence), but the allocator contract is shared with
    future patterns (T2/T3 synthetic/uniform/hotspot/transpose).

        offset = base_offset + src_node * stride + seq * (n_nodes * stride)
    (row-major in seq: each src is one stride apart; each seq jumps a full
    n_nodes-wide band, so src + seq*n_nodes is injective for src,seq in [0,n_nodes)).
    The allocator ASSERTS the chosen offset + the slot's reserved bytes stays within
    [base_offset, base_offset + memory_size); a violation raises ValueError rather
    than silently overflowing into the next dst tile (the contract T2/T3 rely on).

Constants
---------
X_WIDTH = 4  -- mirrors c_model addr_trans.hpp / ni_flit_constants.h
ADDR_DST_SHIFT = 32  -- addr[63:32] carries dst_id (LOCAL_ADDR_BITS = 32)
DST_ID_WIDTH = 8  -- mirrors ni_flit_constants.h header::DST_ID_WIDTH (X_WIDTH + Y_WIDTH)
"""

import argparse
import copy
import os
import random as _random_module
import sys

import yaml

# Must mirror c_model addr_trans.hpp:
#   dst_id = (y << X_WIDTH) | x
#   addr[LOCAL_ADDR_BITS + DST_ID_BITS - 1 : LOCAL_ADDR_BITS] = y
#   addr[LOCAL_ADDR_BITS + X_WIDTH - 1 : LOCAL_ADDR_BITS] = x
X_WIDTH = 4
Y_WIDTH = 4          # mirrors ni_flit_constants.h width::Y_WIDTH
ADDR_DST_SHIFT = 32  # LOCAL_ADDR_BITS
DST_ID_WIDTH = 8     # header::DST_ID_WIDTH = X_WIDTH + Y_WIDTH; max nodes = 2**8 = 256
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


def uniform_random_dsts(src_node, n_nodes, n_txn, rng, exclude_self=False):
    """Booksim2 UniformRandomTrafficPattern::dest (traffic.cpp:386-390): uniform random node.

    Returns a list of n_txn destination linear node indices.  Each packet draws its
    own dst independently (per-packet random, not one dst per node).

    Self-traffic policy (booksim-faithful by default):
      - booksim `RandomInt(nodes-1)` returns a uniform node in [0, nodes-1] and
        PERMITS self (no source exclusion).  This is the default.
      - pass exclude_self=True to re-sample until dst != src (clean NoC-only
        measurement; opt-in via --exclude-self).
    """
    dsts = []
    for _ in range(n_txn):
        while True:
            d = rng.randint(0, n_nodes - 1)
            if not exclude_self or d != src_node:
                break
        dsts.append(d)
    return dsts


def hotspot_dsts(src_node, n_nodes, n_txn, rng, hotspots, rates=None, exclude_self=False):
    """Booksim2 HotSpotTrafficPattern::dest (traffic.cpp:506-526): weighted hotspot selection.

    hotspots: list of linear node indices (0..n_nodes-1).
    rates:    weights parallel to hotspots (default: all 1, i.e. equal weight).
              Must be positive integers.

    Single hotspot: all packets go to that node (booksim fast path, traffic.cpp:510;
    returns the hotspot UNCONDITIONALLY -- even when src == hotspot).
    Multiple hotspots: weighted cumulative selection (traffic.cpp:514-525); booksim
    applies NO source exclusion.

    Self-traffic policy (booksim-faithful by default): permit self.  Pass
    exclude_self=True (--exclude-self) to re-sample until dst != src; if no non-self
    dst exists (e.g. single hotspot == src) the selection falls back to the
    booksim-faithful value rather than raising.
    """
    if not hotspots:
        raise ValueError("hotspot pattern requires at least one --hotspot node id")
    for h in hotspots:
        if not (0 <= h < n_nodes):
            raise ValueError(f"hotspot node id {h} out of range [0, {n_nodes})")

    if rates is None:
        rates = [1] * len(hotspots)
    if len(rates) != len(hotspots):
        raise ValueError("--hotspot-rates length must match --hotspot length")
    for r in rates:
        if r <= 0:
            raise ValueError(f"hotspot rate {r} must be positive")

    max_val = sum(rates) - 1  # mirrors booksim _max_val accumulation
    # True only if every hotspot equals src -- then exclude_self cannot succeed and we
    # fall back to the booksim-faithful value (no raise; booksim never raises).
    all_hotspots_are_self = all(h == src_node for h in hotspots)

    def _select():
        if len(hotspots) == 1:
            # booksim fast path: single hotspot -> return it directly (traffic.cpp:510)
            return hotspots[0]
        # booksim weighted cumulative select (traffic.cpp:514-525)
        pct = rng.randint(0, max_val)
        for i in range(len(hotspots) - 1):
            if rates[i] > pct:
                return hotspots[i]
            pct -= rates[i]
        return hotspots[-1]  # mirrors booksim assert-backed fallthrough

    dsts = []
    for _ in range(n_txn):
        d = _select()
        if exclude_self and not all_hotspots_are_self:
            # Re-sample until non-self (bounded; only meaningful for multi-hotspot).
            while d == src_node:
                d = _select()
        dsts.append(d)
    return dsts


def _linear_to_coord(node, x_dim):
    """Convert linear node index to (x, y) mesh coordinates."""
    return node % x_dim, node // x_dim


# ---------------------------------------------------------------------------
# Global unique-offset allocator
# ---------------------------------------------------------------------------

def alloc_unique_offset(dst_node, src_node, seq, base_offset, n_nodes,
                        memory_size, reserved=_SLOT_STRIDE, stride=_SLOT_STRIDE):
    """Return a local offset that is globally unique across all (src_node, seq) pairs.

    Layout within the dst node's memory window (row-major in seq, column in src):
        offset = base_offset + src_node * stride + seq * (n_nodes * stride)

    This guarantees uniqueness: the value src_node + seq * n_nodes is injective
    for all (src_node in [0, n_nodes), seq in [0, n_nodes)).

    Bounds: the chosen offset plus the slot's reserved bytes must stay within the
    dst tile's memory window [base_offset, base_offset + memory_size).  A violation
    raises ValueError instead of silently overflowing into the next dst tile.  The
    formula fits when n_nodes * max_seq_per_src * stride <= memory_size; for the
    default mesh_4x4 (n_nodes=16, memory_size=0x1000, stride=0x40) this allows up
    to 4 transactions per node (16*4*64 = 4096 = memory_size, the tightest current
    case — AX4-BAS-005's 4 writes).  T2's --transactions-per-node beyond that
    trips the assertion rather than corrupting a neighbouring tile.

    Args:
        dst_node: linear node index of the destination (informational only — not
                  used in the formula; the caller encodes it in addr bits 32+).
        src_node: linear index of the sending node.
        seq:      0-based transaction-pair index within this src_node's sequence.
        base_offset: base local address from the scenario (memory_base & 0xFFFFFFFF).
        n_nodes:  total node count in the topology (upper bound on src_node and seq).
        memory_size: dst tile's memory window size (config.memory_size); the offset
                  + reserved must stay below base_offset + memory_size.
        reserved: bytes the slot occupies (default one slot = stride); for a burst,
                  pass the burst's total byte length so the tail also fits.
        stride:   byte step between adjacent slots (default _SLOT_STRIDE = 0x40).

    Raises:
        ValueError: if offset + reserved would exceed base_offset + memory_size.
    """
    _ = dst_node  # unused in formula; kept for caller clarity and T2/T3 reuse
    offset = base_offset + src_node * stride + seq * (n_nodes * stride)
    if (offset - base_offset) + reserved > memory_size:
        raise ValueError(
            f"alloc_unique_offset: local offset {offset:#x} (+{reserved:#x} reserved) "
            f"exceeds memory window [{base_offset:#x}, {base_offset + memory_size:#x}) "
            f"(memory_size={memory_size:#x}); reduce transactions-per-node or "
            f"enlarge memory_size"
        )
    return offset


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
# Mesh capacity guard
# ---------------------------------------------------------------------------

def _check_mesh_capacity(x_dim, y_dim):
    """Fail fast if the mesh exceeds the dst_id address space.

    coord_id = (y << X_WIDTH) | x, so the encoding requires PER-DIMENSION fit:
      - x_dim <= 2**X_WIDTH  (else x aliases into the y field)
      - y_dim <= 2**Y_WIDTH  (else y overflows DST_ID_WIDTH)
      - x_dim * y_dim <= 2**DST_ID_WIDTH  (total node count fits dst_id)
    The product check alone misses e.g. 17x15 (=255 <= 256 but x_dim 17 > 16 aliases)
    or 32x8 (=256 but x_dim 32 > 16).  All three must hold (spec section 3 invariant).
    """
    if x_dim > 2 ** X_WIDTH:
        sys.exit(
            f"ERROR: x_dim={x_dim} exceeds X_WIDTH={X_WIDTH} capacity "
            f"({2**X_WIDTH} columns max); x would alias into the y field of coord_id. "
            f"Reduce x_dim."
        )
    if y_dim > 2 ** Y_WIDTH:
        sys.exit(
            f"ERROR: y_dim={y_dim} exceeds Y_WIDTH={Y_WIDTH} capacity "
            f"({2**Y_WIDTH} rows max); y would overflow DST_ID_WIDTH. Reduce y_dim."
        )
    if x_dim * y_dim > 2 ** DST_ID_WIDTH:
        sys.exit(
            f"ERROR: mesh {x_dim}x{y_dim} = {x_dim * y_dim} nodes exceeds "
            f"DST_ID_WIDTH={DST_ID_WIDTH} capacity ({2**DST_ID_WIDTH} nodes max). "
            f"Reduce x_dim or y_dim."
        )


# ---------------------------------------------------------------------------
# Pattern dispatch
# ---------------------------------------------------------------------------

def _dst_for(pattern, x, y, x_dim, y_dim):
    """Return (dst_x, dst_y) for the given pattern and source coordinates (deterministic)."""
    if pattern == "neighbor":
        return neighbor_dst(x, y, x_dim, y_dim)
    raise ValueError(f"Unknown pattern: {pattern!r} (use per-packet sampler for random patterns)")


# ---------------------------------------------------------------------------
# Per-node variant emitter
# ---------------------------------------------------------------------------

def _as_int(v):
    return v if isinstance(v, int) else int(str(v), 0)


def _emit_node(base_sc, src_dir, out_dir, src_idx, dst_cid, src_cid,
               n_nodes, base_local, memory_size):
    """Emit <out_dir>/scenario.yaml for one node.

    base_sc     -- base scenario dict (read-only; deep-copied internally)
    src_dir     -- directory of the base scenario.yaml (for relative file rewrites)
    out_dir     -- destination directory for this node's variant
    src_idx     -- linear index of the source node (for alloc_unique_offset)
    dst_cid     -- coord_id of the destination tile (encodes into addr bits 32+)
    src_cid     -- coord_id of the source node (sets config.memory_base)
    n_nodes     -- total node count (allocator domain)
    base_local  -- base local address from config.memory_base & 0xFFFF_FFFF
    memory_size -- dst tile memory window size (config.memory_size); allocator bound
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
                                                  n_nodes, memory_size)
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


def _emit_synthetic_node(out_dir, src_idx, dst_cids, src_cid,
                         n_nodes, base_local, memory_size, axi_size, axi_len):
    """Emit <out_dir>/scenario.yaml with synthetic write+read pairs (no base scenario).

    dst_cids    -- list of per-transaction dst coord_ids (one per pair)
    axi_size    -- AxSIZE field (0..7; bytes per beat = 2**axi_size)
    axi_len     -- AxLEN field (0..255; beats per burst; 0 = single beat)

    Burst footprint = (axi_len + 1) * 2**axi_size bytes; passed as `reserved` to
    alloc_unique_offset so the tail also fits within the dst tile memory window.
    """
    reserved = (axi_len + 1) * (1 << axi_size)
    transactions = []
    for seq, dst_cid in enumerate(dst_cids):
        local_off = alloc_unique_offset(dst_cid, src_idx, seq, base_local,
                                        n_nodes, memory_size, reserved=reserved)
        addr = (dst_cid << ADDR_DST_SHIFT) + local_off
        transactions.append({"op": "write", "addr": addr, "size": axi_size, "len": axi_len})
        transactions.append({"op": "read",  "addr": addr, "size": axi_size, "len": axi_len})

    sc = {
        "config": {
            "memory_base": (src_cid << ADDR_DST_SHIFT) + base_local,
            "memory_size": memory_size,
        },
        "transactions": transactions,
    }
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
    ap.add_argument("--from", dest="base", default=None,
                    help="Base scenario.yaml path (omit for synthetic payload mode; for "
                         "random patterns it only borrows the memory shape)")
    ap.add_argument("--pattern", required=True,
                    choices=["neighbor", "uniform_random", "hotspot"],
                    help="Traffic pattern")
    ap.add_argument("--topology", default="mesh_4x4_vc1",
                    help="Topology name (matches sim/topologies/<name>.yaml)")
    ap.add_argument("--out", required=True,
                    help="Output directory; writes <out>/node<i>/scenario.yaml")
    # Per-packet random pattern options
    ap.add_argument("--transactions-per-node", type=int, default=1,
                    help="Write+read pairs per node (synthetic / random patterns)")
    ap.add_argument("--seed", type=int, default=0,
                    help="RNG seed for reproducibility (uniform_random / hotspot)")
    ap.add_argument("--exclude-self", action="store_true",
                    help="Exclude dst == src (opt-in; default permits self, "
                         "booksim-faithful)")
    # Hotspot options
    ap.add_argument("--hotspot", type=int, nargs="+", default=None,
                    help="Linear node id(s) for hotspot pattern (0..N-1)")
    ap.add_argument("--hotspot-rates", type=int, nargs="+", default=None,
                    help="Weights for each hotspot (parallel to --hotspot; default: equal)")
    # Synthetic payload shape (only used when --from is absent)
    ap.add_argument("--size", type=int, default=2,
                    help="AxSIZE for synthetic transactions (0..7; default 2 = 4 bytes)")
    ap.add_argument("--len", type=int, default=0, dest="burst_len",
                    help="AxLEN for synthetic transactions (0..255; default 0 = single beat)")
    ap.add_argument("--memory-size", type=lambda v: int(str(v), 0), default=None,
                    help="dst tile memory window size (default: from --from scenario "
                         "config.memory_size, else 0x1000); sizes the allocator bound "
                         "and the emitted config.memory_size")
    a = ap.parse_args(argv)

    nodes, x_dim, y_dim = _load_topology(a.topology)
    _check_mesh_capacity(x_dim, y_dim)
    n_nodes = len(nodes)

    if a.pattern == "neighbor":
        # Deterministic bijection: uses base scenario; --from required.
        if a.base is None:
            ap.error("--from is required for the neighbor pattern")
        src_path = os.path.abspath(a.base)
        src_dir = os.path.dirname(src_path)
        with open(src_path) as f:
            base_sc = yaml.safe_load(f)
        cfg = base_sc.get("config", {})
        base_local = _as_int(cfg.get("memory_base", 0)) & 0xFFFFFFFF
        memory_size = _as_int(cfg.get("memory_size", 0x1000))
        if a.memory_size is not None:
            memory_size = a.memory_size  # explicit CLI override wins

        for (idx, x, y, src_cid) in nodes:
            dst_x, dst_y = _dst_for(a.pattern, x, y, x_dim, y_dim)
            dst_cid = coord_id(dst_x, dst_y)
            out_dir = os.path.join(a.out, f"node{idx}")
            _emit_node(base_sc, src_dir, out_dir, idx, dst_cid, src_cid,
                       n_nodes, base_local, memory_size)

    else:
        # Per-packet random pattern (uniform_random / hotspot): synthetic payload.
        # --size / --len / --transactions-per-node govern output; --from (if given) only
        # borrows the memory shape (memory_base / memory_size).
        rng = _random_module.Random(a.seed)
        base_local = 0x1000  # default local base for synthetic scenarios
        memory_size = 0x1000  # default dst tile window (same as existing topologies)
        if a.base is not None:
            # Borrow memory shape from a base scenario.
            with open(os.path.abspath(a.base)) as f:
                base_sc = yaml.safe_load(f)
            cfg = base_sc.get("config", {})
            base_local = _as_int(cfg.get("memory_base", base_local)) & 0xFFFFFFFF
            memory_size = _as_int(cfg.get("memory_size", memory_size))
        if a.memory_size is not None:
            memory_size = a.memory_size  # explicit CLI override wins

        for (idx, x, y, src_cid) in nodes:
            if a.pattern == "uniform_random":
                dst_linears = uniform_random_dsts(
                    idx, n_nodes, a.transactions_per_node, rng, a.exclude_self
                )
            else:  # hotspot
                if a.hotspot is None:
                    ap.error("--hotspot is required for the hotspot pattern")
                dst_linears = hotspot_dsts(
                    idx, n_nodes, a.transactions_per_node, rng,
                    a.hotspot, a.hotspot_rates, a.exclude_self
                )

            # Convert linear dst indices → coord_ids
            dst_cids = [coord_id(*_linear_to_coord(d, x_dim)) for d in dst_linears]
            out_dir = os.path.join(a.out, f"node{idx}")
            _emit_synthetic_node(out_dir, idx, dst_cids, src_cid,
                                  n_nodes, base_local, memory_size,
                                  a.size, a.burst_len)


if __name__ == "__main__":
    main(sys.argv[1:])
