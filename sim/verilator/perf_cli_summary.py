#!/usr/bin/env python3
"""CLI summary printer for perf.json (spec §5.1, NoC section).

Usage: perf_cli_summary.py <perf.json>

Prints per-router fifo occupancy and per-link flit/stall counters.
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
