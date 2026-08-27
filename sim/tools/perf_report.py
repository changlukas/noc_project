#!/usr/bin/env python3
"""Render the standard performance report from the runs under an output dir.

    perf_report.py [output dir] [-o perf_report.md]

Every `continuous_*/result.csv` under the output dir is one measured point. Points
are grouped by their parameter tuple and pattern and ordered by offered load, so a
group with three or more offered points is a latency-vs-load curve and a group with
fewer is a single operating point.

Method, in one place, because every number below depends on it:

    offered   analytic, from the injection rate and the burst length
              (emit_result_csv.py), in DAT network flits per node per cycle
    accepted  measured at the AXI master monitors, bytes per node per cycle,
              converted to DAT network flits by the same 34 / 33 convention
    nlat      monitor latency, from the AX handshake
    plat      nlat plus the open-loop source queue delay, booksim2's packet latency
    3x rule   saturation is the offered load at which plat reaches three times its
              value at the lowest offered point, linearly interpolated

Percent of ideal charges the analytic ideal only for the traffic that reaches the
NoC. A pattern that permits self traffic answers that share in the tile crossbar,
where no monitor sees it, so the comparison is against `(1 - self_fraction) * ideal`.
"""

import argparse
import csv
import json
import pathlib
import re
import sys

import pattern_metrics as pm

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    plt = None

# AXI data bus width in bytes (specgen DATA_WIDTH 512 b), what the monitor
# charges per beat. emit_result_csv.py charges the same on the offered side.
BEAT_BYTES = 64

_PARAM_COLS = ("topology", "vc", "router_depth", "ni_rx_depth", "outstanding",
               "txns_per_id", "ids/init", "burst_len", "txns/node", "mst_stall")

_NI_PARAMS_H = (pathlib.Path(__file__).resolve().parents[2] /
                "specgen/generated/cpp/ni_params.h")


def ni_params():
    """{name: value} from the specgen NI parameter header, so the report never
    carries a second copy of a generated constant."""
    if not _NI_PARAMS_H.exists():
        return {}
    return dict(re.findall(r"constexpr int (\w+)\s*=\s*(\d+);", _NI_PARAMS_H.read_text()))


_P = ni_params()
# Only the three the report calls out as a sensitivity axis need a default; a
# group differing in any of them gets its own appendix row.
_DEFAULTS = {"vc": _P.get("NOC_DAT_NUM_VC", "2"),
             "router_depth": _P.get("NOC_ROUTER_VC_DEPTH", "8"),
             "ni_rx_depth": _P.get("NOC_NI_DAT_RX_VC_DEPTH", "8")}


def accepted_flits(bytes_per_node_cycle, burst_len):
    """Accepted bytes per node per cycle as DAT network flits per node per cycle.

    One AXI transaction of `beats` beats is `1 + beats` DAT flits when it is a
    write (AW header plus its W beats) and `beats` when it is a read (R beats,
    AR rides REQ). Read and write are offered in equal measure, and the monitor
    charges `beats` full beats either way, so bytes convert at
    (2 * beats + 1) / (2 * beats): 67 / 66 at AxLEN 32."""
    beats = int(burst_len) + 1
    return bytes_per_node_cycle / BEAT_BYTES * (2 * beats + 1) / (2 * beats)


def dat_link_util(perf_path):
    """(mean, max, min) DAT inter-router link utilization from a run's perf.json:
    flit_count / window cycles per link, 1 flit per cycle being the link's
    capacity. None when the run carries no perf.json."""
    if not perf_path.exists():
        return None
    perf = json.loads(perf_path.read_text())
    cyc = perf["window"]["end_cyc"] - perf["window"]["start_cyc"]
    if cyc <= 0:
        return None
    utils = [l["flit_count"] / cyc for l in perf["noc"]["links"]
             if l["name"].startswith("dat_")]
    return (sum(utils) / len(utils), max(utils), min(utils)) if utils else None


def collect(out_root, stale=None):
    """{param_tuple: {pattern: [point]}}, param_tuple ordered as _PARAM_COLS.

    A continuous run without a result.csv aborted before its monitors reported
    and carries no measurement, so it is skipped rather than shown as a hole.
    So is a row written before `offered_flits_per_node_cycle` and
    `mean_latency_open` existed: it has neither an offered load nor a packet
    latency, so it is not a point on any curve. Those run directories are named
    in `stale` and the report says how many it left out."""
    groups = {}
    for run_dir in sorted(p for p in out_root.iterdir() if p.is_dir()):
        csv_path = run_dir / "result.csv"
        if not run_dir.name.startswith("continuous_") or not csv_path.exists():
            continue
        row = next(csv.DictReader(csv_path.open()))
        if "offered_flits_per_node_cycle" not in row or "mean_latency_open" not in row:
            if stale is not None:
                stale.append(run_dir.name)
            continue
        accepted = row.get("accepted_bytes_per_node_cycle") or ""
        # Per channel latency, empty on a row written before the columns existed
        # and on a channel that retired nothing. `-` in the table, never a zero.
        chan = lambda col: float(row[col]) if row.get(col) else None
        key = (row["topology"], row["vc"], row.get("router_vc_depth") or "?",
               row.get("ni_dat_rx_vc_depth") or "?", row["max_outstanding"],
               row.get("max_txns_per_id", "32"), row.get("ids_per_initiator", "1"),
               row.get("burst_len", "0"), row["injection_count"],
               row.get("mst_stall_random") or "-")
        groups.setdefault(key, {}).setdefault(row["pattern"], []).append({
            "seed": row["seed"],
            "offered": float(row["offered_flits_per_node_cycle"]),
            "offered_bytes": float(row["offered_bytes_per_node_cycle"]),
            "accepted_bytes": float(accepted) if accepted else None,
            "nlat": float(row["mean_latency_network"]),
            "plat": float(row["mean_latency_open"]),
            "nlat_read": chan("mean_latency_network_read"),
            "nlat_write": chan("mean_latency_network_write"),
            "plat_read": chan("mean_latency_open_read"),
            "plat_write": chan("mean_latency_open_write"),
            "dat_util": dat_link_util(run_dir / "perf.json"),
        })
    return groups


_PROBE_MON = re.compile(r"\[Monitor node0\.master\]\[(Read|Write)\]\s+Latency:\s*([\d.]+)")
_PROBE_SRCQ = re.compile(r"\[SrcQueue node0\]\[(Read|Write)\]\s+mean:\s*([\d.]+)")


def zero_load_probes(out_root):
    """The narrow and data zero-load probe runs, read straight out of run.log.

    node0 is the only loaded node of these runs, so its monitor means are the
    unloaded latency of one traffic class. The class is the tag suffix, which is
    how the recipe in docs/backlog.md names them. A probe from before the open
    loop tb has no [SrcQueue node0] line, and its `plat` is then unknown rather
    than equal to `nlat`."""
    probes = []
    for run_dir in sorted(p for p in out_root.iterdir() if p.is_dir()):
        log = run_dir / "run.log"
        if not run_dir.name.startswith("s2zl") or not log.exists():
            continue
        text = log.read_text(errors="ignore")
        nlat = {ch: float(v) for ch, v in _PROBE_MON.findall(text)}
        if not nlat:
            continue
        probes.append({
            "tag": run_dir.name,
            "space": "narrow" if run_dir.name.endswith("_narrow") else "data",
            "nlat": nlat,
            "srcq": {ch: float(v) for ch, v in _PROBE_SRCQ.findall(text)},
        })
    return probes


def _mean(values):
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


def curve(points):
    """One row per offered load, seeds folded into a mean and a spread."""
    by_offered = {}
    for p in points:
        by_offered.setdefault(round(p["offered"], 6), []).append(p)
    rows = []
    for offered in sorted(by_offered):
        seeds = by_offered[offered]
        plats = [p["plat"] for p in seeds]
        rows.append({
            "offered": offered,
            "offered_bytes": seeds[0]["offered_bytes"],
            "accepted_bytes": _mean(p["accepted_bytes"] for p in seeds),
            "nlat": _mean(p["nlat"] for p in seeds),
            "plat": sum(plats) / len(plats),
            "nlat_read": _mean(p["nlat_read"] for p in seeds),
            "nlat_write": _mean(p["nlat_write"] for p in seeds),
            "plat_read": _mean(p["plat_read"] for p in seeds),
            "plat_write": _mean(p["plat_write"] for p in seeds),
            # One seed is one sample, and a spread of zero would read as three
            # seeds that happened to agree.
            "spread": None if len(plats) == 1 else max(plats) - min(plats),
            "seeds": len(seeds),
            "dat_util": _mean(p["dat_util"][0] for p in seeds if p["dat_util"]),
            "dat_util_max": max((p["dat_util"][1] for p in seeds if p["dat_util"]),
                                default=None),
            "dat_util_min": min((p["dat_util"][2] for p in seeds if p["dat_util"]),
                                default=None),
        })
    return rows


def saturation(points):
    """The 3x rule: the offered load at which plat reaches three times its value
    at the lowest offered point, linearly interpolated between the two points
    that bracket the crossing. None when the curve never reaches 3x.

    Accepted is interpolated on the same fraction, which is the throughput the
    network was carrying when latency ran away."""
    rows = curve(points)
    if len(rows) < 2:
        return None
    threshold = 3 * rows[0]["plat"]
    for a, b in zip(rows, rows[1:]):
        if a["plat"] < threshold <= b["plat"]:
            f = (threshold - a["plat"]) / (b["plat"] - a["plat"])
            lerp = lambda k: (None if a[k] is None or b[k] is None
                              else a[k] + f * (b[k] - a[k]))
            return {"offered": a["offered"] + f * (b["offered"] - a["offered"]),
                    "accepted_bytes": lerp("accepted_bytes"),
                    "threshold": threshold}
    return None


def analytic(pattern, topology):
    """pattern_metrics for a mesh_<x>x<y> topology. None when the topology is
    not a mesh or the pattern has no definition on those dimensions (the bit
    permutations need a power-of-two node count).

    SystemExit is caught because the shape guards it borrows from
    gen_test_patterns.py end the process, which is the right contract for a
    stimulus generator asked for one pattern and the wrong one for a report
    surveying ten."""
    m = re.match(r"mesh_(\d+)x(\d+)$", topology)
    if not m:
        return None
    try:
        return pm.metrics(pattern, int(m.group(1)), int(m.group(2)))
    except (ValueError, SystemExit):
        return None


##########
# Layout #
##########


def table(header, rows):
    rows = [[str(c) for c in r] for r in rows]
    widths = [max([len(h)] + [len(r[i]) for r in rows]) for i, h in enumerate(header)]
    line = lambda cells: "| " + " | ".join(c.ljust(w) for c, w in zip(cells, widths)) + " |"
    out = [line(header), "|" + "|".join("-" * (w + 2) for w in widths) + "|"]
    return "\n".join(out + [line(r) for r in rows]) + "\n"


def fmt(value, digits=1):
    return "-" if value is None else f"{value:.{digits}f}"


def group_label(key):
    """Name a group by how it differs from the shipped specgen defaults."""
    diffs = [f"{col}={val}" for col, val in zip(_PARAM_COLS, key)
             if col in _DEFAULTS and val not in ("?", "-") and val != _DEFAULTS[col]]
    return ", ".join(diffs) if diffs else "default"


def _seeds_rule(patterns):
    """Which offered points carry more than one seed, read off the runs."""
    repeated = {}
    for pattern in sorted(patterns):
        for r in curve(patterns[pattern]):
            if r["seeds"] > 1:
                repeated.setdefault((round(r["offered"], 3), r["seeds"]), []).append(pattern)
    extra = [f"{', '.join(pats)} carry {n} seeds at offered {offered:.3f}"
             for (offered, n), pats in sorted(repeated.items())]
    return "seed 1 on every point. " + (". ".join(extra) if extra
                                        else "No point carries a second seed")


def _batch_note(patterns):
    """The accepted against offered shortfall at the two lowest offered points of
    a curve, so the accepted row states the batch overhang from the runs."""
    for pattern in ["bit_complement"] + sorted(patterns):
        rows = curve(patterns.get(pattern, []))
        if len(rows) >= 3 and all(r["accepted_bytes"] for r in rows[:2]):
            return pattern + " accepts " + " and ".join(
                f"{100 * r['accepted_bytes'] / r['offered_bytes']:.0f} percent of "
                f"offered at {r['offered']:.3f}" for r in rows[:2])
    return "no curve here carries an accepted column"


def section_method(key, patterns):
    topology, vc, router_depth, ni_rx_depth, outstanding, txns_per_id, ids, burst, \
        count, mst_stall = key
    beats = int(burst) + 1 if burst.isdigit() else "?"
    # The shortest offered window of the whole report: the highest offered point
    # is the highest Bernoulli rate, so it is the first to run out of work.
    top = max(p["offered"] for points in patterns.values() for p in points)
    rate = top / (2 * beats + 1)
    window = f"`p` = {rate:.4f} per channel at the highest offered point " \
             f"({top:.3f} flits per node per cycle), so the offered window is at " \
             f"least {int(count) / rate:.0f} cycles"
    out = ["## 1 Method\n",
           "The machine under measurement and the definitions every later number "
           "depends on.\n\n"]
    out.append(table(["item", "value"], [
        ["topology", f"{topology}, XY routing, wormhole"],
        ["networks", "REQ and RSP narrow, DAT wide"],
        ["router pipeline", "REQ and RSP 2 cycles per router, DAT head 4 cycles "
                            "then one flit per cycle (docs/router-spec.md 2.4)"],
        ["flow control", "credit per (output, VC) on DAT, ready/valid on REQ and RSP"],
        ["DAT VCs", f"`NOC_DAT_NUM_VC` = {vc}"],
        ["router input VC FIFO", f"`NOC_ROUTER_VC_DEPTH` = {router_depth}"],
        ["NI DAT receive FIFO", f"`NOC_NI_DAT_RX_VC_DEPTH` = {ni_rx_depth}"],
        ["NMU per ID depth", f"`NMU_MAX_TXNS_PER_ID` = {txns_per_id}"],
        ["NSU outstanding", f"`NSU_META_BUFFER_MAX_OUTSTANDING` = {outstanding}"],
        ["AXI IDs per initiator", ids],
        ["burst", f"AxLEN {burst} ({beats} beats) of {BEAT_BYTES} B"],
        ["patterns", ", ".join(sorted(patterns))],
    ]))
    out.append("\n### Injection\n")
    out.append(table(["point", "rule"], [
        ["model", "open loop, Bernoulli per node per channel (AW, AR) per cycle"],
        ["`qtime`", "advanced every cycle by the trial, never blocked by "
                    "`awready` or `arready`"],
        ["source queue delay", "AX handshake cycle minus the slot's `qtime`"],
        ["run length", f"{count} transactions per node, fixed. {window}"],
        ["seeds", _seeds_rule(patterns)],
        ["backpressure", "ideal master face, ideal memory: the curve measures "
                         "the fabric, not a consumer that stalls first"],
    ]))
    out.append("\n### Latency and flits\n")
    out.append(table(["term", "definition"], [
        ["`nlat`", "the monitor value, from the AX handshake at the master. A "
                   "read ends at its first R beat, a write at B, so the two are "
                   "different measurements and section 2 keeps them apart"],
        ["`plat`", "`nlat` plus the source queue delay, booksim2's packet latency"],
        ["zero load", "`plat` at the lowest offered point"],
        ["saturation", "offered load where `plat` reaches 3x zero load, linearly "
                       "interpolated"],
        ["network flits", f"every DAT flit: a write is {beats + 1} flits (AW header "
                          f"plus its W beats), a read {beats}"],
        ["offered", "analytic, `rate * (1 + beats) + rate * beats` flits per node "
                    "per cycle"],
        ["accepted", f"measured bytes per node per cycle / {BEAT_BYTES} * "
                     f"(2 * {beats} + 1) / (2 * {beats}) flits, the same convention"],
        ["accepted window", f"one whole run batch throughput, total delivered over "
                            f"the `perf.json` window. Below saturation that window "
                            f"outlives the offered traffic by the overhang of the "
                            f"slowest node finishing its fixed {count} transaction "
                            f"batch, so accepted sits under offered: "
                            f"{_batch_note(patterns)}. Only once congestion sets "
                            f"the run length do the two meet"],
        ["percent of ideal", "accepted flits / ((1 - self fraction) * ideal). Self "
                             "addressed traffic is answered by the tile crossbar "
                             "and reaches no monitor"],
    ]))
    return "".join(out)


# docs/router-spec.md 2.4 and the retired Scenario 2 probe: one node, single beat
# AxSIZE 3, node0 to node3, three hops east, four routers, no competing traffic.
_STAGES = [
    ["master AR handshake to NI egress flit", 3, 3],
    ["4 request routers on REQ, 2 cycles each", 8, 8],
    ["NSU ingress to slave arvalid", 2, 2],
    ["slave arvalid to rlast", 3, 3],
    ["slave R to NSU egress flit", 4, 4],
    ["DAT merge, NSU pins to router LOCAL", 0, 1],
    ["4 response routers, RSP 2 cycles each, DAT 4 cycles each", 8, 16],
    ["DAT merge, router LOCAL to NMU pins", 0, 1],
    ["NMU ingress to master rvalid", 3, 3],
]
# Everything in the static decomposition that is neither a response router nor a
# payload beat: the fixed NI, slave and merge cost of a data read.
_DATA_READ_FIXED = sum(r[2] for r in _STAGES) - 16


def _probe_block(probes):
    """The measured narrow against data probe rows, the thing the static per
    stage table below them is an account of."""
    if not probes:
        return ("\nNo `s2zl*` zero-load probe run sits under this output dir, so "
                "the per stage table below has no measured column beside it.\n")
    rows = []
    for p in sorted(probes, key=lambda p: (p["space"], p["tag"])):
        cells = [p["tag"], p["space"]]
        for channel in ("Read", "Write"):
            nlat, srcq = p["nlat"].get(channel), p["srcq"].get(channel)
            cells += [fmt(nlat), "-" if nlat is None or srcq is None else fmt(nlat + srcq)]
        rows.append(cells)
    out = ["\nMeasured on the single node zero-load probe: node0 issues single "
           "beat reads and writes on one AXI ID over three hops with the mesh "
           "otherwise idle, and the target address space selects the class. This "
           "is the measurement the per stage table below accounts for.\n\n",
           table(["probe", "class", "read nlat", "read plat", "write nlat",
                  "write plat"], rows)]
    if any(c == "-" for r in rows for c in r):
        out.append("\nA `-` under `plat` is a probe run before the open loop tb, "
                   "whose log carries no `[SrcQueue node0]` line. Its source queue "
                   "delay is unknown, not zero.\n")
    return "".join(out)


def section_zero_load(key, patterns, probes=()):
    out = ["## 2 Zero-load latency\n",
           "Measured at the lowest offered point of each curve, where the source "
           "queue is empty and `plat` equals `nlat`. A pattern measured at a "
           "single operating point is not listed: its one point is saturated, "
           "not unloaded.\n\n"]
    rows = []
    for pattern in sorted(patterns):
        rows_c = curve(patterns[pattern])
        if len(rows_c) < 3:
            continue
        m = analytic(pattern, key[0])
        hops = m["avg_hops"] if m else None
        ideal = None if hops is None else _DATA_READ_FIXED + 4 * (hops + 1)
        first = rows_c[0]
        gap = (None if ideal is None or first["plat_read"] is None
               else first["plat_read"] - ideal)
        rows.append([pattern, fmt(first["offered"], 3), fmt(hops, 2),
                     fmt(first["nlat_read"]), fmt(first["plat_read"]),
                     fmt(ideal, 0), fmt(gap),
                     fmt(first["nlat_write"]), fmt(first["plat_write"])])
    out.append(table(["pattern", "offered", "avg hops", "read nlat", "read plat",
                      "read ideal", "read gap", "write nlat", "write plat"], rows))
    out.append(
        "\n`read ideal` is the static decomposition below generalized to the "
        "measured hop count: the fixed NI, slave and merge cost of a data read "
        f"({_DATA_READ_FIXED} cycles), plus 4 cycles in each of the `avg hops` + 1 "
        "response routers a DAT head passes. It carries no burst term because "
        "`axi_bw_monitor.sv` timestamps a read at its first R beat rather than at "
        "`rlast`, so the rest of the worm falls outside the measurement. "
        "`read gap` is `read plat` minus `read ideal`, which is queueing plus the "
        "cost of a mean over a hop distribution.\n"
        "\nThe write columns carry no analytic. A write is timestamped at B, "
        "after every W beat has landed, and its response returns on RSP rather "
        "than DAT, so the read decomposition is not an account of it.\n")
    out.append(_probe_block(probes))
    out.append(
        "\nPer stage, single beat, three hops, from `docs/router-spec.md` 2.4 and "
        "`docs/nmu-spec.md` / `docs/nsu-spec.md` rule 8. The narrow column is a "
        "request on REQ answered on RSP, the data column a request on REQ answered "
        "on DAT.\n\n")
    out.append(table(["stage", "narrow read", "data read"],
                     [[s, str(n), str(d)] for s, n, d in _STAGES] +
                     [["total", str(sum(r[1] for r in _STAGES)),
                       str(sum(r[2] for r in _STAGES))]]))
    out.append(
        "\nThe 10 cycle gap is the DAT return path: 2 cycles more per router over "
        "4 routers, plus 1 cycle at each end for the DAT merge. A write follows "
        "the same shape with AW and W on DAT and B on RSP, 32 and 42.\n")
    return "".join(out)


def _curve_rows(rows, sat, burst):
    marked = False
    out = []
    for r in rows:
        mark = ""
        if sat and not marked and r["offered"] >= sat["offered"]:
            mark, marked = "*", True
        acc_flits = (None if r["accepted_bytes"] is None
                     else accepted_flits(r["accepted_bytes"], burst))
        out.append([fmt(r["offered"], 3) + mark, fmt(r["offered_bytes"], 1),
                    fmt(acc_flits, 3), fmt(r["accepted_bytes"], 1),
                    fmt(r["nlat"]), fmt(r["plat"]), fmt(r["spread"]),
                    str(r["seeds"])])
    return out


def _throughput_note(rows, sat, burst):
    """Where throughput stops climbing, against where latency runs away.

    The 3x rule reads latency alone, so it can fire while the network is still
    taking more work. The knee stated here is the first offered load whose
    accepted throughput is within 5 percent of the curve's maximum, which is
    what the throughput side of the same curve calls saturated."""
    acc = [(r["offered"], accepted_flits(r["accepted_bytes"], burst))
           for r in rows if r["accepted_bytes"] is not None]
    if not acc:
        return ""
    peak = max(a for _offered, a in acc)
    knee = next(offered for offered, a in acc if a >= 0.95 * peak)
    head = (f"Accepted peaks at {peak:.3f} flits per node per cycle and first "
            f"reaches 95 percent of that ({0.95 * peak:.3f}) at offered {knee:.3f}")
    if sat is None:
        return head + ". The curve never reaches 3x zero load, so it carries no " \
                      "latency saturation point.\n"
    rel, tail = (
        ("above", "so latency runs away while throughput is still climbing")
        if knee > sat["offered"] else
        ("below", "so throughput flattens before latency runs away")
        if knee < sat["offered"] else ("at", "so the two sides of the curve agree"))
    return f"{head}, {rel} the 3x point at offered {sat['offered']:.3f}, {tail}.\n"


def section_curves(key, patterns, out_root):
    out = ["## 3 Latency vs offered load\n",
           "One table per pattern with a full curve. `*` marks the first point at "
           "or above the 3x crossing. Offered and accepted are per node per cycle, "
           "flits on the DAT plane and bytes at the AXI master. `spread` is max "
           "minus min of `plat` across the seeds of that point, `-` at a point "
           "with one seed.\n"
           "\n`offered` is per node and counts the traffic a node addresses to "
           "itself. `accepted` averages over all nodes and excludes that traffic, "
           "which the tile crossbar answers where no monitor sees it. The `served` "
           "factor in section 4 reconciles the two.\n"]
    curves = [p for p in sorted(patterns) if len(curve(patterns[p])) >= 3]
    if not curves:
        return "".join(out) + ("\nNo pattern under this output dir carries three or "
                               "more offered points, so there is no curve to draw.\n")
    for pattern in curves:
        rows = curve(patterns[pattern])
        sat = saturation(patterns[pattern])
        out.append(f"\n### {pattern}\n\n")
        out.append(table(["offered", "offered B", "accepted", "accepted B",
                          "nlat", "plat", "spread", "seeds"],
                         _curve_rows(rows, sat, key[7])))
        last = rows[-1]
        if sat:
            acc = ("not recorded" if sat["accepted_bytes"] is None else
                   f"{accepted_flits(sat['accepted_bytes'], key[7]):.3f} flits "
                   f"({sat['accepted_bytes']:.1f} B)")
            out.append(
                f"\nSaturation (3x of {rows[0]['plat']:.1f} = "
                f"{sat['threshold']:.1f} cycles): offered {sat['offered']:.3f} "
                f"flits per node per cycle, accepted {acc}.\n")
        else:
            out.append(f"\nSaturation (3x rule): > {rows[-1]['offered']:.3f} flits "
                       f"per node per cycle, the curve never reaches 3x zero load.\n")
        out.append(
            f"Accepted at the highest offered load ({last['offered']:.3f}): "
            f"{accepted_flits(last['accepted_bytes'], key[7]):.3f} flits "
            f"({last['accepted_bytes']:.1f} B) per node per cycle.\n"
            if last["accepted_bytes"] is not None else "")
        out.append(_throughput_note(rows, sat, key[7]))
        png = plot(pattern, rows, out_root)
        if png:
            out.append(f"\n![{pattern} latency vs offered load]({png})\n")
    return "".join(out)


def plot(pattern, rows, out_root):
    """One PNG per curve pattern beside the report. Skipped without matplotlib."""
    if plt is None:
        return None
    offered = [r["offered"] for r in rows]
    fig, ax = plt.subplots(figsize=(5, 3.2))
    ax.plot(offered, [r["plat"] for r in rows], "o-", label="plat")
    ax.plot(offered, [r["nlat"] for r in rows], "s--", label="nlat")
    ax.set_xlabel("offered load (DAT flits per node per cycle)")
    ax.set_ylabel("latency (cycles)")
    ax.set_title(pattern)
    ax.legend()
    fig.tight_layout()
    name = f"perf_{pattern}.png"
    fig.savefig(out_root / name, dpi=120)
    plt.close(fig)
    return name


def section_summary(key, patterns):
    out = ["## 4 Pattern summary\n",
           "`ideal` is the analytic channel load bound (`pattern_metrics.py`). It "
           "counts both directions, the write worm on the request path and the "
           "read reply on the reverse path, because `accepted at max` counts both "
           "too. "
           "`served` is `1 - self fraction`, the share of the offered traffic that "
           "reaches the NoC at all. `% ideal` charges the accepted flits at the "
           "highest offered load against `served * ideal`. A pattern with fewer "
           "than three offered points contributes its single operating point and "
           "no saturation or zero-load figure.\n"
           "\n`pattern_metrics` counts inter router links only, never the LOCAL "
           "injection and ejection ports. For hotspot and neighbor the real "
           "limiter is the terminal ejection port of the node every flow lands "
           "on, so `% ideal` understates what the fabric delivered.\n\n"]
    rows = []
    notes = {}
    for pattern in sorted(patterns):
        rows_c = curve(patterns[pattern])
        is_curve = len(rows_c) >= 3
        sat = saturation(patterns[pattern]) if is_curve else None
        m = analytic(pattern, key[0])
        last = rows_c[-1]
        acc = (None if last["accepted_bytes"] is None
               else accepted_flits(last["accepted_bytes"], key[7]))
        served = None if m is None else (1 - m["self_fraction"]) * m["ideal_flits_per_node_cycle"]
        pct = None if acc is None or not served else 100 * acc / served
        rows.append([
            pattern,
            fmt(None if m is None else m["avg_hops"], 2),
            fmt(None if m is None else m["ideal_flits_per_node_cycle"], 3),
            fmt(None if m is None else 1 - m["self_fraction"], 3),
            fmt(sat["offered"], 3) if sat else
            (f"> {last['offered']:.3f}" if is_curve else "-"),
            fmt(acc, 3),
            # One decimal: the two patterns that saturate their bottleneck link
            # land at 99.6, and a whole number column would print that as 100
            # and lose the one digit that says the bound holds.
            fmt(pct, 1),
            fmt(rows_c[0]["plat"]) if is_curve else "-",
        ])
        notes[pattern] = (pct, last["dat_util_max"])
    out.append(table(["pattern", "avg hops", "ideal", "served", "saturation (3x)",
                      "accepted at max", "% ideal", "zero-load plat"], rows))
    pct, busiest = notes.get("uniform_random", (None, None))
    if pct is not None and busiest is not None:
        out.append(
            f"\nEvery row but one reads `% ideal` within a decimal of its busiest "
            f"measured DAT link in Appendix A, because model and measurement are "
            f"then the same link. uniform_random is the exception, {pct:.1f} "
            f"against {100 * busiest:.1f}: its bound comes from the bisection, "
            f"while the busiest link the run actually loaded sits elsewhere.\n")
    return "".join(out)


def section_links(key, patterns):
    out = ["## Appendix A: DAT link utilization\n",
           "Inter-router DAT links at the highest offered load of each pattern, "
           "`flit_count` over the perf.json window against the link's 1 flit per "
           "cycle capacity.\n\n"]
    rows = []
    for pattern in sorted(patterns):
        last = curve(patterns[pattern])[-1]
        if last["dat_util"] is None:
            continue
        rows.append([pattern, fmt(last["offered"], 3),
                     fmt(100 * last["dat_util"]), fmt(100 * last["dat_util_min"]),
                     fmt(100 * last["dat_util_max"])])
    if not rows:
        return "".join(out) + "No run under this output dir carries a perf.json.\n"
    return "".join(out) + table(
        ["pattern", "offered", "avg %", "min %", "max %"], rows)


def section_sensitivity(groups, main_key):
    out = ["## Appendix B: parameter sensitivity\n",
           "Groups whose parameter tuple differs from the shipped specgen "
           "defaults, at their highest offered load.\n\n"]
    rows = []
    for key in sorted(groups, key=group_label):
        label = group_label(key)
        if key == main_key or label == "default":
            continue
        for pattern in sorted(groups[key]):
            last = curve(groups[key][pattern])[-1]
            acc = (None if last["accepted_bytes"] is None
                   else accepted_flits(last["accepted_bytes"], key[7]))
            rows.append([label, pattern, fmt(last["offered"], 3), fmt(acc, 3),
                         fmt(last["nlat"]), fmt(last["plat"])])
    if not rows:
        return "".join(out) + "No run under this output dir departs from the defaults.\n"
    return "".join(out) + table(
        ["group", "pattern", "offered", "accepted", "nlat", "plat"], rows)


def report(out_root):
    stale = []
    groups = collect(out_root, stale)
    if not groups:
        sys.exit(f"perf_report: no continuous_*/result.csv under {out_root} carries the "
                 f"offered load and open-loop latency columns"
                 f"{f' ({len(stale)} run dirs predate them)' if stale else ''}")
    # The report's subject is the machine as built: the default group, and the
    # one with the most measured points when several share the label.
    main_key = max(groups, key=lambda k: (group_label(k) == "default",
                                          sum(len(v) for v in groups[k].values())))
    patterns = groups[main_key]
    curves = [p for p in sorted(patterns) if len(curve(patterns[p])) >= 3]
    singles = [p for p in sorted(patterns) if len(curve(patterns[p])) < 3]
    body = [f"# Performance report: {main_key[0]}\n",
            f"\nGenerated by `sim/tools/perf_report.py` from `{out_root}`. "
            f"Parameter set: {group_label(main_key)}.\n",
            f"\nCurve patterns: {', '.join(curves) or 'none'}. Single operating "
            f"point: {', '.join(singles) or 'none'}.\n\n",
            section_method(main_key, patterns), "\n",
            section_zero_load(main_key, patterns, zero_load_probes(out_root)), "\n",
            section_curves(main_key, patterns, out_root), "\n",
            section_summary(main_key, patterns), "\n",
            section_links(main_key, patterns), "\n",
            section_sensitivity(groups, main_key)]
    if plt is None:
        body.insert(3, "matplotlib is not installed, so the curves are tables only.\n\n")
    if stale:
        body.insert(3, f"{len(stale)} run directories are left out: their `result.csv` "
                       f"predates the offered load and open-loop latency columns, so they "
                       f"carry no point on any curve. First: `{stale[0]}`.\n\n")
    return "".join(body)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out_dir", nargs="?", default="sim/verilator/output",
                    help="Directory holding the continuous_* run directories")
    ap.add_argument("-o", "--out", default=None,
                    help="Report path (default <out_dir>/perf_report.md)")
    a = ap.parse_args(argv)
    out_root = pathlib.Path(a.out_dir)
    text = report(out_root)
    dest = pathlib.Path(a.out) if a.out else out_root / "perf_report.md"
    dest.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {dest} ({len(text.splitlines())} lines)")


if __name__ == "__main__":
    main(sys.argv[1:])
