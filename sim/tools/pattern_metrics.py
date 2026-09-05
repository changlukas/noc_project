#!/usr/bin/env python3
"""Analytic average hop count and ideal throughput per traffic pattern.

Usage:
    pattern_metrics.py mesh_4x4
    pattern_metrics.py mesh_4x4 --hotspot 5

For each source node the destinations come from gen_test_patterns.py (the same
functions the stimulus generator calls, so the metric and the traffic cannot
drift apart). Every route is walked XY, x first then y, matching
router.hpp route_compute. The reported numbers are then

    avg hops         weighted mean of the Manhattan distance, request path
    max channel load max over links of the accumulated weight
    ideal            1 / max channel load, in flits per node per cycle
    self fraction    share of the weight whose destination is its own node

which is the standard channel load bound (On-Chip Networks 2e ch 7): with every
node injecting one flit per cycle the busiest channel carries `max channel
load` flits per cycle, so the network saturates at its reciprocal.

Both directions count. A flow s to d is an AXI read/write mix, so it puts a
write worm (AW plus its W beats) on the request path s to d and a read reply
(the R beats) on the reverse path d to s, which booksim2 models the same way:
under `_use_read_write` its `_GeneratePacket` injects the reply packet at the
destination (trafficmanager.cpp). The channel load matrix therefore takes both,
split by flit share, because what a measurement compares against is the
accepted throughput at the AXI master, and that counts write payload delivered
forward and read payload delivered back. Charging only the request direction
halves the load on a symmetric permutation, where the reverse path of A to B is
another flow's forward path, so every loaded link carries both and a run at the
true bound would report 200 percent of ideal.

Link set. Only the directed inter-router links count, not the LOCAL
injection and ejection links, matching the textbook definition of channel
load. Self traffic therefore contributes 0 hops and no link load.

Weights, one per source, summing to 1:
    the seven deterministic permutations   its single destination, weight 1
    uniform_random                         every node INCLUDING self, 1 / n
    all_to_all                             every OTHER node, 1 / (n - 1)
    hotspot                                the hotspot set, equal weight
These are the expectations of the generator's samplers. uniform_random permits
self and all_to_all excludes it because uniform_random_dsts and all_to_all_dsts
do (booksim2 applies no source exclusion, all_to_all skips the source by
construction). The hotspot set defaults to the one sim/verilator/Makefile
passes for PATTERN=hotspot (HOTSPOT ?= 5) with the generator's default equal
rates.
"""

import argparse
import sys

import gen_test_patterns as g

PATTERNS = list(g._DETERMINISTIC_PATTERNS) + ["uniform_random", "all_to_all", "hotspot"]

# sim/verilator/Makefile:291 HOTSPOT ?= 5
_DEFAULT_HOTSPOTS = (5,)

# Beats per AXI transaction, AxLEN + 1, at the AxLEN 32 the sweep runs.
_DEFAULT_BEATS = 33

_AI_X_DIM = 4
_AI_Y_DIM = 4
_NATIVE_BYTES_PER_BEAT = 64
_BROADCAST_SHAPES = {
    "broadcast_row": "row",
    "broadcast_col": "col",
    "broadcast_submesh": "submesh",
    "broadcast_global": "global",
}


def _flows(pattern, x_dim, y_dim, hotspots):
    """Yield (src_xy, dst_xy, weight) for every source. Weights sum to 1 per source."""
    n = x_dim * y_dim
    for src in range(n):
        x, y = g._coords(src, x_dim)
        if pattern in g._DETERMINISTIC_PATTERNS:
            yield (x, y), g._dst_for(pattern, x, y, x_dim, y_dim), 1.0
        elif pattern == "uniform_random":
            for dst in range(n):
                yield (x, y), g._coords(dst, x_dim), 1.0 / n
        elif pattern == "all_to_all":
            for dst in range(n):
                if dst != src:
                    yield (x, y), g._coords(dst, x_dim), 1.0 / (n - 1)
        elif pattern == "hotspot":
            for h in hotspots:
                yield (x, y), g._coords(h, x_dim), 1.0 / len(hotspots)
        else:
            raise ValueError(f"unknown pattern {pattern!r} (known: {', '.join(PATTERNS)})")


def _xy_links(src, dst):
    """The directed inter-router links an XY route crosses, x first then y.

    Mirrors router.hpp route_compute: the x coordinate is resolved before the y
    one, so a route is an x run followed by a y run. An empty list means self
    traffic, which never leaves the tile.
    """
    x, y = src
    dst_x, dst_y = dst
    links = []
    while x != dst_x:
        step = x + 1 if dst_x > x else x - 1
        links.append(((x, y), (step, y)))
        x = step
    while y != dst_y:
        step = y + 1 if dst_y > y else y - 1
        links.append(((x, y), (x, step)))
        y = step
    return links


def _node_id(coord):
    return coord[1] * _AI_X_DIM + coord[0]


def _add_resource(resources, name, count):
    resources[name] = resources.get(name, 0) + count


def _add_unicast(resources, plane, src, dst, count):
    """Book one non-local stream on its injection, XY links, and ejection."""
    if src == dst:
        return
    _add_resource(resources, f"{plane}_inject_{src}", count)
    for first, second in _xy_links(
            g._coords(src, _AI_X_DIM), g._coords(dst, _AI_X_DIM)):
        _add_resource(resources,
                      f"{plane}_{_node_id(first)}to{_node_id(second)}", count)
    _add_resource(resources, f"{plane}_eject_{dst}", count)


def _ai_payload_edges(mapping, rounds):
    if mapping in _BROADCAST_SHAPES:
        groups = g.mcast_groups(
            _BROADCAST_SHAPES[mapping], _AI_X_DIM, _AI_Y_DIM)
        return {
            _node_id(source): [_node_id(member) for member in members] * rounds
            for source, members in groups
        }

    n_nodes = _AI_X_DIM * _AI_Y_DIM
    if mapping == "gather_global_root0":
        make_dsts = lambda src: g.gather_dsts(
            src, _AI_X_DIM, _AI_Y_DIM, rounds, "global", 0)
    elif mapping == "gather_submesh":
        make_dsts = lambda src: g.gather_dsts(
            src, _AI_X_DIM, _AI_Y_DIM, rounds, "submesh", 0)
    elif mapping == "alltoall":
        make_dsts = lambda src: g.ai_alltoall_dsts(src, n_nodes, rounds)
    elif mapping == "neighbor_exchange":
        make_dsts = lambda src: g.neighbor_exchange_dsts(
            src, _AI_X_DIM, _AI_Y_DIM, rounds)
    elif mapping == "pipeline":
        make_dsts = lambda src: g.pipeline_dsts(
            src, _AI_X_DIM, _AI_Y_DIM, rounds)
    elif mapping == "many_to_many":
        make_dsts = lambda src: g.many_to_many_dsts(
            src, _AI_X_DIM, _AI_Y_DIM, 4 * rounds)
    else:
        raise ValueError(f"unknown AI mapping {mapping!r}")
    return {src: make_dsts(src) for src in range(n_nodes)}


def _multicast_fork_edges(source, members):
    """Fixed 4x4 fork edges matching route_mask_fork's X-then-Y spread."""
    src_x, src_y = source
    xs = sorted({x for x, _y in members})
    ys = sorted({y for _x, y in members})
    edges = []
    edges += [((x, src_y), (x + 1, src_y)) for x in range(src_x, xs[-1])]
    edges += [((x, src_y), (x - 1, src_y))
              for x in range(src_x, xs[0], -1)]
    for x in xs:
        edges += [((x, y), (x, y + 1)) for y in range(src_y, ys[-1])]
        edges += [((x, y), (x, y - 1)) for y in range(src_y, ys[0], -1)]
    return edges


def _collectb_join_edges(collector, members):
    """Fixed 4x4 join edges matching route_mask_join's X-then-Y returns."""
    dst_x, dst_y = collector
    xs = sorted({x for x, _y in members})
    ys = sorted({y for _x, y in members})
    edges = []
    for y in ys:
        edges += [((x, y), (x - 1, y)) for x in range(xs[-1], dst_x, -1)]
        edges += [((x, y), (x + 1, y)) for x in range(xs[0], dst_x)]
    edges += [((dst_x, y), (dst_x, y - 1))
              for y in range(ys[-1], dst_y, -1)]
    edges += [((dst_x, y), (dst_x, y + 1))
              for y in range(ys[0], dst_y)]
    return edges


def ai_resource_flits(mapping, direction, burst_beats, rounds, multicast_mode):
    """Return exact native-plane flits per physical NoC resource on a 4x4 mesh."""
    if direction not in ("read", "write"):
        raise ValueError(f"unknown direction {direction!r}")
    if burst_beats <= 0 or rounds <= 0:
        raise ValueError("burst_beats and rounds must be positive")

    payload_edges = _ai_payload_edges(mapping, rounds)
    if mapping in _BROADCAST_SHAPES:
        if direction != "write":
            raise ValueError("Broadcast Read is not supported")
        if multicast_mode not in ("hardware", "repeated_unicast"):
            raise ValueError("broadcast multicast_mode must be hardware or repeated_unicast")
    elif multicast_mode is not None:
        raise ValueError("multicast_mode applies only to Broadcast")

    resources = {}
    if mapping in _BROADCAST_SHAPES and multicast_mode == "hardware":
        groups = g.mcast_groups(
            _BROADCAST_SHAPES[mapping], _AI_X_DIM, _AI_Y_DIM)
        for source, members in groups:
            src = _node_id(source)
            dat_flits = (burst_beats + 1) * rounds
            _add_resource(resources, f"dat_inject_{src}", dat_flits)
            for first, second in _multicast_fork_edges(source, members):
                _add_resource(resources,
                              f"dat_{_node_id(first)}to{_node_id(second)}", dat_flits)
            for member in members:
                node = _node_id(member)
                _add_resource(resources, f"dat_eject_{node}", dat_flits)
                _add_resource(resources, f"rsp_inject_{node}", rounds)
            for first, second in _collectb_join_edges(source, members):
                _add_resource(resources,
                              f"rsp_{_node_id(first)}to{_node_id(second)}", rounds)
            _add_resource(resources, f"rsp_eject_{src}", rounds)
        return resources

    request_edges = (g.reverse_payload_edges(payload_edges)
                     if direction == "read" else payload_edges)
    for src, destinations in request_edges.items():
        for dst in destinations:
            if direction == "write":
                _add_unicast(resources, "dat", src, dst, burst_beats + 1)
                _add_unicast(resources, "rsp", dst, src, 1)
            else:
                _add_unicast(resources, "req", src, dst, 1)
                _add_unicast(resources, "dat", dst, src, burst_beats)
    return resources


def ideal_throughput_bound(mapping, direction, burst_beats, rounds, multicast_mode):
    """Logical delivered bytes divided by the busiest resource's flit cycles."""
    resources = ai_resource_flits(
        mapping, direction, burst_beats, rounds, multicast_mode)
    deliveries = sum(map(len, _ai_payload_edges(mapping, rounds).values()))
    return deliveries * burst_beats * _NATIVE_BYTES_PER_BEAT / max(resources.values())


def metrics(pattern, x_dim, y_dim, hotspots=None, beats=_DEFAULT_BEATS):
    """avg_hops, max_channel_load and ideal_flits_per_node_cycle for one pattern.

    `beats` is AxLEN + 1, which sets how one AX pair splits over the two
    directions: the write worm is AW plus its `beats` W flits on the request
    path and the read reply is `beats` R flits on the reverse path, so the pair
    is 2 * beats + 1 flits and the forward share is (beats + 1) of them."""
    forward_share = (beats + 1) / (2 * beats + 1)
    reverse_share = beats / (2 * beats + 1)
    if pattern == "transpose":
        g._check_transpose_guard(x_dim, y_dim)
    if pattern in ("bit_complement", "bit_reverse", "shuffle", "bit_rotation"):
        g._check_bit_permutation_guard(pattern, x_dim, y_dim)
    if pattern == "tornado":
        g._check_tornado_guard(x_dim, y_dim)
    hotspots = list(_DEFAULT_HOTSPOTS if hotspots is None else hotspots)
    for h in hotspots:
        if not 0 <= h < x_dim * y_dim:
            raise ValueError(f"hotspot node id {h} out of range [0, {x_dim * y_dim})")

    load = {}
    hops = 0.0
    total_weight = 0.0
    self_weight = 0.0
    for src, dst, weight in _flows(pattern, x_dim, y_dim, hotspots):
        links = _xy_links(src, dst)
        hops += weight * len(links)
        total_weight += weight
        if src == dst:
            self_weight += weight
        for link in links:
            load[link] = load.get(link, 0.0) + weight * forward_share
        # The read reply walks back XY from the destination, x first from d.
        for link in _xy_links(dst, src):
            load[link] = load.get(link, 0.0) + weight * reverse_share
    max_load = max(load.values()) if load else 0.0
    return {
        "avg_hops": hops / total_weight,
        "max_channel_load": max_load,
        # Share of the offered traffic whose destination is its own node. The
        # tile crossbar answers it, so it reaches neither the NoC nor a
        # monitor: a measurement compares against `1 - self_fraction` of ideal.
        "self_fraction": self_weight / total_weight,
        # All self traffic (a 1x1 mesh is illegal, so only a degenerate hotspot
        # set can reach this) never loads a channel and has no bound.
        "ideal_flits_per_node_cycle": float("inf") if max_load == 0 else 1.0 / max_load,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Markdown table of avg hops and ideal throughput per traffic pattern.")
    ap.add_argument("topology", nargs="?", default="mesh_4x4",
                    help="Configuration name (matches sim/configs/<name>.yml) or a "
                         "direct path to a config file")
    ap.add_argument("--hotspot", type=int, nargs="+", default=None,
                    help=f"Hotspot node id(s) (default {list(_DEFAULT_HOTSPOTS)}, "
                         f"what the Makefile passes)")
    a = ap.parse_args(argv)
    _nodes, x_dim, y_dim, *_rest = g._load_topology(a.topology)

    print(f"### {a.topology} ({x_dim}x{y_dim}), XY routing\n")
    print("| pattern | avg hops | max channel load | ideal flits per node per cycle |")
    print("|---|---|---|---|")
    for pattern in PATTERNS:
        m = metrics(pattern, x_dim, y_dim, a.hotspot)
        print(f"| {pattern} | {m['avg_hops']:.3f} | {m['max_channel_load']:.3f} "
              f"| {m['ideal_flits_per_node_cycle']:.3f} |")


if __name__ == "__main__":
    main(sys.argv[1:])
