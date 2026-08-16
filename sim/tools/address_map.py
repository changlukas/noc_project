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

FlooNoC-shaped config (sim/configs/*.yml), read by pack_config():
    endpoints:
      - name: "tile"
        array: [4, 4]
        sbr_port_protocol: ["axi"]           # is_sbr() gates SAM participation
        addr_range:
          - { base: 0x0, size: 0x2000000, stride: 0x100000000, space: memory }
    routers:
      - { name: "router", array: [4, 4] }
    connections:
      - { src: "tile", dst: "router", src_range: ..., dst_range: ..., dst_dir: 4 }
Member k of a range lands at base + stride * k, stride defaulting to size
(FlooNoC routing.py:440-449). The member's coordinate is its host router's,
taken from the connection: array index k is X-fast, k = (y << x_bits) | x, which
is this repo's node numbering and not FlooNoC's Y-fast one -- on a square mesh
the two produce the same SET of bases and transpose every coordinate.

pack_document() takes either shape and dispatches on it, mirroring
nmu/sam_yaml.hpp load_sam_table(). Both readers must expand a config to the
same ordered rules; sim/tools/test_sam_config_parity.py and c_model's
tests/nmu/test_sam_config.cpp hold each side to the same file.
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


def pack(address_map, x_dim, y_dim):
    """Pack address_map["tiles"] into {dst_id: base} + ordered entries.

    x_dim/y_dim is the router array. A peripheral shares its host router's
    coordinate and takes no coordinate of its own, so the array is also the
    tile count per axis.

    Raises ValueError (fail-loud, mirrors SamTable::validate) on: missing/empty
    tiles list, non-positive or non-4KB-aligned size, a tile outside the mesh, a
    non-power-of-two x_dim, an unrecognized space, or a missing/duplicate mesh
    node per space -- both spaces must cover every node exactly once.

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

    x_bits = _clog2(x_dim)
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
        if not (x < x_dim and y < y_dim):
            raise ValueError(
                f"address_map tile (x={x},y={y}) outside mesh {x_dim}x{y_dim}")
        # A tile base is ((y << x_bits) | x) * block_size, so the top tile index
        # is x_dim * y_dim - 1 -- and next_base below is above the array -- only
        # when 1 << x_bits == x_dim. On a 3x2 array the top index is
        # (1 << 2) | 2 = 6 while next_base is 3 * 2 = 6 blocks, which IS tile
        # (2,1)'s own base: the first peripheral region would land on a tile
        # silently. Rejected here rather than in each caller, because this is
        # the arithmetic that depends on it -- sam_yaml.hpp and
        # gen_tb_top._check_flit_capacity reject the same shape independently.
        if (1 << x_bits) != x_dim:
            raise ValueError(
                f"address_map: x_dim {x_dim} must be a power of two -- the tile base packs x "
                f"into {x_bits} bits, so a non-power-of-two x_dim leaves gaps in the block "
                f"index and puts the first peripheral region on top of a tile")
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
        if len(seen) != x_dim * y_dim:
            raise ValueError(
                f"address_map.tiles {space} space covers {len(seen)} nodes, expected "
                f"{x_dim * y_dim} ({x_dim}x{y_dim} mesh, one {space} tile per node)")
    # address_map.peripherals, placed after the coverage checks above so a
    # peripheral is never counted as a tile of either space. Declaration order,
    # above the tile array, each region aligned to its own size -- the same
    # placement SamTable::packed() does, which is what keeps noc_egress_base()
    # above every real region rather than inside a peripheral's window.
    # This is above the whole tile array because x_dim is a power of two
    # (checked above), which makes x_dim * y_dim the block index just past the
    # top tile.
    next_base = x_dim * y_dim * block_size
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

    # No overlap check needed. Tiles: (x, y) maps to a unique block below
    # block_size, every tile's size is at most slot[space] because that slot IS
    # the largest size declared in the space, offset[config] >= slot[memory]
    # keeps the two spaces apart inside a block, and block_size >= extent keeps
    # a block's own entries inside it -- so blocks, and the spaces inside them,
    # are always disjoint. Peripherals: next_base starts above the whole tile
    # array -- which the power-of-two x_dim check makes true -- and only ever
    # moves up, so each region sits above every entry placed before it.

    bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "memory"}
    return bases, entries


# XYDirections (FlooNoC routing.py:245-252) -> the router port an endpoint hangs
# off. EJECT is the LOCAL port the tile owns; EAST/WEST are the x face and
# NORTH/SOUTH the y face, which is what address_map.peripherals spells "face".
# dst_dir is read as a port and nothing else: a peripheral shares its host
# router's coordinate, so FlooNoC's coordinate derivation for a non-router NI
# (network.py:354-363) must NOT be applied -- it would place the peripheral one
# step outside the mesh.
_PORT_OF_DST_DIR = {0: 2, 1: 1, 2: 2, 3: 1, 4: 0}


def router_array(cfg):
    """The single router array's (x_dim, y_dim).

    The router array IS the route coordinate space: a peripheral shares its host
    router's coordinate and takes none of its own, so this is also the tile count
    per axis. x must be a power of two -- the array index packs x into x_bits, so
    a non-power-of-two x leaves gaps in it.
    """
    routers = cfg["routers"]
    if len(routers) != 1:
        raise ValueError("config.routers: expected exactly one router array")
    x_dim, y_dim = int(routers[0]["array"][0]), int(routers[0]["array"][1])
    x_bits = _clog2(x_dim)
    if (1 << x_bits) != x_dim:
        raise ValueError(
            f"config: router array x {x_dim} must be a power of two -- the array index packs x "
            f"into {x_bits} bits, so a non-power-of-two x leaves gaps in it")
    return x_dim, y_dim


def members(endpoint):
    """Endpoint member count. Authored num wins; otherwise the array product."""
    if endpoint.get("num") is not None:
        return int(endpoint["num"])
    array = endpoint.get("array")
    if not array:
        return 1
    n = 1
    for d in array:
        n *= int(d)
    return n


def attachments(cfg, name, num, x_dim):
    """Member index -> {"x", "y", "port", "dir"}, off the connections naming this endpoint.

    Two forms. src_idx/dst_idx list the pairing explicitly. src_range/dst_range
    pair the two arrays element for element, which this reader accepts only when
    the two ranges are identical -- every shipped config connects a tile array to
    the router array it sits on, and a general range remap has no caller.
    """
    x_bits = _clog2(x_dim)
    out = [None] * num
    for c in cfg.get("connections") or []:
        if c.get("src") != name:
            continue
        direction = int(c["dst_dir"])
        port = _PORT_OF_DST_DIR.get(direction)
        if port is None:
            raise ValueError(
                f"connection {name}: dst_dir {c['dst_dir']} is not an XYDirections value")
        if c.get("src_idx") is not None:
            pairs = list(zip(c["src_idx"], c["dst_idx"]))
        else:
            if c.get("src_range") != c.get("dst_range"):
                raise ValueError(
                    f"connection {name}: src_range and dst_range differ -- this reader pairs the "
                    f"two arrays element for element and has no range remap")
            pairs = [(k, k) for k in range(num)]
        for src, dst in pairs:
            dst = int(dst)
            out[int(src)] = {"x": dst & (x_dim - 1), "y": dst >> x_bits, "port": port,
                             "dir": direction}
    for k, a in enumerate(out):
        if a is None:
            raise ValueError(f"endpoint {name}: member {k} has no connection")
    return out


def pack_config(cfg):
    """Expand a FlooNoC-shaped config into the same (bases, entries) pack() returns.

    An endpoint contributes to the SAM only if it declares sbr_port_protocol
    (FlooNoC endpoint.py:87-89). mgr_port_protocol / sbr_port_protocol are
    presence markers and nothing more: the protocols: block they would resolve
    against is omitted (design 3.6), the four AXI widths living in
    specgen/source/constants.yaml instead, so the labels themselves are ignored.

    Rules come out range-major -- every member of a range, then the next range --
    which is the order the topology YAML lists its tiles in, so the two shapes of
    the same map expand to the same list.
    """
    x_dim, y_dim = router_array(cfg)
    entries = []
    for ep in cfg["endpoints"]:
        if ep.get("sbr_port_protocol") is None:
            continue
        num = members(ep)
        attach = attachments(cfg, ep["name"], num, x_dim)
        for r in ep["addr_range"]:
            base, size = int(r["base"]), int(r["size"])
            stride = int(r["stride"]) if r.get("stride") is not None else size
            space = r.get("space", "memory")
            # Spelled out rather than tested against SPACE_ORDER: that constant
            # is the tile crossbar's TARGET order, and reordering the targets
            # must not change which spaces a range may declare.
            if space not in ("config", "memory", "peripheral"):
                raise ValueError(
                    f"endpoint {ep['name']}: space {space!r} is not a declared space")
            for k in range(num):
                x, y, port = attach[k]["x"], attach[k]["y"], attach[k]["port"]
                if not (x < x_dim and y < y_dim):
                    raise ValueError(
                        f"endpoint {ep['name']}: member {k} outside mesh {x_dim}x{y_dim}")
                entries.append({"x": x, "y": y, "size": size, "base": base + stride * k,
                                "dst_id": dst_id(x, y), "space": space, "port": port})
    bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "memory"}
    return bases, entries


def pack_document(doc):
    """Expand either config shape into (bases, entries), dispatching on the shape.

    Twin of nmu/sam_yaml.hpp's load_sam_table(), which dispatches the same way:
    endpoints: is the FlooNoC-shaped config, topology: + address_map: the
    topology YAML it replaces.
    """
    if doc.get("endpoints") is not None:
        return pack_config(doc)
    t = doc["topology"]
    return pack(doc.get("address_map"), int(t["x_dim"]), int(t["y_dim"]))


def node_windows(entries, node_id, port):
    """One endpoint's own region per space, in SPACE_ORDER, present spaces only.

    Keyed on (node_id, port), not on node_id alone: a peripheral shares its host
    router's coordinate and is told apart by the boundary port it hangs off, so
    matching the coordinate alone would stamp a peripheral's window into that
    router's crossbar decode as if it were the tile's. Port 0 is the tile on the
    router's LOCAL port and takes the config and memory windows; a peripheral
    port takes its own single window.

    This is what the tile crossbar decodes on: a request that reached this
    endpoint -- either from its own initiator or from the fabric -- names an
    address in one of these ranges, or it is not this endpoint's. The NSU
    rewrites an arriving address's node-coordinate field to this node before the
    crossbar sees it (nsu::Depacketize::rebase_), so a collective replica
    carrying the request's own address lands here like any unicast.

    Sizes are exact, not rounded: pulp axi_xbar states a rule as start/end, so
    a window need not be a power of two.

    Returns [{"space", "base", "size"}, ...].
    """
    out = []
    for space in SPACE_ORDER:
        for e in entries:
            if (e["dst_id"] == node_id and e.get("port", 0) == port
                    and e.get("space", "memory") == space):
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
