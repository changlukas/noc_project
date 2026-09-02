#!/usr/bin/env python3
"""Render an AI-inference NoC performance report from simulation results."""

import argparse
import csv
import html
import json
import pathlib
import sys


AI_WRITE_ONLY = frozenset({
    "broadcast", "gather", "alltoall", "neighbor_exchange", "pipeline",
    "many_to_many",
})

DISPLAY_NAME = {
    "broadcast": "Broadcast / Multicast",
    "gather": "Gather",
    "alltoall": "AlltoAll",
    "neighbor_exchange": "Neighbor Exchange",
    "pipeline": "Pipeline P2P",
    "many_to_many": "Regional Exchange",
}

MAPPING_SUFFIX = {
    "broadcast_row": "Row",
    "broadcast_col": "Column",
    "broadcast_submesh": "Local 2x2",
    "broadcast_global": "Global",
    "gather_global_root0": "Global, root 0",
    "gather_submesh": "Local 2x2",
}

EXPECTED_MAPPINGS = (
    "broadcast_row", "broadcast_col", "broadcast_submesh",
    "broadcast_global", "gather_global_root0", "gather_submesh",
    "alltoall", "neighbor_exchange", "pipeline", "many_to_many",
)

USEFUL_RATIO = {pattern: 1.0 for pattern in AI_WRITE_ONLY}
USEFUL_RATIO["broadcast"] = 0.5


def mapping_label(mapping):
    pattern = mapping if mapping in DISPLAY_NAME else mapping.split("_", 1)[0]
    base = DISPLAY_NAME.get(pattern)
    if base is None:
        raise ValueError(f"unknown AI traffic mapping: {mapping}")
    suffix = MAPPING_SUFFIX.get(mapping)
    return f"{base} — {suffix}" if suffix else base


def _one_csv(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        sys.exit(f"perf_report: expected one result row in {path}")
    return rows[0]


def _integer(row, field, path):
    try:
        value = int(row[field])
    except (KeyError, ValueError):
        sys.exit(f"perf_report: invalid {field} in {path}")
    if value <= 0:
        sys.exit(f"perf_report: {field} must be positive in {path}")
    return value


def _number(row, field, path):
    try:
        return float(row[field])
    except (KeyError, ValueError):
        sys.exit(f"perf_report: invalid {field} in {path}")


def _dat_link_utils(path):
    if not path.exists():
        sys.exit(f"perf_report: missing {path}")
    payload = json.loads(path.read_text(encoding="utf-8"))
    cycles = payload["window"]["end_cyc"] - payload["window"]["start_cyc"]
    if cycles <= 0:
        sys.exit(f"perf_report: invalid measurement window in {path}")
    return [link["flit_count"] / cycles for link in payload["noc"]["links"]
            if link["name"].startswith("dat_")]


def collect(out_root):
    out_root = pathlib.Path(out_root)
    continuous = []
    rounds = []
    settings = set()
    offered_by_mapping = {}

    for run_dir in sorted(path for path in out_root.iterdir() if path.is_dir()):
        csv_path = run_dir / "result.csv"
        if not csv_path.exists():
            continue
        row = _one_csv(csv_path)
        pattern = row.get("pattern")
        mapping = row.get("traffic_mapping")
        if pattern not in AI_WRITE_ONLY or mapping not in EXPECTED_MAPPINGS:
            continue
        settings.add((row.get("topology"), row.get("stim_size"),
                      row.get("burst_len"), row.get("seed")))
        active = _integer(row, "active_sources", csv_path)
        base = {"pattern": pattern, "mapping": mapping,
                "active_sources": active}
        if row.get("round_completion_cycles"):
            source_writes = _integer(row, "round_write_bursts", csv_path)
            if _integer(row, "round_active_sources", csv_path) != active:
                sys.exit(f"perf_report: round active-source mismatch in {csv_path}")
            rounds.append({
                **base,
                "source_writes": source_writes,
                "destination_deliveries": _integer(
                    row, "destination_deliveries", csv_path),
                "completion_cycles": _integer(
                    row, "round_completion_cycles", csv_path),
            })
            continue
        offered = _number(row, "offered_load_per_active_source", csv_path)
        delivered = _number(row, "delivered_payload_bytes_per_cycle", csv_path)
        link_utils = _dat_link_utils(run_dir / "perf.json")
        if not link_utils:
            sys.exit(f"perf_report: no DAT links in {run_dir / 'perf.json'}")
        continuous.append({
            **base,
            "destination_deliveries": _integer(
                row, "destination_deliveries", csv_path),
            "offered": offered,
            "offered_mesh_avg": _number(row, "offered_load_mesh_avg", csv_path),
            "accepted_injection": _number(
                row, "accepted_injection_load_mesh_avg", csv_path),
            "completion_latency": _number(
                row, "mean_latency_open_write", csv_path),
            "delivered_bandwidth": delivered,
            "useful_bandwidth": delivered * USEFUL_RATIO[pattern],
            "dat_link_utils": link_utils,
            "dat_link_utilization": max(link_utils) * 100,
        })
        offered_by_mapping.setdefault(mapping, set()).add(round(offered, 9))

    if not continuous:
        sys.exit(f"perf_report: no AI continuous results under {out_root}")
    if len(settings) != 1:
        sys.exit("perf_report: topology, transfer geometry, and seed must match")
    load_sets = list(offered_by_mapping.values())
    if (set(offered_by_mapping) != set(EXPECTED_MAPPINGS) or
            any(loads != load_sets[0] for loads in load_sets[1:])):
        sys.exit("perf_report: every AI mapping must use the same offered-load points")
    return {"continuous": continuous, "rounds": rounds,
            "settings": next(iter(settings))}


def _table(headers, rows):
    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join("---" if index == 0 else "---:"
                         for index in range(len(headers))) + "|",
    ]
    lines.extend("| " + " | ".join(str(value) for value in row) + " |"
                 for row in rows)
    return "\n".join(lines)


def _require_complete(results):
    for key in ("continuous", "rounds"):
        found = {row["mapping"] for row in results[key]}
        if found != set(EXPECTED_MAPPINGS):
            sys.exit(f"perf_report: incomplete AI {key} results")


def _by_mapping(rows):
    return {mapping: [row for row in rows if row["mapping"] == mapping]
            for mapping in EXPECTED_MAPPINGS}


def _continuous_round_count(curves, round_by_mapping):
    counts = set()
    for mapping in EXPECTED_MAPPINGS:
        deliveries = {row["destination_deliveries"] for row in curves[mapping]}
        one_round = round_by_mapping[mapping]["destination_deliveries"]
        if len(deliveries) != 1:
            sys.exit("perf_report: continuous round count changed across load points")
        total = deliveries.pop()
        if total % one_round:
            sys.exit("perf_report: continuous deliveries are not whole rounds")
        counts.add(total // one_round)
    if len(counts) != 1:
        sys.exit("perf_report: continuous round count must match across mappings")
    return counts.pop()


def report(out_root):
    results = collect(out_root)
    _require_complete(results)
    topology, _stim_size, burst_len, seed = results["settings"]
    beats = int(burst_len) + 1
    flits = beats + 1
    curves = _by_mapping(results["continuous"])
    round_by_mapping = {row["mapping"]: row for row in results["rounds"]}
    continuous_rounds = _continuous_round_count(curves, round_by_mapping)

    communication_rows = []
    for mapping in EXPECTED_MAPPINGS:
        row = curves[mapping][0]
        communication_rows.append([
            mapping_label(mapping), row["active_sources"],
            "50% GEMM payload" if row["pattern"] == "broadcast" else "100% payload",
        ])

    metric_rows = []
    for mapping in EXPECTED_MAPPINGS:
        points = sorted(curves[mapping], key=lambda point: point["offered"])
        low = points[0]
        peak = max(points, key=lambda point: point["useful_bandwidth"])
        metric_rows.append([
            mapping_label(mapping), low["active_sources"],
            f"{low['completion_latency']:.1f}",
            f"{peak['useful_bandwidth']:.1f}", f"{peak['offered']:.3f}",
            f"{peak['dat_link_utilization']:.1f}",
        ])

    round_rows = []
    for mapping in EXPECTED_MAPPINGS:
        row = round_by_mapping[mapping]
        round_rows.append([
            mapping_label(mapping), row["active_sources"], row["source_writes"],
            row["destination_deliveries"], row["completion_cycles"],
        ])

    return "\n".join([
        f"# AI Inference NoC Performance Report — {topology}",
        "",
        "## 1. Test Setup and Measurement",
        "",
        f"所有結果固定使用 seed {seed}、4 KB AXI write（{beats} beats，burst length {burst_len}）。Continuous sweep 使用 {continuous_rounds} rounds 與相同 offered-load sweep。",
        "",
        "1. 先指定每個 active source 的 Offered load。",
        f"2. Offered load per active source = Injection rate × {flits} DAT flits",
        "3. Offered load mesh average = Offered load per active source × active sources / 16",
        "4. Continuous sweep 量測每筆 transaction 的 completion latency、delivery rate 與 DAT-link loading。",
        "5. Directed round 讓所有 source 同步送出一次 mapping，再等待全部 B response。",
        "",
        "## 2. AI Communication Types",
        "",
        _table(["Communication type", "Active sources", "Useful-byte policy"], communication_rows),
        "",
        "Broadcast / Multicast 使用 50% useful-byte ratio，表示 GEMM 情境中只有送到各 destination 的目標 tensor slice 計入 useful payload；其他 mapping 的 delivered payload 全數計入。",
        "",
        "## 3. Continuous-load Results",
        "",
        _table([
            "Communication type", "Active sources",
            "Low-load completion latency (cycles/transaction)",
            "Peak useful delivered bandwidth (B/cycle)",
            "Offered load at peak (DAT flits/active source/cycle)",
            "Busiest DAT-link utilization (%)",
        ], metric_rows),
        "",
        "Low-load completion latency 越小越好。Useful delivered bandwidth (B/cycle) 越大越好。DAT-link utilization (%) 顯示同一 peak-bandwidth 量測點的最忙 link，越接近 100% 越可能成為 bottleneck。三者必須看同一列：先看 latency，再看能送達多少 useful tensor data，最後確認是否由單一 link 限制。",
        "",
        "![Completion latency](perf_ai_latency.svg)",
        "",
        "![Useful delivered bandwidth](perf_ai_bandwidth.svg)",
        "",
        "![DAT-link utilization](perf_ai_link_utilization.svg)",
        "",
        "## 4. Synchronized Directed Round",
        "",
        "AXI write round completion time (cycles/round) 的邊界是 common issue start through the final expected B response。它包含 source issue、NI、NoC、destination handling 與 B return，不是 NoC-only latency，也沒有 background traffic。",
        "",
        _table([
            "Communication type", "Active sources", "Source write bursts",
            "Destination deliveries", "Completion time (cycles/round)",
        ], round_rows),
        "",
        "Source write bursts 是注入的 AXI writes；Destination deliveries 會把 Broadcast fanout 展開。Completion time 用來比較完整 communication round 完成速度，不能直接與每筆 transaction latency 相減。",
        "",
        "![AXI write round completion time](perf_ai_round_completion.svg)",
        "",
    ])


_COLORS = ("#1a73e8", "#d93025", "#188038", "#a142f4", "#f29900",
           "#0097a7", "#5f6368", "#c2185b", "#7cb342", "#3949ab")


def _line_svg(series, title, x_label, y_label, path):
    width, height = 1100, 650
    left, right, top, bottom = 90, 25, 115, 70
    all_points = [point for _label, points in series for point in points]
    xs = [point[0] for point in all_points]
    ys = [point[1] for point in all_points]
    x_span = max(xs) - min(xs) or 1
    y_span = max(ys) - min(ys) or 1
    sx = lambda x: left + (x - min(xs)) / x_span * (width - left - right)
    sy = lambda y: top + (max(ys) - y) / y_span * (height - top - bottom)
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">',
        '<style>text{font-family:Arial,sans-serif;fill:#202124}.title{font-size:22px;font-weight:700}.label{font-size:17px}.legend{font-size:13px}</style>',
        f'<text class="title" x="20" y="28">{html.escape(title)}</text>',
    ]
    for index, (label, points) in enumerate(series):
        color = _COLORS[index]
        lx = 20 + (index % 5) * 215
        ly = 55 + (index // 5) * 24
        lines.append(f'<line x1="{lx}" y1="{ly}" x2="{lx + 18}" y2="{ly}" stroke="{color}" stroke-width="3"/>')
        lines.append(f'<text class="legend" x="{lx + 24}" y="{ly + 4}">{html.escape(label)}</text>')
        coords = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in points)
        lines.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="2"/>')
        lines.extend(f'<circle cx="{sx(x):.1f}" cy="{sy(y):.1f}" r="3" fill="{color}"/>'
                     for x, y in points)
    lines.append(f'<text class="label" x="{(left + width - right) / 2:.1f}" y="{height - 12}" text-anchor="middle">{html.escape(x_label)}</text>')
    lines.append(f'<text class="label" transform="translate(18 {(top + height - bottom) / 2:.1f}) rotate(-90)" text-anchor="middle">{html.escape(y_label)}</text>')
    lines.append('</svg>')
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def _bar_svg(rows, title, unit, path):
    width, height = 1100, 600
    left, right, top, bottom = 250, 50, 60, 45
    plot_width = width - left - right
    maximum = max(value for _label, value in rows) or 1
    row_height = (height - top - bottom) / len(rows)
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">',
        '<style>text{font-family:Arial,sans-serif;fill:#202124}.title{font-size:22px;font-weight:700}.label{font-size:14px}.value{font-size:13px}</style>',
        f'<text class="title" x="20" y="28">{html.escape(title)} ({html.escape(unit)})</text>',
    ]
    for index, (label, value) in enumerate(rows):
        y = top + index * row_height
        bar_width = value / maximum * plot_width
        lines.append(f'<text class="label" x="{left - 10}" y="{y + 18:.1f}" text-anchor="end">{html.escape(label)}</text>')
        lines.append(f'<rect x="{left}" y="{y + 3:.1f}" width="{bar_width:.1f}" height="20" fill="{_COLORS[index]}"/>')
        lines.append(f'<text class="value" x="{left + bar_width + 6:.1f}" y="{y + 18:.1f}">{value:.1f}</text>')
    lines.append('</svg>')
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def write_figures(results, destination):
    destination = pathlib.Path(destination)
    _require_complete(results)
    curves = _by_mapping(results["continuous"])
    latency = destination / "perf_ai_latency.svg"
    bandwidth = destination / "perf_ai_bandwidth.svg"
    links = destination / "perf_ai_link_utilization.svg"
    rounds = destination / "perf_ai_round_completion.svg"
    _line_svg([
        (mapping_label(mapping), [(point["offered"], point["completion_latency"])
                                  for point in sorted(curves[mapping], key=lambda p: p["offered"])])
        for mapping in EXPECTED_MAPPINGS
    ], "Completion Latency", "Offered load (DAT flits/active source/cycle)",
       "Completion latency (cycles/transaction)", latency)
    _line_svg([
        (mapping_label(mapping), [(point["offered"], point["useful_bandwidth"])
                                  for point in sorted(curves[mapping], key=lambda p: p["offered"])])
        for mapping in EXPECTED_MAPPINGS
    ], "Useful Delivered Bandwidth", "Offered load (DAT flits/active source/cycle)",
       "Useful delivered bandwidth (B/cycle)", bandwidth)
    peak_rows = []
    for mapping in EXPECTED_MAPPINGS:
        peak = max(curves[mapping], key=lambda point: point["useful_bandwidth"])
        peak_rows.append((mapping_label(mapping), peak["dat_link_utilization"]))
    _bar_svg(peak_rows, "Busiest DAT-link Utilization at Peak Bandwidth", "%", links)
    round_by_mapping = {row["mapping"]: row for row in results["rounds"]}
    _bar_svg([(mapping_label(mapping), round_by_mapping[mapping]["completion_cycles"])
              for mapping in EXPECTED_MAPPINGS],
             "AXI Write Round Completion Time", "cycles/round", rounds)
    return [latency, bandwidth, links, rounds]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_dir", nargs="?", default="sim/verilator/output")
    parser.add_argument("-o", "--out")
    args = parser.parse_args(argv)
    out_root = pathlib.Path(args.out_dir)
    results = collect(out_root)
    _require_complete(results)
    destination = pathlib.Path(args.out) if args.out else out_root / "perf_report.md"
    write_figures(results, destination.parent)
    text = report(out_root)
    destination.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {destination} ({len(text.splitlines())} lines)")


if __name__ == "__main__":
    main(sys.argv[1:])
