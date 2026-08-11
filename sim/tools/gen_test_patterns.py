#!/usr/bin/env python3
"""Emit per-node pulp axi_file_master stimulus (write.txt/read.txt) for a traffic pattern.

Usage:
    gen_test_patterns.py --pattern neighbor \\
        --topology <name> --out <dir>

    gen_test_patterns.py --pattern uniform_random \\
        --topology mesh_4x4_vc1 --out <dir> \\
        --transactions-per-node 4 --seed 42

Writes <out>/node<i>/{write,read}.txt for each node i. Each transaction:
  - addr = base(dst_coord) + local_offset  (base from the topology's packed
    address_map.tiles list; see address_map.py)
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

beat_exact  (S2 gate: DPI word-packing fault injection, not a spatial pattern)
    Every node writes one full-width beat (per-lane-distinct bytes, full
    strobe) plus an 8-position walking-strb sweep to its neighbor
    (neighbor_dst bijection) -- see emit_beat_exact_node / _BEAT_EXACT_STRB_OFFSETS.
    A node whose topology entry owns a config-space tile additionally routes
    one narrow-class 2-beat burst to it (narrow_beat_exact_lines), so a
    config-space topology exercises both classes in one run. Ignores
    --transactions-per-node/--size/--len/--seed (own fixed shape).

multicast  (S4 collectives; stimulus intent ported from FlooNoC
            tb_floo_rob_multicast.sv:30-49,189-195,379-395 masked-region writes)
    Source nodes issue multicast writes whose AWUSER carries the address mask
    (row / column / 2x2-submesh member sets, --mcast-shape, ONE shape per run),
    then read back every member replica; non-source nodes carry unicast filler
    plus (config topologies) cross-node narrow probes. The issue schedule obeys
    restriction R1: same-shape trees are pairwise disjoint across sources, and
    each source's own multicasts share one AXI id so the NMU's R2 gate
    serializes them on the merged B. See the "Multicast pattern" section below.
    Ignores --ids-per-tile (one id per node is the serialization mechanism).

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

Per-tile base address = base(dst_id), packed from the topology YAML's
address_map.tiles list (see address_map.py). Mirrors c_model SamTable::packed's
base formula (addr_trans.hpp).
"""

import argparse
import os
import random as _random_module
import sys
from pathlib import Path

import yaml

import address_map

# Must mirror c_model addr_trans.hpp SamTable::packed:
#   dst_id = (y << X_WIDTH) | x
#   base(i) = base(i-1) + size(i-1), in address_map.tiles list order
X_WIDTH = 4
Y_WIDTH = 4          # mirrors ni_flit_constants.h width::Y_WIDTH
DST_ID_WIDTH = 8     # header::DST_ID_WIDTH = X_WIDTH + Y_WIDTH; max nodes = 2**8 = 256
_FILE_KEYS = ("data_file", "dump_file", "strb_file")

# Per-transaction slot stride for the unique-offset allocator.  Must be at least
# as large as the max transaction data payload (one cache-line = 64 B = 0x40).
_SLOT_STRIDE = 0x40

# beat_exact pattern: 8 single-byte (walking-1 WSTRB) probe offsets within a
# 64 B beat, chosen to straddle every DPI word boundary that matters, not all
# 64 lanes. DATA (512 b) packs as 16 x 32-bit words (word i = bytes[4i:4i+3]);
# WSTRB (64 b) packs as 2 x 32-bit words (word0 = bytes[0:31], word1 =
# bytes[32:63]) per specgen/generated DPI marshalling (16 words / 2 words,
# S2 design doc sec 4). Offsets:
#   0, 63  -- vector edges (word0 LSB / word15 MSB); MSB (63) is also the
#             exact tail-mask bit T2c's DPI fix targeted.
#   3, 4   -- straddle the first DATA word boundary (word0/word1).
#   31, 32 -- straddle BOTH the WSTRB word0/word1 boundary (its only one,
#             lane 31|32 = strb bit 31|32) AND a DATA word boundary
#             (word7/word8); the single pair that most directly proves the
#             "lo | hi<<32" WSTRB packing together with mid-vector DATA
#             packing.
#   59, 60 -- straddle a DATA word boundary near the top (word14/word15).
_BEAT_EXACT_STRB_OFFSETS = (0, 3, 4, 31, 32, 59, 60, 63)

_CONSTANTS_YAML = Path(__file__).resolve().parents[2] / "specgen" / "source" / "constants.yaml"


def axi_widths():
    """AXI ID/ADDR/DATA widths from the DUT single source of truth
    (specgen/source/constants.yaml -- the same file ni_params_pkg is generated from).
    Read directly (values only); the specgen validator owns schema checking."""
    axi = yaml.safe_load(_CONSTANTS_YAML.read_text(encoding="utf-8"))["axi"]
    return {
        "id":   int(axi["INITIATOR_ID_WIDTH"]["default"]),
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


def _ax_fields(axid, addr, axi_len, axi_size, include_atop, user=0):
    """The AW/AR field lines in parse_write/parse_read order. Write includes atop
    (12 fields); read omits it (11 fields, matching axi_file_master.parse_read).

    user: AWUSER value (58 b, decimal in the file; axi_file_master parses %d).
    Nonzero only for collective writes: [9:8] collective_op, [57:10] the
    address mask (docs/noc-target-spec.md AWUSER layout). Reads keep 0."""
    lines = [str(axid), f"0x{addr:x}", str(axi_len), str(axi_size),
             "1", "0", "0", "0", "0", "0"]          # burst=INCR lock cache prot qos region
    if include_atop:
        lines.append("0")                            # atop (write only)
    lines.append(str(user))                          # user (AWUSER / ARUSER)
    return lines


def emit_file_master_node(out_dir, src_idx, dst_cids, n_nodes,
                          base_local, region_bytes, axi_size, axi_len, data_width,
                          ids_per_tile=1, num_axi_ids=256, bases=None, extra=None):
    """Write out_dir/{write,read}.txt for one node. One write+read pair per dst_cid,
    src-partitioned address, address-in-data payload. INCR, atop=0, full strobe.

    bases: {dst_id: base}, from address_map.pack() -- the packed tile base for
    each dst_cid in dst_cids.

    AXI ids: each tile owns an independent, non-overlapping block of `ids_per_tile`
    ids starting at src_idx*ids_per_tile (mod num_axi_ids), so no two tiles share
    an id. ids_per_tile=1 gives each tile one distinct id (= src_idx); >1 lets a
    tile keep several transactions outstanding (distinct ids escape same-id
    ordering), raising injected concurrency for saturation runs. VC allocation is
    id-agnostic (VC id only), so this changes concurrency, not VC spread.

    extra: optional (write_lines, read_lines) tuple appended after the regular
    dst_cids transactions -- e.g. one narrow-class (config-space) probe for a
    node that owns a config tile, so a single node routes both classes."""
    os.makedirs(out_dir, exist_ok=True)
    reserved = (axi_len + 1) * (1 << axi_size)
    id_base = (src_idx * ids_per_tile) % num_axi_ids
    write_lines, read_lines = [], []
    for seq, dst_cid in enumerate(dst_cids):
        local_off = alloc_unique_offset(dst_cid, src_idx, seq, base_local,
                                        n_nodes, region_bytes, reserved=reserved)
        addr = bases[dst_cid] + local_off
        axid = (id_base + (seq % ids_per_tile)) % num_axi_ids
        write_lines += _ax_fields(axid, addr, axi_len, axi_size, include_atop=True)
        write_lines += encode_write_beats(addr, axi_size, axi_len, data_width)
        read_lines += _ax_fields(axid, addr, axi_len, axi_size, include_atop=False)
    if extra is not None:
        extra_write, extra_read = extra
        write_lines += extra_write
        read_lines += extra_read
    with open(os.path.join(out_dir, "write.txt"), "w") as f:
        f.write("\n".join(write_lines) + "\n")
    with open(os.path.join(out_dir, "read.txt"), "w") as f:
        f.write("\n".join(read_lines) + "\n")


def emit_beat_exact_node(out_dir, src_idx, dst_base, data_width, extra=None, base_local=0x1000):
    """Write out_dir/{write,read}.txt for one node's beat-exact (data-class)
    probe: one full-width beat with per-lane-distinct bytes and full strobe,
    followed by the 8-position walking-strb sweep (_BEAT_EXACT_STRB_OFFSETS),
    all single-beat writes so address-in-data (encode_write_beats) already
    gives per-lane-distinct bytes and, at AxSIZE=0, an exact single-bit
    (walking) WSTRB -- no new data/strb encoding needed.

    Offset by base_local (same convention emit_file_master_node's slots use),
    not raw dst_base, so this probe's [+0x000, +0x080) window is disjoint by
    construction from anything else that may occupy the tile's low offset 0
    (e.g. a config-space probe sharing the same address-in-data low byte
    pattern at local offset 0 -- narrow readback couldn't otherwise tell the
    two probes' bytes apart).

    Two disjoint 64 B-aligned windows at dst_base+base_local: [+0x000, +0x040)
    for the full beat, [+0x040, +0x080) for the walking-strb sweep, so the
    sweep's partial-strobe writes can never be masked by the full beat's bytes.

    extra: optional (write_lines, read_lines) tuple appended after the
    beat-exact transactions -- see emit_file_master_node."""
    bus_bytes = data_width // 8
    if bus_bytes != 64:
        raise ValueError(
            f"emit_beat_exact_node: _BEAT_EXACT_STRB_OFFSETS are derived for a "
            f"64 B bus (512 b DATA_WIDTH); got {bus_bytes} B (data_width={data_width})")
    full_size = bus_bytes.bit_length() - 1  # log2(64) = 6
    os.makedirs(out_dir, exist_ok=True)
    axid = src_idx
    probe_base = dst_base + base_local
    write_lines, read_lines = [], []
    write_lines += _ax_fields(axid, probe_base, 0, full_size, include_atop=True)
    write_lines += encode_write_beats(probe_base, full_size, 0, data_width)
    read_lines += _ax_fields(axid, probe_base, 0, full_size, include_atop=False)
    strb_base = probe_base + bus_bytes
    for off in _BEAT_EXACT_STRB_OFFSETS:
        addr = strb_base + off
        write_lines += _ax_fields(axid, addr, 0, 0, include_atop=True)
        write_lines += encode_write_beats(addr, 0, 0, data_width)
        read_lines += _ax_fields(axid, addr, 0, 0, include_atop=False)
    if extra is not None:
        extra_write, extra_read = extra
        write_lines += extra_write
        read_lines += extra_read
    with open(os.path.join(out_dir, "write.txt"), "w") as f:
        f.write("\n".join(write_lines) + "\n")
    with open(os.path.join(out_dir, "read.txt"), "w") as f:
        f.write("\n".join(read_lines) + "\n")


def narrow_beat_exact_lines(axid, config_base, data_width):
    """(write_lines, read_lines) for one narrow-class beat-exact probe: a
    2-beat INCR burst at AxSIZE=3 (8 B, the narrow lane width) targeting a
    config-space aperture. encode_write_beats already gives per-lane-distinct
    bytes and full per-beat strobe -- same formula as the data-class full
    beat, just narrower. Two beats ('a couple') proves lane re-anchor holds
    across an address increment; no walking-strb needed here, the narrow
    class's 81 b NarrowW payload carries the whole 8 B lane in one flit, no
    multi-word DPI packing to fault-inject. AxSIZE<=3 is required -- narrow
    class rejects larger (S2 design doc sec 1)."""
    axi_len, axi_size = 1, 3
    write = _ax_fields(axid, config_base, axi_len, axi_size, include_atop=True)
    write += encode_write_beats(config_base, axi_size, axi_len, data_width)
    read = _ax_fields(axid, config_base, axi_len, axi_size, include_atop=False)
    return write, read


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
# Multicast pattern (S4 collectives)
# ---------------------------------------------------------------------------
#
# One mask SHAPE per run (--mcast-shape row|col|submesh): concurrent multicast
# spanning trees must be pairwise disjoint (restriction R1, s4-phase0-design
# §1.3). Row trees live in their own row's links, column trees in their own
# column's, 2x2-block trees in their own block's -- disjoint across sources by
# construction. Mixing shapes in one run would overlap trees at shared eject
# outputs, which R1 forbids for concurrently in-flight multicasts.
#
# Within one source every transaction shares ONE AXI id, so the NMU's R2 gate
# (one outstanding collective per (NMU, id); nmu/rob.hpp) serializes that
# source's own multicasts on the merged B -- the "issuer waits for the merged
# B" arm of R1, enforced by the DUT, not by file pacing.
#
# Per-node roles:
#   source nodes  : T data-class multicast writes over the member set (anchor =
#                   the source's own tile; AWUSER carries the address mask) +
#                   on a config topology one narrow-class multicast into the
#                   members' config tiles (config-space message replication).
#                   Readback phase reads EVERY member replica (scoreboard keys
#                   by (dst_id, local_addr) -- full address = base + offset).
#   other nodes   : T unicast neighbor write+read pairs (filler traffic) + on
#                   a config topology one narrow 2-beat write+read probe into
#                   the NEXT node's config tile -- cross-node narrow transit
#                   traffic, so the CollectB join contends on RSP with responses
#                   it does not own. (Every B and R beat is stamped flit_tail=1
#                   at nsu/packetize.hpp:99,125, so RSP carries no multi-flit
#                   worm and this probe cannot exercise a mid-worm hold.)
#
# Local-offset partitions inside a tile. The two spaces sit at different bases
# in the map itself, so a config offset can never collide with a memory one;
# these windows stay disjoint within their own space:
#   [0x0,    0x10)                 config multicast slot (16 B narrow burst)
#   [0x800,  0x800 + n*0x40)       cross-node config probes (one per node)
#   [0x1000, 0x1000 + region)      unicast filler slots (alloc_unique_offset)
#   [0x1000 + region, ... )        data multicast slots (seq * stride)

_MCAST_SHAPES = ("row", "col", "submesh")
_CONFIG_PROBE_BASE = 0x800  # cross-node config probe window, below base_local


def collective_addr_mask(bases, member_cids, anchor_cid):
    """OR of (base[m] XOR base[anchor]) with wildcard-closure validation.

    The AWUSER mask semantics (spec §6) require the member set to be exactly a
    wildcard over the mask bits: 2^popcount(mask) members whose bases cover
    every masked combination.  The NMU's collective_translate re-checks this
    and aborts the run; validating here turns a mask-unfriendly address map
    into a stimulus-generation error instead of a co-sim abort.

    `bases` must be the bases of ONE address space, and the caller is expected
    to have picked the space its anchor lands in.  The node-index field sits at
    log2 of that space's region size (spec §5.1), so the mask bits land at
    different address bits per space -- on the shipped 4x4, bits [21:20] for a
    1 MB memory region and [13:12] for a 4 KB config region.  Deriving the mask
    from the bases rather than from a constant is what keeps this correct
    without the caller naming a bit position: mixing two spaces' bases in one
    call produces a mask spanning both fields, which the wildcard check below
    rejects.
    """
    anchor = bases[anchor_cid]
    mask = 0
    for m in member_cids:
        mask |= bases[m] ^ anchor
    n_bits = bin(mask).count("1")
    if (1 << n_bits) != len(member_cids):
        raise ValueError(
            f"multicast member set of {len(member_cids)} needs a power-of-two wildcard; "
            f"mask {mask:#x} has {n_bits} bits (address map not mask friendly)")
    lo = anchor & ~mask
    combos = set()
    sub = mask
    while True:
        combos.add(lo | sub)
        if sub == 0:
            break
        sub = (sub - 1) & mask
    if combos != {bases[m] for m in member_cids}:
        raise ValueError(
            f"multicast member bases are not a wildcard over mask {mask:#x}: "
            f"{sorted(hex(bases[m]) for m in member_cids)}")
    if mask >> 48:
        raise ValueError(f"collective address mask {mask:#x} exceeds AWUSER[57:10] (48 b)")
    return mask


def _awuser_multicast(addr_mask):
    """AWUSER encode: [9:8] = MULTICAST (1), [57:10] = address mask."""
    return (addr_mask << 10) | (1 << 8)


def mcast_groups(shape, x_dim, y_dim):
    """[(src_xy, [member_xy...]), ...] with pairwise-disjoint spanning trees."""
    if shape == "row":
        return [((0, y), [(x, y) for x in range(x_dim)]) for y in range(y_dim)]
    if shape == "col":
        return [((x, 0), [(x, y) for y in range(y_dim)]) for x in range(x_dim)]
    if shape == "submesh":
        if x_dim % 2 or y_dim % 2:
            sys.exit(f"ERROR: --mcast-shape submesh requires even mesh dims (got {x_dim}x{y_dim})")
        return [((bx, by), [(bx + dx, by + dy) for dy in (0, 1) for dx in (0, 1)])
                for by in range(0, y_dim, 2) for bx in range(0, x_dim, 2)]
    raise ValueError(f"unknown mcast shape {shape!r}")


def multicast_lines(axid, anchor_addr, addr_mask, member_addrs, axi_size, axi_len, data_width):
    """(write_lines, read_lines) for one multicast write + per-member readback.

    The write is ONE AW (anchor address, AWUSER mask) + its beats; the fabric
    replicates it.  Reads are plain unicasts, one per member replica address.
    Address-in-data payload only uses addr[7:0], and replicas differ from the
    anchor only in node-index bits (>= bit 12), so the anchor-encoded beats
    compare equal at every replica.
    """
    write = _ax_fields(axid, anchor_addr, axi_len, axi_size, include_atop=True,
                       user=_awuser_multicast(addr_mask))
    write += encode_write_beats(anchor_addr, axi_size, axi_len, data_width)
    read = []
    for m_addr in member_addrs:
        read += _ax_fields(axid, m_addr, axi_len, axi_size, include_atop=False)
    return write, read


def emit_multicast_pattern(out_root, nodes, x_dim, y_dim, bases, config_bases,
                           sizes, shape, n_txn, axi_size, axi_len, data_width,
                           base_local, region_bytes):
    """Write node<i>/{write,read}.txt for every node of the multicast pattern."""
    n_nodes = len(nodes)
    groups = {coord_id(*src): members for src, members in mcast_groups(shape, x_dim, y_dim)}
    burst_footprint = (axi_len + 1) * (1 << axi_size)
    stride = max(_SLOT_STRIDE, burst_footprint)
    mcast_base = base_local + region_bytes  # after the unicast filler window
    # Config space is all-or-nothing for the multicast pattern: narrow
    # multicast needs a config tile per member.
    config_all = len(config_bases) == n_nodes
    if config_bases and not config_all:
        sys.exit("ERROR: multicast pattern needs a config tile on EVERY node "
                 f"(got {len(config_bases)}/{n_nodes}); extend the topology's config tiles")
    # Bound the probe window by the CONFIG ENTRY, not by base_local: the two are
    # both 0x1000 on today's maps, but base_local is a memory-space slot
    # convention and has no say in how large a config aperture is. An overrun
    # would not fault -- it falls into the next SAM entry, routes to a different
    # node's config RAM, rebases to a legal tile-local offset there, and its
    # readback agrees, so nothing downstream would notice.
    if config_all:
        config_bytes = min(sizes["config"].values())
        if _CONFIG_PROBE_BASE + n_nodes * _SLOT_STRIDE > config_bytes:
            sys.exit(f"ERROR: cross-node config probe window "
                     f"{_CONFIG_PROBE_BASE:#x}+{n_nodes * _SLOT_STRIDE:#x} overruns the "
                     f"{config_bytes:#x} B config entry; reduce node count, move "
                     f"_CONFIG_PROBE_BASE, or enlarge the topology's config tiles")

    for (idx, x, y, src_cid) in nodes:
        write_lines, read_lines = [], []
        axid = idx % 256
        if src_cid in groups:
            members = [coord_id(mx, my) for (mx, my) in groups[src_cid]]
            addr_mask = collective_addr_mask(bases, members, src_cid)
            for seq in range(n_txn):
                off = mcast_base + seq * stride
                tile_size = sizes["memory"][src_cid]
                if off + burst_footprint > tile_size:
                    raise ValueError(
                        f"multicast slot {off:#x}+{burst_footprint:#x} exceeds tile "
                        f"size {tile_size:#x}; reduce transactions-per-node or burst")
                w, r = multicast_lines(axid, bases[src_cid] + off, addr_mask,
                                       [bases[m] + off for m in members],
                                       axi_size, axi_len, data_width)
                write_lines += w
                read_lines += r
            if config_all:
                # Narrow config-space multicast: 2-beat 8 B burst at config
                # offset 0 (config-space message replication use case).
                cfg_mask = collective_addr_mask(config_bases, members, src_cid)
                w, r = multicast_lines(axid, config_bases[src_cid], cfg_mask,
                                       [config_bases[m] for m in members],
                                       axi_size=3, axi_len=1, data_width=data_width)
                write_lines += w
                read_lines += r
        else:
            # Filler: unicast neighbor write+read pairs (same shape as the
            # neighbor pattern), src-partitioned offsets in the base_local
            # window -- disjoint from every multicast slot by construction.
            dst_cid = coord_id(*neighbor_dst(x, y, x_dim, y_dim))
            reserved = burst_footprint
            for seq in range(n_txn):
                off = alloc_unique_offset(dst_cid, idx, seq, base_local,
                                          n_nodes, region_bytes, reserved=reserved)
                addr = bases[dst_cid] + off
                write_lines += _ax_fields(axid, addr, axi_len, axi_size, include_atop=True)
                write_lines += encode_write_beats(addr, axi_size, axi_len, data_width)
                read_lines += _ax_fields(axid, addr, axi_len, axi_size, include_atop=False)
            if config_all:
                # Cross-node narrow probe (write then read back): transit
                # NarrowB/NarrowR traffic on RSP contending with the CollectB join.
                probe_cid = nodes[(idx + 1) % n_nodes][3]
                probe_addr = config_bases[probe_cid] + _CONFIG_PROBE_BASE + idx * _SLOT_STRIDE
                w, r = narrow_beat_exact_lines(axid, probe_addr, data_width)
                write_lines += w
                read_lines += r
        out_dir = os.path.join(out_root, f"node{idx}")
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "write.txt"), "w") as f:
            f.write("\n".join(write_lines) + "\n")
        with open(os.path.join(out_dir, "read.txt"), "w") as f:
            f.write("\n".join(read_lines) + "\n")


# ---------------------------------------------------------------------------
# Topology loader
# ---------------------------------------------------------------------------

def _load_topology(name):
    """Return (nodes, x_dim, y_dim, bases, config_bases, sizes) where nodes =
    [(idx, x, y, cid), ...], bases = {dst_id: base} (memory space) and
    config_bases = {dst_id: base} (config space, sparse -- most topologies
    have none), both from address_map.pack(); sizes = {"memory": {dst_id:
    size}, "config": {dst_id: size}} for capacity checks.

    `name` is either a topology name resolved against sim/topologies/<name>.yaml, or
    a direct path to a topology yaml (ends in .yaml or names an existing file).  The
    path form lets callers point at a temp topology without writing into the live tree.
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
    bases, entries = address_map.pack(topo.get("address_map"), x_dim, y_dim)
    config_bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "config"}
    sizes = {
        "memory": {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"},
        "config": {e["dst_id"]: e["size"] for e in entries if e["space"] == "config"},
    }
    nodes = []
    idx = 0
    for y in range(y_dim):
        for x in range(x_dim):
            nodes.append((idx, x, y, coord_id(x, y)))
            idx += 1
    return nodes, x_dim, y_dim, bases, config_bases, sizes


# ---------------------------------------------------------------------------
# Mesh capacity guard
# ---------------------------------------------------------------------------

def _check_mesh_capacity(x_dim, y_dim):
    """Fail fast if the mesh exceeds the dst_id address space, or falls below the
    per-dimension minimum.

    coord_id = (y << X_WIDTH) | x, so the encoding requires PER-DIMENSION fit:
      - x_dim <= 2**X_WIDTH  (else x aliases into the y field)
      - y_dim <= 2**Y_WIDTH  (else y overflows DST_ID_WIDTH)
      - x_dim * y_dim <= 2**DST_ID_WIDTH  (total node count fits dst_id)
    The product check alone misses e.g. 17x15 (=255 <= 256 but x_dim 17 > 16 aliases)
    or 32x8 (=256 but x_dim 32 > 16).  All three must hold.

    Mesh dim minimum is 2 per dimension: a mesh communicating through NI +
    router needs at least 2x2. 1x1 and 1xN meshes are illegal.
    """
    if x_dim < 2:
        sys.exit(
            f"ERROR: x_dim={x_dim} < 2; mesh dimension minimum is 2 "
            f"(1x1/1xN meshes are illegal)."
        )
    if y_dim < 2:
        sys.exit(
            f"ERROR: y_dim={y_dim} < 2; mesh dimension minimum is 2 "
            f"(1x1/1xN meshes are illegal)."
        )
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
                    choices=["neighbor", "transpose", "uniform_random", "hotspot", "beat_exact",
                             "multicast"],
                    help="Traffic pattern")
    ap.add_argument("--mcast-shape", choices=list(_MCAST_SHAPES), default="row",
                    help="Multicast mask shape (multicast pattern only). One shape "
                         "per run: concurrent multicast trees must be pairwise "
                         "disjoint (restriction R1)")
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

    nodes, x_dim, y_dim, bases, config_bases, sizes = _load_topology(a.topology)
    _check_mesh_capacity(x_dim, y_dim)
    n_nodes = len(nodes)

    widths = axi_widths()
    if n_nodes * a.ids_per_tile > (1 << widths["id"]):
        ap.error(f"--ids-per-tile {a.ids_per_tile} x {n_nodes} nodes exceeds the "
                 f"{1 << widths['id']} AXI id space one initiator may drive "
                 f"(constants.yaml INITIATOR_ID_WIDTH); per-tile id blocks would overlap")
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
    if a.pattern == "multicast":
        # One AXI id per node (R2 serializes a source's own multicasts on the
        # merged B); --ids-per-tile does not apply here.
        emit_multicast_pattern(a.out, nodes, x_dim, y_dim, bases, config_bases,
                               sizes, a.mcast_shape, a.transactions_per_node,
                               a.size, a.burst_len, widths["data"],
                               base_local, region_bytes)
        return
    for (idx, x, y, src_cid) in nodes:
        # A node that owns a config-space tile also routes one narrow-class
        # probe to it (self-targeted; config space is per-node, not spatial),
        # so a config-space topology exercises both classes in one run
        # regardless of which pattern drives the data-class traffic below.
        narrow_extra = (narrow_beat_exact_lines(idx, config_bases[src_cid], widths["data"])
                        if src_cid in config_bases else None)
        if a.pattern == "beat_exact":
            dst_x, dst_y = neighbor_dst(x, y, x_dim, y_dim)
            dst_base = bases[coord_id(dst_x, dst_y)]
            emit_beat_exact_node(os.path.join(a.out, f"node{idx}"), idx, dst_base,
                                 widths["data"], extra=narrow_extra, base_local=base_local)
            continue
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
                              bases=bases, extra=narrow_extra)


if __name__ == "__main__":
    main(sys.argv[1:])
