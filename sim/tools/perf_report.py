"""Validate NoC measurement data and export explicit DMA records as JSON.

The authored report is maintained in docs/perf_report_restructured.md.
Legacy counter readers remain available for existing measurement validation.
"""

import argparse
import csv
import html
import json
import math
import pathlib
import os
import re
import shlex
import sys

import pattern_metrics as pm
from ai_scenario_figures import write_scenario_figures
from lifecycle_routes import write_figures as write_lifecycle_figures
from host_control_preview import write_figures as write_host_figures
from dense_traffic_preview import write_figure as write_dense_figure
from ai_mapping_preview import write_figures as write_mapping_figures
from ai_mapping_preview import PLACEMENTS


WRITE_MAPPINGS = (
    "broadcast_row", "broadcast_col", "broadcast_submesh", "broadcast_global",
    "gather_global_root0", "gather_submesh", "alltoall",
    "neighbor_exchange", "pipeline", "many_to_many",
)
READ_MAPPINGS = (
    "gather_global_root0", "gather_submesh", "alltoall",
    "neighbor_exchange", "pipeline", "many_to_many",
)
TRADEOFF_CELLS = (
    ("write", "broadcast_global"),
    ("write", "many_to_many"),
    ("read", "many_to_many"),
)
BURST_BEATS = (1, 4, 16, 64)
BURST_MAPPINGS = ("broadcast_global", "gather_global_root0", "alltoall", "pipeline")
REPORT_BURST_MAPPINGS = ("broadcast_global", "alltoall", "pipeline")
NI_TX_DAT_DEPTH = 8

REPORT_WRITE_MAPPINGS = (
    "broadcast_row", "broadcast_col", "broadcast_submesh", "broadcast_global",
    "alltoall", "pipeline", "many_to_many",
)
REPORT_READ_MAPPINGS = ("alltoall", "pipeline", "many_to_many")
GALLERY_MAPPINGS = REPORT_WRITE_MAPPINGS

DISPLAY_NAME = {
    "broadcast_row": "Row-wise Multicast",
    "broadcast_col": "Column-wise Multicast",
    "broadcast_submesh": "Local Multicast (2×2)",
    "broadcast_global": "Global Multicast",
    "gather_global_root0": "Global Gather (root 0)",
    "gather_submesh": "Local Gather (2x2)",
    "alltoall": "All-to-All",
    "neighbor_exchange": "Neighbor Exchange",
    "pipeline": "Pipeline P2P",
    "many_to_many": "Cyclic Inter-region Many-to-Many",
}

def _one_csv(path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        sys.exit(f"perf_report: expected one result row in {path}")
    return rows[0]


def _positive_int(row, field, path):
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


def _nonnegative_int(row, field, path):
    try:
        value = int(row.get(field, "0"))
    except ValueError:
        sys.exit(f"perf_report: invalid {field} in {path}")
    if value < 0:
        sys.exit(f"perf_report: {field} must be non-negative in {path}")
    return value


def _expected_workload_counts(mapping, rounds, multicast_mode):
    payload_edges = pm._ai_payload_edges(mapping, rounds)
    deliveries = sum(map(len, payload_edges.values()))
    requests = deliveries
    if mapping.startswith("broadcast_") and multicast_mode == "hardware":
        requests = len(payload_edges) * rounds
    return requests, deliveries


def _expected_destinations_per_source(mapping):
    fanouts = {len(destinations) for destinations in
               pm._ai_payload_edges(mapping, 1).values() if destinations}
    if len(fanouts) != 1:
        sys.exit(f"perf_report: inconsistent Broadcast fanout for {mapping}")
    return fanouts.pop()


def _validate_resource_counters(
        perf_path, mapping, direction, burst_beats, rounds, multicast_mode):
    try:
        links = json.loads(perf_path.read_text(encoding="utf-8"))["noc"]["links"]
        emitted_resources = {
            link["name"]: int(link["flit_count"])
            for link in links if int(link["flit_count"]) > 0
        }
    except (FileNotFoundError, KeyError, TypeError, ValueError, json.JSONDecodeError):
        sys.exit(f"perf_report: invalid resource counters in {perf_path}")
    expected_resources = pm.ai_resource_flits(
        mapping, direction, burst_beats, rounds, multicast_mode)
    if emitted_resources != expected_resources:
        sys.exit(f"perf_report: resource flit counts do not match in {perf_path}")


def collect_rows(root):
    root = pathlib.Path(root)
    rows = []
    seen = set()
    if not root.exists():
        return rows
    for path in sorted(root.rglob("result.csv")):
        raw = _one_csv(path)
        if raw.get("measurement_mode") != "outstanding":
            continue
        direction = raw.get("direction")
        mapping = raw.get("traffic_mapping")
        allowed = WRITE_MAPPINGS if direction == "write" else READ_MAPPINGS if direction == "read" else ()
        if mapping not in allowed:
            sys.exit(f"perf_report: invalid {direction} mapping in {path}")
        if (raw.get("topology") != "mesh_4x4" or raw.get("checker_status") != "PASS" or
                raw.get("burst_beats") != "64" or raw.get("transaction_bytes") != "4096" or
                raw.get("rounds") != "16"):
            sys.exit(f"perf_report: invalid geometry or checker evidence in {path}")
        outstanding = _positive_int(raw, "source_outstanding_depth", path)
        hwm = _positive_int(raw, "source_outstanding_hwm", path)
        max_txns_per_id = _positive_int(raw, "max_txns_per_id", path)
        if outstanding != 32 or max_txns_per_id != 32 or hwm > outstanding:
            sys.exit(f"perf_report: invalid Outstanding evidence in {path}")
        burst_beats = _positive_int(raw, "burst_beats", path)
        rounds = _positive_int(raw, "rounds", path)
        source_requests = _positive_int(raw, "source_requests", path)
        payload_deliveries = _positive_int(raw, "payload_deliveries", path)
        multicast_mode = (raw.get("multicast_mode") or
                          ("hardware" if mapping.startswith("broadcast_") else None))
        expected_requests, expected_deliveries = _expected_workload_counts(
            mapping, rounds, multicast_mode)
        if payload_deliveries != expected_deliveries:
            sys.exit(f"perf_report: invalid payload delivery count in {path}")
        if source_requests != expected_requests:
            sys.exit(f"perf_report: invalid source request count in {path}")
        perf_path = path.with_name("perf.json")
        _validate_resource_counters(
            perf_path, mapping, direction, burst_beats, rounds, multicast_mode)
        key = (direction, mapping, _positive_int(raw, "vc", path),
               _positive_int(raw, "router_vc_depth", path), outstanding,
               _positive_int(raw, "seed", path))
        if key in seen:
            sys.exit(f"perf_report: duplicate result in {path}")
        seen.add(key)
        rows.append({
            "path": path, "direction": direction, "mapping": mapping,
            "vc": key[2], "router_depth": key[3], "outstanding": outstanding,
            "ni_rx_depth": _positive_int(raw, "ni_dat_rx_vc_depth", path),
            "outstanding_hwm": hwm, "seed": key[5],
            "r_rob_depth": _positive_int(raw, "r_rob_depth", path),
            "max_txns_per_id": max_txns_per_id,
            "read_slot_hwm": _nonnegative_int(
                raw, "nmu_read_slot_hwm_beats", path),
            "order_list_hwm": _nonnegative_int(
                raw, "nmu_order_list_hwm_transactions", path),
            "data_producers": _positive_int(raw, "data_producers", path),
            "consumers": _positive_int(raw, "consumers", path),
            "initiators": _positive_int(raw, "axi_initiators", path),
            "burst_beats": burst_beats, "rounds": rounds,
            "source_requests": source_requests,
            "payload_deliveries": payload_deliveries,
            "multicast_mode": multicast_mode,
            "completion_cycles": _positive_int(raw, "completion_cycles", path),
            "latency": _number(raw, "completion_latency_cycles", path),
            "bandwidth": _number(raw, "delivered_payload_bytes_per_cycle", path),
            "busiest_link": _number(raw, "busiest_dat_link_utilization_pct", path),
            "high_load_links": _positive_int(raw, "dat_links_ge_75_pct", path)
            if raw.get("dat_links_ge_75_pct") != "0" else 0,
            "buffer_entries": key[2] * key[3],
        })
    return rows


def _collect_characterization_rows(root, experiment):
    root = pathlib.Path(root)
    rows = []
    seen = set()
    if not root.exists():
        return rows
    for path in sorted(root.rglob("result.csv")):
        raw = _one_csv(path)
        if raw.get("measurement_mode") != "outstanding":
            continue
        mapping = raw.get("traffic_mapping")
        direction = raw.get("direction")
        mode = raw.get("multicast_mode") or None
        burst_beats = _positive_int(raw, "burst_beats", path)
        transactions = _positive_int(raw, "transactions_per_flow", path)
        rounds = _positive_int(raw, "rounds", path)
        bytes_per_beat = _positive_int(raw, "bytes_per_beat", path)
        bytes_per_flow = _positive_int(raw, "bytes_per_flow_round", path)
        if (raw.get("topology") != "mesh_4x4" or raw.get("checker_status") != "PASS" or
                _positive_int(raw, "source_outstanding_depth", path) != 32 or
                _positive_int(raw, "max_txns_per_id", path) != 32 or
                _positive_int(raw, "vc", path) != 2 or
                _positive_int(raw, "router_vc_depth", path) != 8 or
                _positive_int(raw, "ni_dat_rx_vc_depth", path) != 8 or
                _positive_int(raw, "r_rob_depth", path) != 128):
            sys.exit(f"perf_report: {experiment} must use the baseline DUT at fixed O32 in {path}")
        if (bytes_per_beat != 64 or bytes_per_flow != 4096 or
                burst_beats * transactions * bytes_per_beat != 4096):
            sys.exit(f"perf_report: {experiment} requires 4096 B/flow/round in {path}")
        if experiment == "burst":
            if (mapping not in BURST_MAPPINGS or
                    direction not in (("write",) if mapping.startswith("broadcast_")
                                      else ("write", "read")) or
                    burst_beats not in BURST_BEATS or transactions != 64 // burst_beats or
                    mode not in (None, "hardware")):
                sys.exit(f"perf_report: invalid Burst characterization row in {path}")
        elif (mapping not in WRITE_MAPPINGS[:4] or direction != "write" or
              burst_beats != 64 or transactions != 1 or
              mode not in ("hardware", "repeated_unicast")):
            sys.exit(f"perf_report: invalid multicast comparison row in {path}")
        source_requests = _positive_int(raw, "source_requests", path)
        deliveries = _positive_int(raw, "payload_deliveries", path)
        expected_requests, expected_deliveries = _expected_workload_counts(
            mapping, rounds * transactions, mode)
        if (source_requests, deliveries) != (expected_requests, expected_deliveries):
            sys.exit(f"perf_report: generated traffic metadata disagrees in {path}")
        destinations_per_source = None
        if experiment == "multicast":
            destinations_per_source = _positive_int(
                raw, "destinations_per_source", path)
            if destinations_per_source != _expected_destinations_per_source(mapping):
                sys.exit(f"perf_report: invalid destinations per source in {path}")
        _validate_resource_counters(
            path.with_name("perf.json"), mapping, direction, burst_beats,
            rounds * transactions, mode)
        source_nodes = pm._ai_payload_edges(mapping, 1)
        try:
            links = json.loads(path.with_name("perf.json").read_text(
                encoding="utf-8"))["noc"]["links"]
            link_flits = {link["name"]: int(link["flit_count"]) for link in links}
            source_injected_flits = sum(
                link_flits.get(f"dat_inject_{source}", 0)
                for source in source_nodes)
        except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
            sys.exit(f"perf_report: invalid source injection counters in {path}")
        if source_injected_flits <= 0:
            sys.exit(f"perf_report: missing source injection counters in {path}")
        key = (direction, mapping, burst_beats, mode,
               _positive_int(raw, "seed", path))
        if key in seen:
            sys.exit(f"perf_report: duplicate {experiment} result in {path}")
        seen.add(key)
        rows.append({
            "path": path, "direction": direction, "mapping": mapping,
            "burst_beats": burst_beats, "transactions_per_flow": transactions,
            "bytes_per_flow_round": bytes_per_flow, "multicast_mode": mode,
            "source_requests": source_requests, "payload_deliveries": deliveries,
            "destination_count": destinations_per_source,
            "source_injected_flits": source_injected_flits,
            "completion_cycles": _positive_int(raw, "completion_cycles", path),
            "latency": _number(raw, "completion_latency_cycles", path),
            "bandwidth": _number(raw, "delivered_payload_bytes_per_cycle", path),
            "seed": key[-1],
        })
    return rows


def collect_burst_rows(root):
    return sorted(_collect_characterization_rows(root, "burst"),
                  key=lambda row: (row["mapping"], row["direction"], row["burst_beats"]))


def collect_multicast_rows(root):
    return _collect_characterization_rows(root, "multicast")


def _require_complete(rows):
    found = {(row["direction"], row["mapping"], row["outstanding"])
             for row in rows}
    required = {
        (direction, mapping, 32)
        for direction, mappings in (("write", WRITE_MAPPINGS), ("read", READ_MAPPINGS))
        for mapping in mappings
    }
    if not required <= found:
        missing = len(required - found)
        sys.exit(f"perf_report: incomplete baseline ({missing} missing)")


def _table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |",
             "|" + "|".join("---" if index == 0 else "---:"
                               for index in range(len(headers))) + "|"]
    lines.extend("| " + " | ".join(map(str, row)) + " |" for row in rows)
    return "\n".join(lines)


def _performance_data(rows):
    data = []
    for direction, mappings in (("write", REPORT_WRITE_MAPPINGS),
                                ("read", REPORT_READ_MAPPINGS)):
        for mapping in mappings:
            item = next(row for row in rows
                        if row["direction"] == direction
                        and row["mapping"] == mapping
                        and row["outstanding"] == 32)
            ideal = pm.ideal_throughput_bound(
                mapping, direction, item["burst_beats"], item["rounds"],
                item["multicast_mode"])
            data.append({
                "mapping": mapping,
                "direction": direction,
                "ideal": ideal,
                "accepted": item["bandwidth"],
            })
    return data


def _traffic_gallery():
    lines = []
    for index in range(0, len(GALLERY_MAPPINGS), 2):
        pair = GALLERY_MAPPINGS[index:index + 2]
        left = pair[0]
        right = pair[1] if len(pair) == 2 else None
        if right is None:
            lines += [
                f"| **{DISPLAY_NAME[left]}** |",
                "|---|",
                f"| ![{DISPLAY_NAME[left]}](traffic_patterns/{left}.svg) |",
                "",
            ]
            continue
        lines += [
            f"| **{DISPLAY_NAME[left]}** | **{DISPLAY_NAME[right]}** |",
            "|---|---|",
            f"| ![{DISPLAY_NAME[left]}](traffic_patterns/{left}.svg) | "
            f"![{DISPLAY_NAME[right]}](traffic_patterns/{right}.svg) |",
            "",
        ]
    return "\n".join(lines).rstrip()


def _traffic_routes(mapping):
    rows = [[4 * y + x for x in range(4)] for y in range(4)]
    if mapping == "broadcast_row":
        return [(row[x], row[x + 1], False) for row in rows for x in range(3)]
    if mapping == "broadcast_col":
        return [(4 * y + x, 4 * (y + 1) + x, False)
                for x in range(4) for y in range(3)]
    if mapping in ("broadcast_submesh", "gather_submesh"):
        if mapping == "gather_submesh":
            edges = (
                (0, 1), (1, 5), (4, 5),
                (3, 2), (2, 6), (7, 6),
                (8, 9), (12, 13), (13, 9),
                (11, 10), (15, 14), (14, 10),
            )
            return [(src, dst, False) for src, dst in edges]
        edges = []
        for base in (0, 2, 8, 10):
            edges += [(base, base + 1), (base, base + 4),
                      (base + 4, base + 5)]
        return [(src, dst, False) for src, dst in edges]
    tree = ([(0, 1), (1, 2), (2, 3)] +
            [(x + 4 * y, x + 4 * (y + 1))
             for x in range(4) for y in range(3)])
    if mapping == "broadcast_global":
        return [(src, dst, False) for src, dst in tree]
    if mapping == "gather_global_root0":
        return [(dst, src, False) for src, dst in tree]
    if mapping == "alltoall":
        return [(0, 15, True), (3, 12, True), (5, 10, True), (10, 5, True)]
    if mapping == "neighbor_exchange":
        return [(node, node + 1, True) for row in rows for node in row[:-1]] + [
            (node, node + 4, True) for node in range(12)]
    if mapping == "pipeline":
        order = (0, 1, 2, 3, 7, 6, 5, 4, 8, 9, 10, 11, 15, 14, 13, 12)
        return [(src, dst, False) for src, dst in zip(order, order[1:])]
    return []


def _traffic_roles(mapping):
    if mapping == "broadcast_row":
        return {0, 4, 8, 12}, set(range(16)) - {0, 4, 8, 12}
    if mapping == "broadcast_col":
        return {0, 1, 2, 3}, set(range(4, 16))
    if mapping == "broadcast_submesh":
        sources = {0, 2, 8, 10}
        return sources, set(range(16)) - sources
    if mapping == "broadcast_global":
        return {0}, set(range(1, 16))
    if mapping == "gather_global_root0":
        return set(range(1, 16)), {0}
    if mapping == "gather_submesh":
        roots = {5, 6, 9, 10}
        return set(range(16)) - roots, roots
    if mapping == "pipeline":
        return set(range(16)) - {12}, set(range(16)) - {0}
    return set(range(16)), set(range(16))


def _traffic_svg(mapping):
    width, height = 700, 520
    node_x = lambda node: 145 + (node % 4) * 120
    node_y = lambda node: 400 - (node // 4) * 85
    sources, destinations = _traffic_roles(mapping)
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<title>' + html.escape(DISPLAY_NAME[mapping]) + ' on a 4 x 4 mesh</title>',
        '<defs><marker id="arrow" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 Z" fill="#b23a3a"/></marker><marker id="arrow-start" markerWidth="8" markerHeight="8" refX="1" refY="4" orient="auto"><path d="M8,0 L0,4 L8,8 Z" fill="#b23a3a"/></marker></defs>',
        '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#1f2933}.title{font-size:24px;font-weight:700}.subtitle{font-size:15px;fill:#52606d}.node{stroke:#334e68;stroke-width:1.6}.node-id{font-size:17px;font-weight:700;text-anchor:middle;dominant-baseline:middle}.mesh{stroke:#cbd5e1;stroke-width:4}.flow{stroke:#b23a3a;stroke-width:4;fill:none;marker-end:url(#arrow)}.both{marker-start:url(#arrow-start)}.region{stroke-width:2;stroke-dasharray:7 5;fill-opacity:.12}.legend{font-size:14px}</style>',
        f'<text class="title" x="35" y="38">{html.escape("Cyclic inter-region exchange" if mapping == "many_to_many" else DISPLAY_NAME[mapping])}</text>',
        '<text class="subtitle" x="35" y="65">4 x 4 Mesh: (0,0) at bottom left</text>',
    ]
    for coordinate in range(4):
        lines.append(f'<text class="subtitle" x="{node_x(coordinate)}" y="442" text-anchor="middle">x={coordinate}</text>')
        lines.append(f'<text class="subtitle" x="73" y="{node_y(4 * coordinate) + 5}" text-anchor="end">y={coordinate}</text>')
    for y in range(4):
        for x in range(3):
            lines.append(f'<line class="mesh" x1="{node_x(4*y+x)}" y1="{node_y(4*y+x)}" x2="{node_x(4*y+x+1)}" y2="{node_y(4*y+x+1)}"/>')
    for x in range(4):
        for y in range(3):
            lines.append(f'<line class="mesh" x1="{node_x(4*y+x)}" y1="{node_y(4*y+x)}" x2="{node_x(4*(y+1)+x)}" y2="{node_y(4*(y+1)+x)}"/>')
    if mapping in ("broadcast_submesh", "gather_submesh", "many_to_many"):
        colors = ("#4c78a8", "#f58518", "#54a24b", "#b279a2")
        for index, base in enumerate((0, 2, 8, 10)):
            x, y = node_x(base) - 42, min(node_y(base), node_y(base + 4)) - 37
            lines.append(f'<rect class="region" x="{x}" y="{y}" width="204" height="159" rx="12" stroke="{colors[index]}" fill="{colors[index]}"/>')
    for src, dst, bidirectional in _traffic_routes(mapping):
        x1, y1, x2, y2 = node_x(src), node_y(src), node_x(dst), node_y(dst)
        dx, dy = x2 - x1, y2 - y1
        length = math.hypot(dx, dy)
        x1, y1 = x1 + 29 * dx / length, y1 + 29 * dy / length
        x2, y2 = x2 - 29 * dx / length, y2 - 29 * dy / length
        dash = ' stroke-dasharray="8 6"' if mapping == "alltoall" else ""
        cls = "flow both" if bidirectional else "flow"
        lines.append(f'<line class="{cls}" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}"{dash}/>')
    if mapping == "many_to_many":
        centers = tuple((node_x(base) + 60, node_y(base) - 42.5)
                        for base in (0, 2, 10, 8))
        for src, dst in zip(centers, centers[1:] + centers[:1]):
            lines.append(f'<line class="flow" x1="{src[0]}" y1="{src[1]}" x2="{dst[0]}" y2="{dst[1]}"/>')
        lines.append('<text class="subtitle" x="350" y="467" text-anchor="middle">Each node sends to all four nodes in the next region</text>')
    elif mapping == "alltoall":
        lines.append('<text class="subtitle" x="350" y="467" text-anchor="middle">All other nodes are destinations. Dashed arrows show examples.</text>')
    for node in range(16):
        in_source, in_destination = node in sources, node in destinations
        fill = "#8064a2" if in_source and in_destination else "#4c78a8" if in_source else "#f2a65a" if in_destination else "#ffffff"
        lines.append(f'<circle class="node" data-node="{node}" cx="{node_x(node)}" cy="{node_y(node)}" r="24" fill="{fill}"/>')
        lines.append(f'<text class="node-id" x="{node_x(node)}" y="{node_y(node)}" fill="{"#ffffff" if in_source or in_destination else "#1f2933"}">{node}</text>')
    lines += [
        '<circle cx="95" cy="497" r="9" fill="#4c78a8"/><text class="legend" x="111" y="502">Source</text>',
        '<circle cx="235" cy="497" r="9" fill="#f2a65a"/><text class="legend" x="251" y="502">Destination</text>',
        '<circle cx="405" cy="497" r="9" fill="#8064a2"/><text class="legend" x="421" y="502">Source and Destination</text>',
        '</svg>',
    ]
    return "\n".join(lines) + "\n"


def write_traffic_figures(destination):
    destination = pathlib.Path(destination) / "traffic_patterns"
    destination.mkdir(parents=True, exist_ok=True)
    paths = []
    for mapping in GALLERY_MAPPINGS:
        path = destination / f"{mapping}.svg"
        path.write_text(_traffic_svg(mapping), encoding="utf-8", newline="\n")
        paths.append(path)
    return paths


def _channel_rows(root):
    values = {}
    for path in pathlib.Path(root).rglob("result.csv"):
        raw = _one_csv(path)
        if raw.get("channel_mapping") not in ("2-channel", "3-channel"):
            continue
        edge = raw.get("shared_directed_edge", "")
        expected_control = f"req_{edge}"
        expected_background = (f"dat_{edge}"
                               if raw["channel_mapping"] == "3-channel"
                               else expected_control)
        if (raw.get("overlap_status") != "PASS" or
                not edge or
                raw.get("control_resource") != expected_control or
                raw.get("background_resource") != expected_background or
                raw.get("data_bursts_per_flow") != "32" or
                raw.get("data_beats_per_flow") != "8192"):
            sys.exit(f"perf_report: invalid RR/RRD overlap or resource evidence in {path}")
        architecture = "RR" if raw["channel_mapping"] == "2-channel" else "RRD"
        values[(architecture, raw["channel_case"])] = float(
            raw["control_mean_latency_cycles"])
    if not values:
        return []
    required = {(architecture, case) for architecture in ("RR", "RRD")
                for case in ("write", "read")}
    if set(values) != required:
        sys.exit("perf_report: incomplete RR/RRD comparison")
    rows = []
    for case in ("write", "read"):
        rr = values[("RR", case)]
        rrd = values[("RRD", case)]
        reduction = rr - rrd
        rows.append((f"Control {case.title()}", f"{rr:.2f}", f"{rrd:.2f}",
                     f"{reduction:.2f}", f"{100.0 * reduction / rr:.1f}"))
    return rows


def _burst_table(rows):
    if not rows:
        return "[TBD] Burst Length characterization has not been run."
    required = {
        (direction, mapping, beats)
        for mapping in BURST_MAPPINGS
        for direction in (("write",) if mapping.startswith("broadcast_")
                          else ("write", "read"))
        for beats in BURST_BEATS
    }
    found = {(row["direction"], row["mapping"], row["burst_beats"])
             for row in rows}
    if found != required:
        sys.exit("perf_report: incomplete Burst Length characterization")
    report_rows = [row for row in rows if row["mapping"] in REPORT_BURST_MAPPINGS]
    return _table([
        "Traffic Model", "Direction", "Burst Length (beats)",
        "Transactions/flow/round", "Accepted Throughput (B/cycle)",
        "Completion time (cycles/run)",
    ], [[DISPLAY_NAME[row["mapping"]], row["direction"].title(),
         row["burst_beats"], row["transactions_per_flow"],
         f"{row['bandwidth']:.1f}", row["completion_cycles"]]
        for row in report_rows])


def _multicast_table(rows):
    if not rows:
        return "[TBD] Hardware/repeated-unicast comparison has not been run."
    required = {(mapping, mode) for mapping in WRITE_MAPPINGS[:4]
                for mode in ("hardware", "repeated_unicast")}
    found = {(row["mapping"], row["multicast_mode"]) for row in rows}
    if found != required:
        sys.exit("perf_report: incomplete multicast implementation comparison")
    by_key = {(row["mapping"], row["multicast_mode"]): row for row in rows}
    table_rows = []
    for mapping in WRITE_MAPPINGS[:4]:
        hardware = by_key[(mapping, "hardware")]
        repeated = by_key[(mapping, "repeated_unicast")]
        if hardware["destination_count"] != repeated["destination_count"]:
            sys.exit(f"perf_report: multicast modes disagree on fanout for {mapping}")
        table_rows.append([
            DISPLAY_NAME[mapping], hardware["destination_count"],
            hardware["source_injected_flits"], hardware["completion_cycles"],
            repeated["source_injected_flits"], repeated["completion_cycles"],
            f"{repeated['completion_cycles'] / hardware['completion_cycles']:.2f}",
        ])
    return _table([
        "Destination Set", "Fanout",
        "Multicast Injected Flits", "Multicast Completion Time (cycles)",
        "Repeated Unicast Injected Flits",
        "Repeated Unicast Completion Time (cycles)", "Speedup (×)",
    ], table_rows)


def _tradeoff_summaries(baseline_rows, tradeoff_rows):
    selected = [row for row in baseline_rows + tradeoff_rows
                if (row["direction"], row["mapping"]) in TRADEOFF_CELLS
                and row["outstanding"] == 32]
    grouped = {}
    for row in selected:
        grouped.setdefault((row["vc"], row["router_depth"], row["ni_rx_depth"]), []).append(row)
    expected = len(TRADEOFF_CELLS)
    for config, rows in grouped.items():
        if len(rows) != expected:
            sys.exit(f"perf_report: incomplete trade-off configuration {config}")

    bandwidth = {}
    completion = {}
    for config, rows in grouped.items():
        for cell in TRADEOFF_CELLS:
            points = [row for row in rows
                      if (row["direction"], row["mapping"]) == cell]
            bandwidth[(config, cell)] = points[0]["bandwidth"]
            completion[(config, cell)] = points[0]["completion_cycles"]
    best_bandwidth = {cell: max(bandwidth[(config, cell)] for config in grouped)
                      for cell in TRADEOFF_CELLS}
    fastest_completion = {
        cell: min(completion[(config, cell)] for config in grouped)
        for cell in TRADEOFF_CELLS
    }

    summaries = []
    for config in grouped:
        vc, depth, ni_rx_depth = config
        r_rob_depth = max(row["r_rob_depth"] for row in grouped[config])
        throughput_cell = min(
            TRADEOFF_CELLS,
            key=lambda cell: bandwidth[(config, cell)] / best_bandwidth[cell])
        completion_cell = max(
            TRADEOFF_CELLS,
            key=lambda cell: completion[(config, cell)] / fastest_completion[cell])
        summaries.append({
            "config": config,
            "costs": (vc * depth, vc * ni_rx_depth,
                      NI_TX_DAT_DEPTH, r_rob_depth),
            "throughput": tuple(bandwidth[(config, cell)]
                                for cell in TRADEOFF_CELLS),
            "completion": tuple(completion[(config, cell)]
                                for cell in TRADEOFF_CELLS),
            "throughput_cell": throughput_cell,
            "throughput_retention": (100.0 * bandwidth[(config, throughput_cell)] /
                                     best_bandwidth[throughput_cell]),
            "completion_cell": completion_cell,
            "completion_ratio": (completion[(config, completion_cell)] /
                                 fastest_completion[completion_cell]),
        })

    def dominates(left, right):
        no_worse = (
            all(a <= b for a, b in zip(left["costs"], right["costs"])) and
            all(a >= b for a, b in zip(left["throughput"], right["throughput"])) and
            all(a <= b for a, b in zip(left["completion"], right["completion"]))
        )
        better = (
            any(a < b for a, b in zip(left["costs"], right["costs"])) or
            any(a > b for a, b in zip(left["throughput"], right["throughput"])) or
            any(a < b for a, b in zip(left["completion"], right["completion"]))
        )
        return no_worse and better

    for row in summaries:
        row["pareto"] = not any(
            dominates(other, row) for other in summaries if other is not row)
    summaries.sort(key=lambda row: (*row["costs"], *row["config"]))
    return summaries


def _tradeoff_table(baseline_rows, tradeoff_rows):
    if not tradeoff_rows:
        return "[TBD] VC／buffer sweep 尚未完成。"
    summaries = [row for row in _tradeoff_summaries(baseline_rows, tradeoff_rows)
                 if row["pareto"]]
    table_rows = []
    previous = None
    for row in summaries:
        vc, depth, _ni_rx_depth = row["config"]
        throughput_direction, throughput_mapping = row["throughput_cell"]
        completion_direction, completion_mapping = row["completion_cell"]
        improvements = []
        if previous is None:
            improvements.append("Baseline")
        else:
            for direction in ("write", "read"):
                indices = [index for index, cell in enumerate(TRADEOFF_CELLS)
                           if cell[0] == direction]
                metrics = []
                if any(row["throughput"][index] > previous["throughput"][index]
                       for index in indices):
                    metrics.append("higher Throughput")
                if any(row["completion"][index] < previous["completion"][index]
                       for index in indices):
                    metrics.append("lower Completion Time")
                if metrics:
                    improvements.append(f"{direction.title()}: {', '.join(metrics)}")
        table_rows.append([
            f"{vc} VC × {depth} flits", "<br>".join(improvements) or "None",
            f"{row['throughput_retention']:.1f} "
            f"({DISPLAY_NAME[throughput_mapping]} {throughput_direction.title()})",
            f"{row['completion_ratio']:.2f} "
            f"({DISPLAY_NAME[completion_mapping]} {completion_direction.title()})",
        ])
        previous = row
    table = _table([
        "DAT configuration", "Performance Improvement vs Previous Configuration",
        "Min. Normalized Throughput (%)",
        "Max. Normalized Completion Time (×)",
    ], table_rows)
    guidance = "\n".join([
        "固定條件：NI TX DAT depth = 8 entries/NI。Read RoB depth = 128 beat slots/NI。",
        "",
        "```text",
        "Normalized Throughput = measured throughput / highest measured throughput",
        "Normalized Completion Time = measured completion time / fastest measured completion time",
        "```",
        "",
        "- Pareto-dominates：各項 storage cost 不增加，各 workload／direction 的 throughput 不降低、completion time 不增加，且至少一項成本或性能嚴格改善。",
        "- Non-dominated：沒有其他 measured configuration 可以 Pareto-dominate 該設定。",
        "- Dominated：至少有一個 measured configuration 可以 Pareto-dominate 該設定。",
        "- Measured non-dominated set：所有 Non-dominated measured configurations 的集合。",
        "- Global Multicast Write、Cyclic M2M Write 與 Read 各自的 throughput、completion time 都是獨立 objective。各 workload 分別正規化，括號標出該設定最差的一項。",
        "- Normalized throughput 越接近 100% 越好。Normalized completion time 越接近 1.00× 越好。",
        "- Router DAT flits/input、NI RX DAT flits/NI、NI TX DAT entries/NI 與 Read RoB beat slots/NI 分別比較，不可直接相加。",
        "- 目前沒有 PPA limit，因此表格不指定最終 DUT configuration。",
    ])
    return guidance + "\n\n" + "### 6.2 Non-dominated configurations\n\n下表列出量測集合中的取捨。Improvement 欄只列相對前一列改善的項目，未列退步項目，不能視為逐列全面升級。完整數值見 6.3。" + "\n\n" + table


def _configuration_performance_table(baseline_rows, tradeoff_rows):
    summaries = _tradeoff_summaries(baseline_rows, tradeoff_rows)
    source_rows = baseline_rows + tradeoff_rows
    ideals = []
    for direction, mapping in TRADEOFF_CELLS:
        item = next(row for row in source_rows
                    if row["direction"] == direction
                    and row["mapping"] == mapping
                    and row["outstanding"] == 32)
        ideals.append(pm.ideal_throughput_bound(
            mapping, direction, item["burst_beats"], item["rounds"],
            item["multicast_mode"]))

    table_rows = []
    for row in summaries:
        vc, depth, _ni_rx_depth = row["config"]
        values = [
            f'{measured:.0f} / {ideal:.0f} '
            f'({100.0 * measured / ideal:.0f}%)'
            for measured, ideal in zip(row["throughput"], ideals)
        ]
        table_rows.append([f"{vc} VC × {depth}", *values])
    return _table(
        ["Config", "Multicast W", "Cyclic M2M W", "Cyclic M2M R"],
        table_rows)


def _ideal_vs_accepted_svg(rows):
    data = _performance_data(rows)
    width = 1600
    left = 410
    plot_width = 1000
    row_step = 44
    height = 125 + row_step * len(data)
    axis_max = math.ceil(max(max(row["ideal"], row["accepted"])
                             for row in data) / 200.0) * 200.0
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<title>Ideal Throughput Bound versus Accepted Throughput</title>',
        '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#1f2933}.title{font-size:26px;font-weight:700}.axis{font-size:16px;fill:#52606d}.label{font-size:17px}.value{font-size:15px}.grid{stroke:#d9e2ec;stroke-width:1}.ideal{fill:#9fb3c8}.accepted{fill:#2f6f9f}</style>',
        '<text class="title" x="34" y="36">Ideal Throughput Bound vs Accepted Throughput</text>',
        '<rect class="ideal" x="1110" y="18" width="20" height="12"/><text class="axis" x="1138" y="30">Ideal Bound</text>',
        '<rect class="accepted" x="1270" y="18" width="20" height="12"/><text class="axis" x="1298" y="30">Accepted</text>',
        f'<text class="axis" x="{left + plot_width / 2:.0f}" y="67" text-anchor="middle">B/cycle</text>',
    ]
    for tick in range(6):
        value = axis_max * tick / 5
        x = left + plot_width * tick / 5
        lines.append(f'<line class="grid" x1="{x:.1f}" y1="78" x2="{x:.1f}" y2="{height - 20}"/>')
        lines.append(f'<text class="axis" x="{x:.1f}" y="94" text-anchor="middle">{value:.0f}</text>')
    for index, row in enumerate(data):
        y = 112 + index * row_step
        label = f'{DISPLAY_NAME[row["mapping"]]} {row["direction"][0].upper()}'
        ideal_width = plot_width * row["ideal"] / axis_max
        accepted_width = plot_width * row["accepted"] / axis_max
        lines += [
            '<g class="metric-row">',
            f'<text class="label" x="{left - 18}" y="{y + 5}" text-anchor="end">{html.escape(label)}</text>',
            f'<rect class="ideal" x="{left}" y="{y - 12}" width="{ideal_width:.1f}" height="10"/>',
            f'<rect class="accepted" x="{left}" y="{y + 2}" width="{accepted_width:.1f}" height="10"/>',
            f'<text class="value" x="{left + ideal_width + 8:.1f}" y="{y - 3}">{row["ideal"]:.1f}</text>',
            f'<text class="value" x="{left + accepted_width + 8:.1f}" y="{y + 12}">{row["accepted"]:.1f}</text>',
            '</g>',
        ]
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def _hierarchical_alltoall_buffer_tradeoff_svg(baseline_rows, tradeoff_rows):
    summaries = _tradeoff_summaries(baseline_rows, tradeoff_rows)
    panels = (("Write", 1), ("Read", 2))
    width = 1480
    height = 620
    plot_top = 125
    plot_height = 380
    plot_width = 545
    panel_lefts = (115, 805)
    capacities = [row["costs"][0] for row in summaries]
    values = [row["throughput"][index]
              for _label, index in panels for row in summaries]
    spread = max(values) - min(values)
    padding = max(5.0, spread * 0.12)
    y_min = max(0.0, math.floor((min(values) - padding) / 10.0) * 10.0)
    y_max = math.ceil((max(values) + padding) / 10.0) * 10.0
    if y_max == y_min:
        y_max += 10.0
    x_max = max(capacities)
    label_offsets = {
        (1, 8): -14,
        (2, 8): -14,
        (1, 32): -14,
        (2, 16): 20,
        (4, 8): 38,
        (2, 32): -14,
        (4, 16): 20,
        (8, 8): 38,
    }
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<title>Cyclic Inter-region Many-to-Many Throughput versus DAT Router Buffer Capacity</title>',
        '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#1f2933}.title{font-size:26px;font-weight:700}.panel-title{font-size:20px;font-weight:700}.axis{font-size:15px;fill:#52606d}.label{font-size:14px;font-weight:700}.grid{stroke:#d9e2ec;stroke-width:1}.frame{fill:none;stroke:#829ab1;stroke-width:1.2}.pareto-point{fill:#2f6f9f;stroke:#174a70;stroke-width:1.5}.dominated-point{fill:#aeb8c2;stroke:#68737d;stroke-width:1.5}</style>',
        '<text class="title" x="34" y="38">Cyclic M2M Throughput vs DAT Router Buffer Capacity</text>',
        '<circle class="pareto-point" cx="1100" cy="31" r="7"/><text class="axis" x="1115" y="36">Non-dominated</text>',
        '<circle class="dominated-point" cx="1240" cy="31" r="7"/><text class="axis" x="1255" y="36">Dominated</text>',
        f'<text class="axis" x="34" y="{plot_top + plot_height / 2:.1f}" text-anchor="middle" transform="rotate(-90 34 {plot_top + plot_height / 2:.1f})">Throughput (B/cycle)</text>',
    ]
    for panel_index, (direction, metric_index) in enumerate(panels):
        left = panel_lefts[panel_index]
        bottom = plot_top + plot_height
        lines.append('<g class="panel">')
        lines.append(f'<text class="panel-title" x="{left + plot_width / 2:.1f}" y="88" text-anchor="middle">{direction}</text>')
        for tick in range(5):
            value = y_min + (y_max - y_min) * tick / 4
            y = bottom - plot_height * tick / 4
            lines.append(f'<line class="grid" x1="{left}" y1="{y:.1f}" x2="{left + plot_width}" y2="{y:.1f}"/>')
            lines.append(f'<text class="axis" x="{left - 12}" y="{y + 5:.1f}" text-anchor="end">{value:.0f}</text>')
        for tick in range(5):
            value = x_max * tick / 4
            x = left + plot_width * tick / 4
            lines.append(f'<line class="grid" x1="{x:.1f}" y1="{plot_top}" x2="{x:.1f}" y2="{bottom}"/>')
            lines.append(f'<text class="axis" x="{x:.1f}" y="{bottom + 25}" text-anchor="middle">{value:.0f}</text>')
        lines.append(f'<rect class="frame" x="{left}" y="{plot_top}" width="{plot_width}" height="{plot_height}"/>')
        lines.append(f'<text class="axis" x="{left + plot_width / 2:.1f}" y="{bottom + 58}" text-anchor="middle">DAT Router Buffer Capacity (flits/input)</text>')
        for row in summaries:
            capacity = row["costs"][0]
            value = row["throughput"][metric_index]
            x = left + plot_width * capacity / x_max
            y = bottom - plot_height * (value - y_min) / (y_max - y_min)
            point_class = "pareto-point" if row["pareto"] else "dominated-point"
            vc, depth, _ni_rx_depth = row["config"]
            label_y = y + label_offsets.get((vc, depth), -14)
            lines += [
                '<g class="candidate-point">',
                f'<circle class="{point_class}" cx="{x:.1f}" cy="{y:.1f}" r="7"/>',
                f'<text class="label" x="{x:.1f}" y="{label_y:.1f}" text-anchor="middle">{vc} VC × {depth}</text>',
                '</g>',
            ]
        lines.append('</g>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def write_report_figures(destination, baseline_rows, tradeoff_rows):
    destination = pathlib.Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    figures = {
        "ideal_vs_accepted_throughput.svg": _ideal_vs_accepted_svg(baseline_rows),
        "hierarchical_alltoall_buffer_tradeoff.svg":
            _hierarchical_alltoall_buffer_tradeoff_svg(
                baseline_rows, tradeoff_rows),
    }
    paths = []
    for name, svg in figures.items():
        path = destination / name
        path.write_text(svg, encoding="utf-8", newline="\n")
        paths.append(path)
    return paths


def collect_operations(directories):
    """Read explicitly selected DMA runs with their accounting and provenance."""
    import dependent_dma_plan as dma
    import emit_result_manifest as provenance
    import gen_tb_top

    rows = []
    for directory in directories:
        directory = pathlib.Path(directory).resolve()
        result = json.loads((directory / "operation.json").read_text())
        perf = json.loads((directory / "perf.json").read_text())
        manifest = json.loads((directory / "manifest.json").read_text())
        log = (directory / "run.log").read_text()
        knobs = {}
        for token in shlex.split(manifest["exact_command"]):
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            if key in knobs and knobs[key] != value:
                raise ValueError(f"replay changes {key}: {directory}")
            knobs[key] = value
        if knobs.get("DMA") != "1" or knobs.get("DMA_DEPENDENT") != "1":
            raise ValueError(f"not a dependent DMA measurement: {directory}")
        config = gen_tb_top.ROOT / "sim/configs" / (knobs["CONFIG"] + ".yml")
        if (manifest["config_file_sha256"] != provenance._sha256(config)
                or manifest["generated_parameter_sha256"] != provenance._parameter_sha256(gen_tb_top.ROOT)
                or manifest["source_patch_sha256"] != provenance._sha256(directory / "source.patch")
                or manifest["seed"] != int(knobs["SEED"])):
            raise ValueError(f"measurement provenance mismatch: {directory}")
        topo = gen_tb_top.load_topology(knobs["CONFIG"])
        resident = dma.parse_resident_shards(knobs["DMA_RESIDENT_SHARDS"]) if knobs.get("DMA_RESIDENT_SHARDS") else ()
        plan = dma.build_plan(topo, int(knobs["DMA_LENGTH"], 0), knobs["DMA_RW"],
                              knobs["DMA_OPERATION"], resident)
        if plan.contract:
            from xy_runtime_checker import validate_snapshot
            validate_snapshot(plan, directory)
        if dma.pass_marker(plan) not in log.splitlines():
            raise ValueError(f"missing byte-checked completion: {directory}")
        dma.validate_result(result, plan)
        dma.validate_perf_window(result, perf)
        memory = dma.validate_memory_reads(log, plan, topo)
        if any(t.phase for t in plan.transfers):
            from composite_dma_plan import phase_results
            observed = phase_results(log, plan)
            if observed != json.loads((directory / "phases.json").read_text()):
                raise ValueError(f"phase observations disagree: {directory}")
        links = perf["noc"]["links"]
        rows.append({**result, **memory, "directory": directory, "topology": knobs["CONFIG"],
                     "direction": "tile write/read" if plan.operation in ("kv_tile_roundtrip", "prefill_decode") else knobs["DMA_RW"],
                     "seed": manifest["seed"],
                     **({"contract": plan.contract, "estimated_time_cycles": None,
                         "slowdown": None, "estimate_status": "uncalibrated"} if plan.contract else {}),
                     "object_bytes": int(knobs["DMA_LENGTH"], 0),
                     "sim_opt": knobs.get("SIM_OPT", "-O0"),
                     "backpressure": knobs["DMA_BACKPRESSURE"],
                     "dat_injected_flits": sum(link["flit_count"] for link in links
                         if re.fullmatch(r"dat_inject_\d+|dat_node\d+\.y_to_node\d+\.router", link["name"])),
                     "dat_flit_hops": sum(link["flit_count"] for link in links
                         if re.fullmatch(r"dat_\d+to\d+", link["name"]))})
    if not rows:
        raise ValueError("select at least one operation result directory")
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Export validated DMA measurements as JSON. "
                    "The authored report is docs/perf_report_restructured.md.")
    parser.add_argument("out_dir", nargs="?", default="sim/verilator/output")
    parser.add_argument("-o", "--out", type=pathlib.Path,
                        help="JSON destination (default: OUT_DIR/operation_results.json)")
    parser.add_argument("--operations", nargs="+", type=pathlib.Path, required=True,
                        help="Explicit dependent-DMA run directories to validate and export")
    args = parser.parse_args(argv)
    destination = args.out or pathlib.Path(args.out_dir) / "operation_results.json"
    if destination.suffix.lower() != ".json":
        parser.error("measurement export requires a .json destination")
    rows = collect_operations(args.operations)
    # Keep raw-record locations portable relative to the exported data file.
    records = [{**row, "directory": os.path.relpath(
        row["directory"], destination.resolve().parent).replace(os.sep, "/")}
        for row in rows]
    payload = {"schema": 1, "records": records}
    encoded = json.dumps(payload, ensure_ascii=False, allow_nan=False, indent=2) + "\n"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(encoded, encoding="utf-8", newline="\n")
    print(f"wrote {destination} ({len(records)} validated records)")


if __name__ == "__main__":
    main()
