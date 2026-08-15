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
      peripherals:                           # optional, ordered list
        - { x: 0, y: 0, face: x, size: 0x1000 }
        # A peripheral hangs off a boundary port of the router at (x, y) --
        # face "x" is port 1, face "y" is port 2 -- and its region is placed
        # above the tile array in declaration order, not coordinate-derived.
No tile_size, no base, no default. space defaults to "memory".

Packing rule: base = ((y << x_bits) | x) * block_size + offset[space], where
block_size is address_map.block_size (a power of two, the node stride) and
offset[space] lays the spaces out inside a node's block, memory first at 0.
slot[space] is the largest declared size in that space and now bounds the
aperture, not the stride. dst_id = (y << X_WIDTH) | x.
"""

X_WIDTH = 4  # mirrors ni_flit_constants.h width::X_WIDTH / addr_trans.hpp

# Tile crossbar target order. Fixed, not inferred: it is what pins m0 to the
# config memory and the last target to the data memory in user_node_endpoint.
SPACE_ORDER = ("config", "memory", "peripheral")


def dst_id(x, y):
    """Coordinate-encoded node id = (y << X_WIDTH) | x. Mirrors addr_trans.hpp."""
    return (y << X_WIDTH) | x


def _clog2(n):
    b = 0
    while (1 << b) < n:
        b += 1
    return b


def _align_up(value, alignment):
    if alignment == 0:
        return value
    return (value + alignment - 1) // alignment * alignment


def _next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def _slot_size(tiles, space):
    sizes = {int(t["size"]) for t in tiles if t.get("space", "memory") == space}
    if not sizes:
        return 0
    return max(sizes)


def pack(address_map, x_span, y_span):
    """Pack address_map["tiles"] into {dst_id: base} + ordered entries.

    x_span/y_span is the route span (docs/noc-target-spec.md), not necessarily
    the router array: a peripheral coordinate outside the tile region still
    counts. Every topology with no peripheral has the two equal, so this is a
    no-op rename for every caller today.

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
        entries: ordered [{"x", "y", "size", "base", "dst_id", "space"}, ...],
                 tiles first, then one entry per address_map.peripherals member
                 (space "peripheral", carrying the "port" it hangs off).
    """
    tiles = (address_map or {}).get("tiles")
    if not tiles:
        raise ValueError("address_map.tiles missing or empty")

    x_bits = _clog2(x_span)
    slot = {sp: _slot_size(tiles, sp) for sp in ("memory", "config")}
    # Spaces sit inside a node's block in a fixed order, memory first at 0,
    # each aligned to its own slot. block_size is the one declared number.
    offset = {"memory": 0}
    offset["config"] = _align_up(slot["memory"], slot["config"]) if slot["config"] else 0
    # No config tile: the block only has to hold memory. With one, config
    # sits after it and dominates. Mirrors sam_yaml.hpp's default_block_size.
    extent = slot["memory"] if slot["config"] == 0 else offset["config"] + slot["config"]
    declared = (address_map or {}).get("block_size")
    block_size = int(declared) if declared is not None else _next_pow2(extent)
    if block_size & (block_size - 1):
        raise ValueError(f"address_map.block_size {block_size:#x} must be a power of two")
    if block_size < extent:
        raise ValueError(
            f"address_map.block_size {block_size:#x} is smaller than the spaces it must hold "
            f"({extent:#x})")

    entries = []
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
        if not (x < x_span and y < y_span):
            raise ValueError(
                f"address_map tile (x={x},y={y}) outside mesh {x_span}x{y_span}")
        sp = space
        base = (((y << x_bits) | x) * block_size) + offset[sp]
        entries.append({"x": x, "y": y, "size": size, "base": base, "dst_id": dst_id(x, y),
                        "space": space})

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
        if len(seen) != x_span * y_span:
            raise ValueError(
                f"address_map.tiles {space} space covers {len(seen)} nodes, expected "
                f"{x_span * y_span} ({x_span}x{y_span} mesh, one {space} tile per node)")
    # address_map.peripherals, placed after the coverage checks above so a
    # peripheral is never counted as a tile of either space. Declaration order,
    # above the tile array, each region aligned to its own size -- the same
    # placement SamTable::packed() does, which is what keeps noc_egress_base()
    # above every real region rather than inside a peripheral's window.
    next_base = x_span * y_span * block_size
    for p in (address_map or {}).get("peripherals") or []:
        x, y, face, size = int(p["x"]), int(p["y"]), p["face"], int(p["size"])
        if face not in ("x", "y"):
            raise ValueError(
                f"address_map peripheral (x={x},y={y}) face {face!r} must be 'x' or 'y'")
        # Zero passes a power-of-two test on its own and would align to base 0,
        # overlapping the whole tile array.
        if size <= 0 or size & (size - 1):
            raise ValueError(
                f"address_map peripheral (x={x},y={y}) size {size:#x} must be a non-zero "
                f"power of two")
        next_base = _align_up(next_base, size)
        entries.append({"x": x, "y": y, "size": size, "base": next_base, "dst_id": dst_id(x, y),
                        "space": "peripheral", "port": 1 if face == "x" else 2})
        next_base += size

    # No overlap check needed: (x, y) maps to a unique block below block_size,
    # every tile's size is at most slot[space] because that slot IS the
    # largest size declared in the space, offset[config] >= slot[memory] keeps
    # the two spaces apart inside a block, and block_size >= extent keeps a
    # block's own entries inside it -- so blocks, and the spaces inside them,
    # are always disjoint.

    bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "memory"}
    return bases, entries


def node_windows(entries, node_id):
    """One node's own region per space, in SPACE_ORDER, present spaces only.

    This is what the tile crossbar decodes on: a request that reached this node
    -- either from its own initiator or from the fabric -- names an address in
    one of these two ranges, or it is not this node's. The NSU rewrites an
    arriving address's node-coordinate field to this node before the crossbar
    sees it (nsu::Depacketize::rebase_), so a collective replica carrying the
    request's own address lands here like any unicast.

    Sizes are exact, not rounded: pulp axi_xbar states a rule as start/end, so
    a window need not be a power of two.

    Returns [{"space", "base", "size"}, ...].
    """
    out = []
    for space in SPACE_ORDER:
        for e in entries:
            if e["dst_id"] == node_id and e.get("space", "memory") == space:
                out.append({"space": space, "base": e["base"], "size": e["size"]})
                break
    return out


def noc_egress_base(entries):
    """Base of the tile crossbar's NoC egress aperture.

    A collective write names a SET of nodes, so "is this address mine" has no
    answer -- but a tile crossbar decodes addresses and nothing else. A
    collective whose address names the issuing node's own region would be
    answered locally and never reach the NI, and the fabric would never
    replicate it.
    The endpoint offsets such a write into this aperture, which the crossbar
    has a rule for pointing at the NI, and takes the offset back off on the way
    out (user_node_endpoint.sv).

    Derived from the map, never declared: the first power of two at or above
    the top of the map. Every node window therefore sits below it by
    construction, so the aperture cannot collide with a real region however the
    map grows -- which a hand-picked spare address bit could not promise.
    """
    top = max(e["base"] + e["size"] for e in entries)
    base = 0x1000
    while base < top:
        base <<= 1
    return base
