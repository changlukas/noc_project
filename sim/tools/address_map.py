"""Shared packed address-map helper for the sim-side generators.

Mirrors c_model SamTable::packed / SamTable::validate (nmu/addr_trans.hpp) so
gen_test_patterns.py and gen_tb_top.py compute the same base(dst_id) as the
C++ SAM from the same topology YAML.

address_map format (topology YAML):
    address_map:
      tiles:                                 # ordered list
        - { x: 0, y: 0, size: 0x100000000 }
        - { x: 1, y: 0, size: 0x100000000 }
        - { x: 0, y: 0, size: 0x1000, space: config }  # narrow aperture
        - { x: 1, y: 0, size: 0x1000, space: config }
        # ... one memory-space entry per node, then one config-space entry per
        # node, both in raster order (docs/noc-target-spec.md §5 "SAM address
        # spaces").
No tile_size, no base, no default. space defaults to "memory".

Packing rule: base(0) = 0x0; base(i) = base(i-1) + size(i-1), in list order
(regardless of space). dst_id = (y << X_WIDTH) | x.
"""

X_WIDTH = 4  # mirrors ni_flit_constants.h width::X_WIDTH / addr_trans.hpp

# Tile-local space order. Fixed, not inferred: the tile crossbar's config
# target indexes off the raw tile-local address, so config must sit at 0x0.
SPACE_ORDER = ("config", "memory")


def dst_id(x, y):
    """Coordinate-encoded node id = (y << X_WIDTH) | x. Mirrors addr_trans.hpp."""
    return (y << X_WIDTH) | x


def pack(address_map, x_dim, y_dim):
    """Pack address_map["tiles"] into {dst_id: base} + ordered entries.

    Raises ValueError (fail-loud, mirrors SamTable::validate) on: missing/empty
    tiles list, non-positive or non-4KB-aligned size, a tile outside the mesh,
    an unrecognized space, or a missing/duplicate mesh node per space -- both
    spaces must cover every node exactly once.

    Config coverage is required unconditionally here and only when the space is
    present in SamTable::validate(): this function only ever sees a shipped
    topology YAML, where spec §5.1 "every node owns one region per address
    space" holds outright, while validate() also runs on hand-built in-memory
    tables (SamTable::uniform() is memory-only and is the fixture constructor
    for most c_model tests).

    Returns (bases, entries):
        bases:   {dst_id: base} for the memory-space tile only (existing
                 consumers -- gen_test_patterns.py / gen_tb_top.py -- want the
                 node's default/data-class base, not a config aperture).
        entries: ordered [{"x", "y", "size", "base", "dst_id", "space"}, ...]
    """
    tiles = (address_map or {}).get("tiles")
    if not tiles:
        raise ValueError("address_map.tiles missing or empty")

    entries = []
    base = 0
    for t in tiles:
        x, y, size = int(t["x"]), int(t["y"]), int(t["size"])
        space = t.get("space", "memory")
        if space not in ("config", "memory"):
            raise ValueError(
                f"address_map tile (x={x},y={y}) space {space!r} must be 'config' or 'memory'")
        if size <= 0 or size % 0x1000 != 0:
            raise ValueError(
                f"address_map tile (x={x},y={y}) size {size:#x} must be positive "
                f"and 4 KB aligned")
        if not (x < x_dim and y < y_dim):
            raise ValueError(
                f"address_map tile (x={x},y={y}) outside mesh {x_dim}x{y_dim}")
        entries.append({"x": x, "y": y, "size": size, "base": base, "dst_id": dst_id(x, y),
                        "space": space})
        base += size

    seen_memory = set()
    seen_config = set()
    for e in entries:
        node = (e["x"], e["y"])
        seen = seen_memory if e["space"] == "memory" else seen_config
        if node in seen:
            raise ValueError(
                f"address_map: duplicate mesh node (x={e['x']},y={e['y']}) in {e['space']} space")
        seen.add(node)
    for space, seen in (("memory", seen_memory), ("config", seen_config)):
        if len(seen) != x_dim * y_dim:
            raise ValueError(
                f"address_map.tiles {space} space covers {len(seen)} nodes, expected "
                f"{x_dim * y_dim} ({x_dim}x{y_dim} mesh, one {space} tile per node)")
    # No overlap check needed: sizes are validated positive above, so packing
    # (base(i) = base(i-1) + size(i-1)) always yields disjoint, contiguous ranges.

    bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "memory"}
    return bases, entries


def space_windows(entries):
    """One window per address space, spanning EVERY node's slot in that space.

    The tile decoder tells the two CLASSES apart; it does not tell nodes apart.
    That is deliberate, and it is what makes a collective work. A multicast AW
    reaches N nodes carrying ONE address -- the anchor's -- because nothing on
    the path rewrites it (upstream is the same: floo_axi_chimney.sv:744 hands
    the destination the flit payload verbatim). Every replica therefore has to
    accept an address naming a different node's slot and land at the same offset
    inside its own. A per-node window would DECERR every replica but the
    anchor's.

    So the window covers the whole space, and node_addr_w is what strips the
    node index back off before the memory sees the address. Upstream's own
    multicast testbench is built the same way: node regions differ only in high
    bits, and the destination memory sits at [0, 0x8000)
    (hw/tb/tb_floo_rob_multicast.sv).

    Returns [{"space", "base", "span", "node_addr_w"}, ...] in SPACE_ORDER,
    present spaces only:
        base / span   the whole space, for the crossbar's decode
        node_addr_w   log2 of one node's slot, for the offset mask
    """
    windows = []
    for space in SPACE_ORDER:
        members = [e for e in entries if e.get("space", "memory") == space]
        if not members:
            continue
        base = min(e["base"] for e in members)
        end = max(e["base"] + e["size"] for e in members)
        span = 0x1000
        while span < end - base:
            span <<= 1
        slot = 0x1000
        while slot < max(e["size"] for e in members):
            slot <<= 1
        # node_addr_w is applied as a power-of-two mask, which only yields the
        # offset within a node's slot if the space starts on a slot boundary. A
        # span-aligned base guarantees that (span >= slot). Fail loud rather than
        # emit a testbench whose replicas land at the wrong offset.
        if base & (span - 1):
            raise ValueError(
                f"address_map: {space} space base {base:#x} is not aligned to its {span:#x} "
                f"window; the endpoint's offset mask would strip the wrong bits")
        windows.append({"space": space, "base": base, "span": span,
                        "node_addr_w": slot.bit_length() - 1})
    return windows
