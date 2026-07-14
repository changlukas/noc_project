#!/usr/bin/env python3
"""Emit per-node pulp axi_file_master stimulus (write.txt/read.txt) for a traffic pattern.

Usage:
    gen_test_patterns.py --pattern neighbor \\
        --topology <name> --out <dir>

    gen_test_patterns.py --pattern uniform_random \\
        --topology mesh_4x4_vc1 --out <dir> \\
        --transactions-per-node 4 --seed 42

Writes <out>/node<i>/{write,read}.txt for each node i. Each transaction:
  - addr = dst_coord * tile_size + local_offset  (tile_size from the topology's
    address_map.tile_size; defaults to 0x100000000 / 4 GB, matching the legacy
    dst_coord<<32 layout, if the block or key is absent)
  - src-partitioned local offset (alloc_unique_offset) so converging sources never collide
  - INCR, atop=0, full strobe, address-in-data payload (byte A = A & 0xFF)

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

transpose  (ported from booksim2 TransposeTrafficPattern::dest, src/traffic.cpp:244-250)
    Bit-half-swap of the node id.  On a square k×k mesh (k a power of two) this is
    equivalent to (x,y)→(y,x).  Requires x_dim == y_dim and x_dim a power of two.
    Diagonal nodes (x==y) are self-traffic; permitted by default (booksim-faithful).
    Each node's dst is fixed; all transactions_per_node pairs target the same dst.

hotspot  (ported from booksim2 HotSpotTrafficPattern::dest, src/traffic.cpp:506-526)
    Directs traffic to one or more hotspot nodes (--hotspot <linear-node-ids>).
    Single hotspot: all packets go to that node.
    Multiple hotspots: weighted selection by --hotspot-rates (default: equal weight).
    Uses random.Random(seed) for reproducibility.

Address allocation
------------------
alloc_unique_offset(dst_node, src_node, seq, base_offset, n_nodes, region_bytes, ...)
    Assigns a globally unique local offset within the dst node's memory window so
    converging sources never collide on an absolute address.  The neighbor pattern
    is a bijection (no convergence), but the allocator contract is shared with
    future patterns (T2/T3 synthetic/uniform/hotspot/transpose).

        offset = base_offset + src_node * stride + seq * (n_nodes * stride)
    (row-major in seq: each src is one stride apart; each seq jumps a full
    n_nodes-wide band, so src + seq*n_nodes is injective for src,seq in [0,n_nodes)).
    The allocator ASSERTS the chosen offset + the slot's reserved bytes stays within
    [base_offset, base_offset + region_bytes); a violation raises ValueError rather
    than silently overflowing into the next dst tile (the contract T2/T3 rely on).
    region_bytes is auto-derived in main() as n_nodes * transactions_per_node * stride
    (stride = max(_SLOT_STRIDE, burst_footprint)), a tight upper bound on the
    allocator's max offset + reserved, so the ValueError never fires in practice.

Constants
---------
X_WIDTH = 4  -- mirrors c_model addr_trans.hpp / ni_flit_constants.h
DST_ID_WIDTH = 8  -- mirrors ni_flit_constants.h header::DST_ID_WIDTH (X_WIDTH + Y_WIDTH)

Per-tile base address = dst_id * tile_size, where tile_size is read from the topology
YAML's address_map.tile_size (default 0x100000000 = 4 GB if the block or key is
absent). Mirrors c_model SamTable::uniform's base formula (addr_trans.hpp).
"""

import argparse
import os
import random as _random_module
import sys
from pathlib import Path

import yaml

# Must mirror c_model addr_trans.hpp SamTable::uniform:
#   dst_id = (y << X_WIDTH) | x
#   base(dst_id) = dst_id * tile_size   (tile_size from topology address_map.tile_size)
X_WIDTH = 4
Y_WIDTH = 4          # mirrors ni_flit_constants.h width::Y_WIDTH
DST_ID_WIDTH = 8     # header::DST_ID_WIDTH = X_WIDTH + Y_WIDTH; max nodes = 2**8 = 256
_FILE_KEYS = ("data_file", "dump_file", "strb_file")

# Default per-tile stride when the topology has no address_map.tile_size; matches the
# legacy dst_cid<<32 layout (addr[63:32] = dst_id) byte-for-byte.
_DEFAULT_TILE_SIZE = 0x100000000

# Per-transaction slot stride for the unique-offset allocator.  Must be at least
# as large as the max transaction data payload (one cache-line = 64 B = 0x40).
_SLOT_STRIDE = 0x40

_CONSTANTS_YAML = Path(__file__).resolve().parents[2] / "specgen" / "source" / "constants.yaml"


def axi_widths():
    """AXI ID/ADDR/DATA widths from the DUT single source of truth
    (specgen/source/constants.yaml -- the same file ni_params_pkg is generated from).
    Read directly (values only); the specgen validator owns schema checking."""
    axi = yaml.safe_load(_CONSTANTS_YAML.read_text(encoding="utf-8"))["axi"]
    return {
        "id":   int(axi["ID_WIDTH"]["default"]),
        "addr": int(axi["ADDR_WIDTH"]["default"]),
        "data": int(axi["DATA_WIDTH"]["default"]),
    }


def encode_write_beats(addr, axi_size, axi_len, data_width):
    """file_master W-beat lines: "0x<data> 0x<strb> 0", INCR, full strobe,
    address-in-data (byte A = A & 0xFF). data/strb sized to the DW bus."""
    bus_bytes = data_width // 8
    beat_bytes = 1 << axi_size
    if beat_bytes > bus_bytes:
        raise ValueError(
            f"axi_size={axi_size} (beat {beat_bytes} B) exceeds the {data_width}-bit "
            f"data bus ({bus_bytes} B); narrow the burst or widen DATA_WIDTH")
    lines = []
    for b in range(axi_len + 1):
        beat_addr = addr + b * beat_bytes
        lane0 = beat_addr % bus_bytes            # byte-lane of the beat's first byte
        data = 0
        strb = 0
        for k in range(beat_bytes):
            lane = lane0 + k
            data |= ((beat_addr + k) & 0xFF) << (8 * lane)
            strb |= 1 << lane
        lines.append(f"0x{data:0{bus_bytes * 2}x} 0x{strb:0{bus_bytes // 4}x} 0")
    return lines


def _ax_fields(axid, addr, axi_len, axi_size, include_atop):
    """The AW/AR field lines in parse_write/parse_read order. Write includes atop
    (12 fields); read omits it (11 fields, matching axi_file_master.parse_read)."""
    lines = [str(axid), f"0x{addr:x}", str(axi_len), str(axi_size),
             "1", "0", "0", "0", "0", "0"]          # burst=INCR lock cache prot qos region
    if include_atop:
        lines.append("0")                            # atop (write only)
    lines.append("0")                                # user
    return lines


def emit_file_master_node(out_dir, src_idx, dst_cids, n_nodes,
                          base_local, region_bytes, axi_size, axi_len, data_width,
                          ids_per_tile=1, num_axi_ids=256, tile_size=_DEFAULT_TILE_SIZE):
    """Write out_dir/{write,read}.txt for one node. One write+read pair per dst_cid,
    src-partitioned address, address-in-data payload. INCR, atop=0, full strobe.

    AXI ids: each tile owns an independent, non-overlapping block of `ids_per_tile`
    ids starting at src_idx*ids_per_tile (mod num_axi_ids), so no two tiles share
    an id. ids_per_tile=1 gives each tile one distinct id (= src_idx); >1 lets a
    tile keep several transactions outstanding (distinct ids escape same-id
    ordering), raising injected concurrency for saturation runs. VC allocation is
    id-agnostic (VC id only), so this changes concurrency, not VC spread."""
    os.makedirs(out_dir, exist_ok=True)
    reserved = (axi_len + 1) * (1 << axi_size)
    id_base = (src_idx * ids_per_tile) % num_axi_ids
    write_lines, read_lines = [], []
    for seq, dst_cid in enumerate(dst_cids):
        local_off = alloc_unique_offset(dst_cid, src_idx, seq, base_local,
                                        n_nodes, region_bytes, reserved=reserved)
        addr = dst_cid * tile_size + local_off
        axid = (id_base + (seq % ids_per_tile)) % num_axi_ids
        write_lines += _ax_fields(axid, addr, axi_len, axi_size, include_atop=True)
        write_lines += encode_write_beats(addr, axi_size, axi_len, data_width)
        read_lines += _ax_fields(axid, addr, axi_len, axi_size, include_atop=False)
    with open(os.path.join(out_dir, "write.txt"), "w") as f:
        f.write("\n".join(write_lines) + "\n")
    with open(os.path.join(out_dir, "read.txt"), "w") as f:
        f.write("\n".join(read_lines) + "\n")


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


def transpose_dst(x, y):
    """Booksim2 TransposeTrafficPattern::dest (traffic.cpp:244): bit-half-swap of node id.

    On a square k×k mesh (k a power of two), booksim's row-major node id y*k+x has
    equal-width x and y fields; the bit-half-swap is equivalent to swapping the two
    coordinate fields, giving dst = (y, x).

    Caller MUST have already validated that x_dim == y_dim and x_dim is a power of two
    (see _check_transpose_guard).  Diagonal nodes (x==y) map to themselves, which is
    booksim-faithful (booksim does NOT special-case self-traffic).

    Returns (dst_x, dst_y).
    """
    return y, x


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
                        region_bytes, reserved=_SLOT_STRIDE, stride=_SLOT_STRIDE):
    """Return a local offset that is globally unique across all (src_node, seq) pairs.

    Layout within the dst node's memory window (row-major in seq, column in src):
        stride = max(stride, reserved)   # a slot must be at least its footprint
        offset = base_offset + src_node * stride + seq * (n_nodes * stride)

    Uniqueness AND disjointness: distinct (src_node, seq) map to distinct slots
    spaced by `stride`, and since stride >= reserved each slot's footprint fits
    inside its own spacing — no two slots overlap even under many-to-one traffic.
    A burst footprint larger than the default _SLOT_STRIDE therefore widens the
    stride (callers size region_bytes to n_nodes * n_seq * stride to hold them all).

    Bounds: the chosen offset plus the slot's reserved bytes must stay within the
    dst tile's memory window [base_offset, base_offset + region_bytes).  A violation
    raises ValueError instead of silently overflowing into the next dst tile.

    Args:
        dst_node: linear node index of the destination (informational only — not
                  used in the formula; the caller encodes it in addr bits 32+).
        src_node: linear index of the sending node.
        seq:      0-based transaction-pair index within this src_node's sequence.
        base_offset: base local address from the scenario (memory_base & 0xFFFFFFFF).
        n_nodes:  total node count in the topology (upper bound on src_node and seq).
        region_bytes: dst tile's memory window size (auto-derived in main() as
                  n_nodes * transactions_per_node * stride); the offset + reserved
                  must stay below base_offset + region_bytes.
        reserved: bytes the slot occupies (default one slot = stride); for a burst,
                  pass the burst's total byte length so the tail also fits.
        stride:   byte step between adjacent slots (default _SLOT_STRIDE = 0x40).

    Raises:
        ValueError: if offset + reserved would exceed base_offset + region_bytes.
    """
    _ = dst_node  # unused in formula; kept for caller clarity and T2/T3 reuse
    # A slot occupies `reserved` bytes; the spacing between slots must be at least
    # that, or a burst footprint larger than the default stride overlaps its
    # neighbour (root cause of BUR-002/003 hotspot off-by-0x40 under many-to-one).
    stride = max(stride, reserved)
    offset = base_offset + src_node * stride + seq * (n_nodes * stride)
    if (offset - base_offset) + reserved > region_bytes:
        raise ValueError(
            f"alloc_unique_offset: local offset {offset:#x} (+{reserved:#x} reserved) "
            f"exceeds memory window [{base_offset:#x}, {base_offset + region_bytes:#x}) "
            f"(region_bytes={region_bytes:#x}); reduce transactions-per-node"
        )
    return offset


# ---------------------------------------------------------------------------
# Topology loader
# ---------------------------------------------------------------------------

def _load_topology(name):
    """Return (nodes, x_dim, y_dim, tile_size) where nodes = [(idx, x, y, cid), ...].

    `name` is either a topology name resolved against sim/topologies/<name>.yaml, or
    a direct path to a topology yaml (ends in .yaml or names an existing file).  The
    path form lets callers point at a temp topology without writing into the live tree.

    tile_size is read from the topology's address_map.tile_size (default
    _DEFAULT_TILE_SIZE if the address_map block or the key is absent).
    """
    if name.endswith(".yaml") or os.path.isfile(name):
        topo_path = name
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        base = name[:-4] if name.endswith("_rob") else name
        topo_path = os.path.join(here, "..", "topologies", f"{base}.yaml")
    with open(topo_path) as f:
        topo = yaml.safe_load(f)
    x_dim = topo["topology"]["x_dim"]
    y_dim = topo["topology"]["y_dim"]
    tile_size = (topo.get("address_map") or {}).get("tile_size", _DEFAULT_TILE_SIZE)
    # The address formula lays every tile at base = coord_id * tile_size. The c_model
    # SAM honors an explicit per-tile `base`, but this generator does not read it, so a
    # custom base would silently misroute. Reject it loudly (honor it when a real
    # non-uniform map needs it).
    for t in (topo.get("address_map") or {}).get("tiles") or []:
        if "base" in t and int(t["base"]) != coord_id(t["x"], t["y"]) * tile_size:
            raise ValueError(
                f"topology tile (x={t['x']},y={t['y']}) base={int(t['base']):#x} != uniform "
                f"base {coord_id(t['x'], t['y']) * tile_size:#x} (coord_id*tile_size); the "
                f"stimulus generator supports only the uniform per-tile base"
            )
    nodes = []
    idx = 0
    for y in range(y_dim):
        for x in range(x_dim):
            nodes.append((idx, x, y, coord_id(x, y)))
            idx += 1
    return nodes, x_dim, y_dim, tile_size


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


def _check_transpose_guard(x_dim, y_dim):
    """Fail fast if the mesh is unsuitable for transpose.

    Booksim TransposeTrafficPattern::TransposeTrafficPattern (traffic.cpp:230-242)
    requires the total node count to be an even power of two (it exit(-1)s otherwise).
    For a square row-major mesh this additionally requires x_dim == y_dim and x_dim
    to be a power of two.  Non-square meshes cannot satisfy the equal bit-field split
    that makes the bit-half-swap equivalent to (x,y)→(y,x).
    """
    if x_dim != y_dim:
        sys.exit(
            f"ERROR: transpose pattern requires a square mesh (x_dim == y_dim); "
            f"got {x_dim}x{y_dim}.  Use a square topology (e.g. a 4x4 square mesh)."
        )
    if x_dim == 0 or (x_dim & (x_dim - 1)) != 0:
        sys.exit(
            f"ERROR: transpose pattern requires x_dim to be a power of two "
            f"(booksim requires even power-of-two node count); got x_dim={x_dim}."
        )


# ---------------------------------------------------------------------------
# Pattern dispatch
# ---------------------------------------------------------------------------

def _dst_for(pattern, x, y, x_dim, y_dim):
    """Return (dst_x, dst_y) for the given pattern and source coordinates (deterministic)."""
    if pattern == "neighbor":
        return neighbor_dst(x, y, x_dim, y_dim)
    if pattern == "transpose":
        return transpose_dst(x, y)
    raise ValueError(f"Unknown pattern: {pattern!r} (use per-packet sampler for random patterns)")


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Emit per-node file_master write.txt/read.txt for a traffic pattern."
    )
    ap.add_argument("--pattern", required=True,
                    choices=["neighbor", "transpose", "uniform_random", "hotspot"],
                    help="Traffic pattern")
    ap.add_argument("--topology", default="mesh_4x4_vc1",
                    help="Topology name (matches sim/topologies/<name>.yaml) or a "
                         "direct path to a topology yaml")
    ap.add_argument("--out", required=True,
                    help="Output directory; writes <out>/node<i>/{write,read}.txt")
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
    # Synthetic payload shape
    ap.add_argument("--size", type=int, default=2,
                    help="AxSIZE for synthetic transactions (0..7; default 2 = 4 bytes)")
    ap.add_argument("--len", type=int, default=0, dest="burst_len",
                    help="AxLEN for synthetic transactions (0..255; default 0 = single beat)")
    ap.add_argument("--ids-per-tile", type=int, default=1,
                    help="Distinct AXI ids per tile (default 1 = one independent id "
                         "per tile). >1 gives each tile a non-overlapping id block, "
                         "round-robin within the tile, so more transactions stay "
                         "outstanding (escape same-id ordering) to load the fabric. "
                         "Does not affect VC allocation (VC is id-agnostic).")
    a = ap.parse_args(argv)

    nodes, x_dim, y_dim, tile_size = _load_topology(a.topology)
    _check_mesh_capacity(x_dim, y_dim)
    n_nodes = len(nodes)

    widths = axi_widths()
    if n_nodes * a.ids_per_tile > (1 << widths["id"]):
        ap.error(f"--ids-per-tile {a.ids_per_tile} x {n_nodes} nodes exceeds the "
                 f"{1 << widths['id']} AXI id space; per-tile id blocks would overlap")
    base_local = 0x1000
    # Auto-derived dst-tile window: n_nodes * transactions_per_node slots, each
    # `stride` bytes apart. stride matches alloc_unique_offset's own
    # max(_SLOT_STRIDE, reserved) so this is a tight upper bound on its max
    # offset + reserved (see alloc_unique_offset docstring).
    burst_footprint = (a.burst_len + 1) * (1 << a.size)
    stride = max(_SLOT_STRIDE, burst_footprint)
    region_bytes = n_nodes * a.transactions_per_node * stride
    rng = _random_module.Random(a.seed)
    if a.pattern == "transpose":
        _check_transpose_guard(x_dim, y_dim)   # square-mesh precondition (legacy parity)
    for (idx, x, y, src_cid) in nodes:
        if a.pattern in ("neighbor", "transpose"):
            dst_x, dst_y = _dst_for(a.pattern, x, y, x_dim, y_dim)
            dst_cids = [coord_id(dst_x, dst_y)] * a.transactions_per_node
        elif a.pattern == "uniform_random":
            dst_lin = uniform_random_dsts(idx, n_nodes, a.transactions_per_node,
                                          rng, a.exclude_self)
            dst_cids = [coord_id(*_linear_to_coord(d, x_dim)) for d in dst_lin]
        else:  # hotspot
            if a.hotspot is None:
                ap.error("--hotspot is required for the hotspot pattern")
            dst_lin = hotspot_dsts(idx, n_nodes, a.transactions_per_node, rng,
                                   a.hotspot, a.hotspot_rates, a.exclude_self)
            dst_cids = [coord_id(*_linear_to_coord(d, x_dim)) for d in dst_lin]
        emit_file_master_node(os.path.join(a.out, f"node{idx}"), idx, dst_cids,
                              n_nodes, base_local, region_bytes,
                              a.size, a.burst_len, widths["data"],
                              ids_per_tile=a.ids_per_tile, num_axi_ids=(1 << widths["id"]),
                              tile_size=tile_size)


if __name__ == "__main__":
    main(sys.argv[1:])
