#!/usr/bin/env python3
"""CLI summary printer for perf.json (spec §5.1).

Usage: perf_cli_summary.py <perf.json>

Prints the aggregate summary to stdout.  Raw transactions[] are JSON-only;
this script never prints them.
"""

import json
import sys


def _hdr(s):
    print(s)


def print_axi_slots(slots):
    _hdr("  AXI throughput / backpressure")
    fmt = "    {:<24} {:>8} {:>8} {:>6} {:>6} {:>7} {:>7}"
    print(fmt.format(
        "slot", "bytes_wr", "bytes_rd",
        "txn_wr", "txn_rd", "idle_wr", "idle_rd"
    ))
    for s in slots:
        print(fmt.format(
            s.get("name", "?"),
            s.get("write_byte_count", 0),
            s.get("read_byte_count", 0),
            s.get("write_txn_count", 0),
            s.get("read_txn_count", 0),
            s.get("slave_write_idle_cyc", 0),
            s.get("master_read_idle_cyc", 0),
        ))


def print_latency(latency, slots):
    _hdr("  Latency -- end-to-end (manager; min = best-case observed)")
    by_sig = latency.get("by_signature", [])
    if by_sig:
        fmt = "    {:<36} {:>4} {:>5} {:>6} {:>5}"
        print(fmt.format("signature", "n", "min", "mean", "max"))
        for s in by_sig:
            src = s.get("src")
            dst = s.get("dst", "?")
            flow = "{}->{}" .format(src, dst) if src else dst
            sig = "{} {}  len{} size{}".format(
                s.get("op", "?"),
                flow,
                s.get("len", "?"),
                s.get("size", "?"),
            )
            mean_val = s.get("mean", 0)
            if isinstance(mean_val, float):
                mean_str = "{:.1f}".format(mean_val)
            else:
                mean_str = str(mean_val)
            print(fmt.format(sig, s.get("count", 0), s.get("min", 0),
                             mean_str, s.get("max", 0)))

    hist = latency.get("histogram", [])
    nonzero = [b for b in hist if b.get("count", 0) > 0]
    if nonzero:
        parts = []
        for b in nonzero:
            lo = b.get("low", 0)
            hi = b.get("high", 0)
            cnt = b.get("count", 0)
            rng = "[{},{})".format(lo, hi) if hi != 0 else "[{},inf)".format(lo)
            parts.append("{}={}".format(rng, cnt))
        print("    histogram (cyc): " + "  ".join(parts))

    # slave service latency from subordinate slots
    for s in slots:
        if s.get("role") == "subordinate" and "service_latency" in s:
            svc = s["service_latency"]
            w = svc.get("write", {})
            r = svc.get("read", {})
            w_str = str(w.get("min", "?")) if s.get("write_txn_count", 0) > 0 else "n/a"
            r_str = str(r.get("min", "?")) if s.get("read_txn_count", 0) > 0 else "n/a"
            print("    slave service @{}: write {}  read {}".format(
                s.get("name", "?"), w_str, r_str))


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
    slots = data.get("axi_slots", [])
    latency = data.get("latency", {})
    noc = data.get("noc", {})
    txn_count = len(latency.get("transactions", []))

    print("[perf] {}   window [{},{}) cyc".format(scenario, w_start, w_end))
    print_axi_slots(slots)
    print_latency(latency, slots)
    print_noc(noc)
    print("  {} transactions -> {}".format(txn_count, path))


if __name__ == "__main__":
    main()
