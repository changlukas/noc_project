#!/usr/bin/env python3
"""Analytic average hop count and ideal throughput per traffic pattern.

Usage:
    pattern_metrics.py mesh_4x4
    pattern_metrics.py mesh_4x4 --hotspot 5

For each source node the destinations come from gen_test_patterns.py (the same
functions the stimulus generator calls, so the metric and the traffic cannot
drift apart). Every route is walked XY, x first then y, matching
router.hpp route_compute, and each directed link it crosses takes the flow's
weight. The reported numbers are then

    avg hops         weighted mean of the Manhattan distance
    max channel load max over links of the accumulated weight
    ideal            1 / max channel load, in flits per node per cycle

which is the standard channel load bound (On-Chip Networks 2e ch 7): with every
node injecting one flit per cycle the busiest channel carries `max channel
load` flits per cycle, so the network saturates at its reciprocal. Cross
checked against the textbook mesh rule, max injection = 4 / k for uniform
random on a k by k mesh.

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


def metrics(pattern, x_dim, y_dim, hotspots=None):
    """avg_hops, max_channel_load and ideal_flits_per_node_cycle for one pattern."""
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
    for src, dst, weight in _flows(pattern, x_dim, y_dim, hotspots):
        links = _xy_links(src, dst)
        hops += weight * len(links)
        total_weight += weight
        for link in links:
            load[link] = load.get(link, 0.0) + weight
    max_load = max(load.values()) if load else 0.0
    return {
        "avg_hops": hops / total_weight,
        "max_channel_load": max_load,
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
