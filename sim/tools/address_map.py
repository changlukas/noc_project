"""Shared packed address-map helper for the sim-side generators.

Mirrors c_model SamTable::packed / SamTable::validate (nmu/addr_trans.hpp) so
gen_test_patterns.py and gen_tb_top.py compute the same base(dst_id) as the
C++ SAM from the same topology YAML.

address_map format (topology YAML):
    address_map:
      tiles:                                 # ordered list
        - { x: 0, y: 0, size: 0x100000000 }
        - { x: 1, y: 0, size: 0x100000000 }
        - { x: 0, y: 0, size: 0x1000, space: config }  # optional narrow aperture
        # ... one memory-space entry per node, in pack order; a node may
        # additionally carry one config-space entry (docs/noc-target-spec.md
        # §5 "SAM address spaces").
No tile_size, no base, no default. space defaults to "memory".

Packing rule: base(0) = 0x0; base(i) = base(i-1) + size(i-1), in list order
(regardless of space). dst_id = (y << X_WIDTH) | x.
"""

X_WIDTH = 4  # mirrors ni_flit_constants.h width::X_WIDTH / addr_trans.hpp


def dst_id(x, y):
    """Coordinate-encoded node id = (y << X_WIDTH) | x. Mirrors addr_trans.hpp."""
    return (y << X_WIDTH) | x


def pack(address_map, x_dim, y_dim):
    """Pack address_map["tiles"] into {dst_id: base} + ordered entries.

    Raises ValueError (fail-loud, mirrors SamTable::validate) on: missing/empty
    tiles list, non-positive or non-4KB-aligned size, a tile outside the mesh,
    an unrecognized space, or a missing/duplicate mesh node per space (memory
    space must cover every node exactly once; config space is sparse, at most
    one tile per node).

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
    if len(seen_memory) != x_dim * y_dim:
        raise ValueError(
            f"address_map.tiles memory space covers {len(seen_memory)} nodes, expected "
            f"{x_dim * y_dim} ({x_dim}x{y_dim} mesh, one memory tile per node)")
    # No overlap check needed: sizes are validated positive above, so packing
    # (base(i) = base(i-1) + size(i-1)) always yields disjoint, contiguous ranges.

    bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "memory"}
    return bases, entries
