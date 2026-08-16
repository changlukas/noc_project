"""Shared packed address-map helper for the sim-side generators.

Mirrors c_model SamTable::validate (nmu/addr_trans.hpp) so gen_test_patterns.py
and gen_tb_top.py compute the same base(dst_id) as the C++ SAM from the same
config file.

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
dst_id = (y << X_WIDTH) | x.

pack_config() is the twin of nmu/sam_yaml.hpp load_config_table(). Both readers
must expand a config to the same ordered rules;
sim/tools/test_sam_config_parity.py and c_model's tests/nmu/test_sam_config.cpp
hold each side to the same file.
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


# XYDirections (FlooNoC routing.py:245-252) -> the router port an endpoint hangs
# off. EJECT is the LOCAL port the tile owns; EAST/WEST are the x face and
# NORTH/SOUTH the y face.
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
    """Expand a FlooNoC-shaped config into (bases, entries).

    bases:   {dst_id: base} for the memory space only -- gen_test_patterns.py
             and gen_tb_top.py want the node's data-class base, not a config
             aperture.
    entries: ordered [{"x", "y", "size", "base", "dst_id", "space", "port"}, ...].

    An endpoint contributes to the SAM only if it declares sbr_port_protocol
    (FlooNoC endpoint.py:87-89). mgr_port_protocol / sbr_port_protocol are
    presence markers and nothing more: the protocols: block they would resolve
    against is omitted (design 3.6), the four AXI widths living in
    specgen/source/constants.yaml instead, so the labels themselves are ignored.

    Rules come out range-major -- every member of a range, then the next range --
    which is the order nmu/sam_yaml.hpp load_config_table() walks them in.
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
