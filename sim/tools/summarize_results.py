"""Summarize every run under sim/verilator/output/ as markdown tables.

One parameter set = one section: a one-row parameter table, then a result
table with one row per pattern (status + performance). Runs sharing a
parameter set and pattern are averaged across seeds (n shown when > 1).

Performance columns are filled from continuous (mode 1) runs only; a directed
run is a closed-loop two-phase drain, so it contributes its scoreboard verdict
and no latency/bandwidth.
"""
import csv
import json
import pathlib
import re
import sys
from collections import defaultdict

_PATTERNS = (
    "neighbor", "transpose", "bit_complement", "bit_reverse", "shuffle",
    "bit_rotation", "tornado", "uniform_random", "all_to_all", "hotspot",
    "multicast",
)
_TAG = re.compile(
    r"^(directed|checked|continuous)_(?P<config>.+)_(?P<pattern>" + "|".join(_PATTERNS) +
    r")(?P<narrow>_narrow)?_(?:r(?P<rate>[\d.]+)_)?s(?P<seed>\d+)$")
_CONFIG = re.compile(
    r"\[Config\]\s+max_unique_ids=(\d+)\s+max_outstanding=(\d+)\s+dat_num_vc=(\d+)"
    r"(?:\s+router_vc_depth=(\d+))?(?:\s+mst_stall_random=(\d+))?")
_PASS = re.compile(r"PASS: all (\d+) nodes done, non-vacuous")

_PARAM_COLS = ("topology", "vc", "router_depth", "outstanding", "txns_per_id",
               "ids/init", "burst_len", "mode", "rate", "txns/node", "mst_stall")


def emit_table(header, rows):
    widths = [max(len(h), *(len(r[i]) for r in rows)) for i, h in enumerate(header)]
    line = lambda cells: "| " + " | ".join(c.ljust(w) for c, w in zip(cells, widths)) + " |"
    print(line(header))
    print("|" + "|".join("-" * (w + 2) for w in widths) + "|")
    for r in rows:
        print(line(r))
    print()


def dat_link_util(perf_path):
    """(mean, max, min) DAT inter-router link utilization from the run's
    perf.json: flit_count / window cycles per link, 1 flit per cycle being the
    link's capacity. None when the run carries no perf.json."""
    if not perf_path.exists():
        return None
    perf = json.loads(perf_path.read_text())
    cyc = perf["window"]["end_cyc"] - perf["window"]["start_cyc"]
    utils = [l["flit_count"] / cyc for l in perf["noc"]["links"]
             if l["name"].startswith("dat_")]
    if not utils or cyc <= 0:
        return None
    return (sum(utils) / len(utils), max(utils), min(utils))


def collect(out_root):
    """{param_tuple: {pattern: [run dict]}}, param_tuple ordered as _PARAM_COLS."""
    groups = defaultdict(lambda: defaultdict(list))
    for run_dir in sorted(p for p in out_root.iterdir() if p.is_dir()):
        if run_dir.name.startswith("continuous_"):
            csv_path = run_dir / "result.csv"
            if not csv_path.exists():
                # Aborted before the monitors reported: no CSV, so only the
                # parameters in the tag and the [Config] line are known.
                m = _TAG.match(run_dir.name)
                log_path = run_dir / "run.log"
                if not m or not log_path.exists():
                    continue
                cfg = _CONFIG.search(log_path.read_text())
                key = (m.group("config"), cfg.group(3), cfg.group(4) or "?",
                       cfg.group(2), "?", "?", "?", "continuous",
                       m.group("rate") or "?", "?", cfg.group(5) or "?")
                groups[key][m.group("pattern")].append({
                    "status": "FAIL",
                    "space": "config" if m.group("narrow") else "memory"})
                continue
            row = next(csv.DictReader(csv_path.open()))
            mode = {"0": "directed", "1": "continuous", "2": "checked"}[
                row["injection_mode"]]
            dat_util = dat_link_util(run_dir / "perf.json")
            key = (row["topology"], row["vc"],
                   row.get("router_vc_depth") or "?",
                   row["max_outstanding"], row.get("max_txns_per_id", "32"),
                   row.get("ids_per_initiator", "1"), row.get("burst_len", "0"),
                   mode, row["injection_rate"], row["injection_count"],
                   row.get("mst_stall_random") or "-")
            groups[key][row["pattern"]].append({
                "status": "PASS (completed, no data check)",
                "space": row.get("space", "memory"),
                "bw": float(row["accepted_bits_per_cycle"]),
                "latency": float(row["mean_latency"]),
                "dat_util": dat_util,
            })
        else:
            m = _TAG.match(run_dir.name)
            log_path = run_dir / "run.log"
            if not m or not log_path.exists():
                continue
            text = log_path.read_text()
            cfg = _CONFIG.search(text)
            key = (m.group("config"), cfg.group(3), cfg.group(4) or "?",
                   cfg.group(2), "-", "-", "-", m.group(1),
                   m.group("rate") or "-", "-", cfg.group(5) or "-")
            groups[key][m.group("pattern")].append({
                "status": "PASS (scoreboard)" if _PASS.search(text) else "FAIL",
                "space": "config" if m.group("narrow") else "memory",
            })
    return groups


# Shipped defaults, mirroring specgen/source/constants.yaml (DAT_NUM_VC,
# NSU_META_BUFFER_*, NMU_MAX_TXNS_PER_ID) and the generator defaults.
_DEFAULTS = {"vc": "2", "router_depth": "8", "outstanding": "32",
             "txns_per_id": "32", "ids/init": "1", "burst_len": "0"}


def group_label(key):
    """Name a group by how it differs from the shipped defaults.
    '?' fields (aborted run, value unrecorded) are excluded from the diff."""
    diffs = [f"{col}={val}" for col, val in zip(_PARAM_COLS, key)
             if col in _DEFAULTS and val not in ("?", "-")
             and val != _DEFAULTS[col]]
    if key[7] != "continuous":
        diffs.append(f"mode={key[7]}")
    if any(v == "?" for v in key):
        diffs.append("aborted before reporting, unrecorded fields ?")
    return ", ".join(diffs) if diffs else "default"


def main():
    out_root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                            "sim/verilator/output")
    groups = collect(out_root)
    labeled = sorted(
        ((group_label(k), k) for k in groups),
        key=lambda lk: (lk[1][7] != "continuous", "?" in lk[1],
                        lk[0] != "default", lk[0]))

    print("# Continuous-mode parameter sweep\n")
    print("BW = accepted bits/cycle summed over every node monitor. "
          "Latency = sample-weighted mean over read and write transactions. "
          "One seed per cell. PASS (completed) means protocol checks, model "
          "invariants and the watchdog stayed clean; the write-readback "
          "scoreboard is armed only in directed and checked modes.\n\n"
          "data = memory space, AxSIZE 5 (32 B/beat), DAT network. "
          "narrow = config space, AxSIZE 3 (the 8 B lane), REQ/RSP network. "
          "Same pattern, parameters and seed policy per row; bits/cycle is "
          "not comparable across classes (lane widths differ by design), "
          "latency is.\n")
    for i, (label, key) in enumerate(labeled, 1):
        patterns = groups[key]
        print(f"## Set {i}: {label}\n")
        emit_table(_PARAM_COLS, [key])
        rows = []
        has_narrow = any(r["space"] == "config"
                         for runs in patterns.values() for r in runs)
        has_util = any(r.get("dat_util")
                       for runs in patterns.values() for r in runs)
        for pattern in sorted(patterns):
            runs = patterns[pattern]
            data = [r for r in runs if r["space"] != "config"]
            narrow = [r for r in runs if r["space"] == "config"]

            def mean(rs, field):
                ok = [r[field] for r in rs if field in r]
                return sum(ok) / len(ok) if ok else None

            flags = [f"{name} FAIL" for name, rs in (("data", data),
                                                     ("narrow", narrow))
                     if any(r["status"] == "FAIL" for r in rs)]
            status = ", ".join(flags) if flags else runs[0]["status"]
            dbw, dlat = mean(data, "bw"), mean(data, "latency")
            nlat = mean(narrow, "latency")
            delta = f"{nlat - dlat:+.1f}" if dlat is not None and \
                nlat is not None else "-"
            fmt = lambda v: f"{v:.1f}" if v is not None else "-"
            row = [pattern, status, fmt(dbw), fmt(dlat)]
            if has_narrow:
                row += [fmt(nlat), delta]
            if has_util:
                utils = [r["dat_util"] for r in data if r.get("dat_util")]
                if utils:
                    # mean of per-run means; extremes across runs
                    row += [f"{100 * sum(u[0] for u in utils) / len(utils):.1f}%",
                            f"{100 * max(u[1] for u in utils):.1f}%",
                            f"{100 * min(u[2] for u in utils):.1f}%"]
                else:
                    row += ["-", "-", "-"]
            rows.append(row)
        header = ["pattern", "status", "BW (bits/cyc)", "data lat (cyc)"]
        if has_narrow:
            header += ["narrow lat (cyc)", "narrow-data (cyc)"]
        if has_util:
            header += ["DAT util mean (%)", "max (%)", "min (%)"]
        emit_table(header, rows)


if __name__ == "__main__":
    main()
