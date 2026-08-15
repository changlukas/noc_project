#!/usr/bin/env python3
"""CLI summary printer for perf.json (spec §5.1, NoC section).

Usage: perf_cli_summary.py <perf.json>

Prints per-router fifo occupancy, per-link flit/stall counters, and a
per-network roll-up of those links.
"""

import json
import sys


def _hdr(s):
    print(s)


def print_noc(noc):
    routers = noc.get("routers", [])
    links = noc.get("links", [])
    if not routers and not links:
        return
    _hdr("  NoC")
    rfmt = "    {:<16} {:>10} {:>11}"
    lfmt = "    {:<18} {:>4} {:>5}"
    print(rfmt.format("router", "in_occ_max", "out_occ_max") +
          "     " + lfmt.format("link", "flit", "stall").lstrip())

    max_rows = max(len(routers), len(links))
    for i in range(max_rows):
        r_part = ""
        l_part = ""
        if i < len(routers):
            r = routers[i]
            r_part = rfmt.format(
                r.get("name", "?"),
                r.get("in_fifo_occ_max", 0),
                r.get("out_fifo_occ_max", 0),
            )
        else:
            r_part = rfmt.format("", "", "")

        if i < len(links):
            lk = links[i]
            l_part = "     {:<18} {:>4} {:>5}".format(
                lk.get("name", "?"),
                lk.get("flit_count", 0),
                lk.get("stall_cyc", 0),
            )
        print(r_part + l_part)

    print_net_rollup(links)


def print_net_rollup(links):
    """Per-network totals over the links.

    gen_tb_top.py names an inter-router link monitor "{net}_{i}to{peer}" and a
    peripheral one "{net}_{endpoint}_to_{endpoint}", so in both the network is
    the name up to the first underscore (req / rsp / dat).

    No grand total across networks: stall_cyc means valid && !ready on the
    ready/valid networks (req, rsp) and a credit-starvation cycle on dat
    (gen_tb_top.py:414-421), so the three columns are not the same quantity.
    """
    nets = {}
    for lk in links:
        net = lk.get("name", "?").split("_", 1)[0]
        flit, stall = nets.get(net, (0, 0))
        nets[net] = (flit + lk.get("flit_count", 0),
                     stall + lk.get("stall_cyc", 0))
    if not nets:
        return
    fmt = "    {:<16} {:>10} {:>11}"
    print("")
    print(fmt.format("network", "flit", "stall"))
    for net in sorted(nets):
        flit, stall = nets[net]
        print(fmt.format(net, flit, stall))


def main():
    if len(sys.argv) < 2:
        print("usage: perf_cli_summary.py <perf.json>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print("[perf] ERROR reading {}: {}".format(path, e), file=sys.stderr)
        sys.exit(1)

    scenario = data.get("scenario", "?")
    window = data.get("window", {})
    w_start = window.get("start_cyc", 0)
    w_end = window.get("end_cyc", "?")
    noc = data.get("noc", {})

    print("[perf] {}   window [{},{}) cyc".format(scenario, w_start, w_end))
    print_noc(noc)


if __name__ == "__main__":
    main()
