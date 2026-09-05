"""Emit one CSV row for a completed continuous-injection run.

Parses the axi_bw_monitor lines out of one run.log and writes result.csv beside
it. The row carries every parameter that bounds the measurement, so a number is
never separated from the configuration that produced it.

Per node or whole mesh, never guess from the name:

    accepted_bits_per_cycle        whole mesh, summed over every node monitor
    accepted_bytes_per_node_cycle  per node, the above / 8 / node count
    offered_*_per_node_cycle       per node, analytic
    mean_latency_*                 a mean, so neither

Accepted throughput is total delivered over the run, booksim2's batch-mode
convention, not a sum of the monitors' printed `BW:` fields. Each monitor
divides by its own span, so in a saturated run the nodes that finish early
report a rate over a short window and the sum overstates what the mesh carried
at any one moment. Measured, in the transpose run at offered 1.199: node1 and
node8 each retired 200 transactions of the same size, yet printed 235.85 and
83.97 bits per cycle, a 2.8x spread that is a 14329 cycle window against a
40347 cycle one. Summing those two rates describes no cycle of the run.

The rule, per monitor and then summed:

    bytes/cyc = (N_read + N_write) * beats * 64 / window

with `beats = burst_len + 1`, 64 B the monitor's charge per beat, and `window`
the common one, `window.end_cyc - window.start_cyc` from the `perf.json` beside
the log. A run with no perf.json falls back to summing `BW:`, which is each
monitor's own span; `window_source` says which of the two produced the row.
`accepted_bits_per_cycle` is the whole mesh, `accepted_bytes_per_node_cycle`
divides it by the node count, so a node that sent nothing (self traffic, which
never reaches a monitor) counts as a zero rather than shrinking the divisor.
The node count comes from a mesh_<x>x<y> topology name. Any other name leaves
the per-node column empty rather than inventing a divisor.

Every latency column is weighted by each monitor's sample count. A plain
average of the printed means is wrong: each is already a mean over that
monitor's own transaction count, and those counts differ.

mean_latency_network is the monitor value, measured from the AX handshake.
mean_latency_open adds the source queue delay the tb prints per node
([SrcQueue ...]), so it runs from the cycle the open-loop injection process
intended to issue, booksim2's packet latency. Under backpressure the two
diverge, below saturation they agree.

Each of the two also comes per channel, `_read` and `_write`. Read and write are
not the same measurement: axi_bw_monitor.sv timestamps a read at its first R
beat and a write at B, after the whole worm has landed, and the two return on
different planes (DAT for R, RSP for B). One mean over both compares against
neither, so the report's analytic read decomposition needs the read column. A
channel that reported no sample leaves its cells empty rather than zero.

Offered load is analytic, from the injection rate and the burst length, on the
DAT plane (the spec's convention):

    beats     = burst_len + 1          AxLEN + 1, per gen_test_patterns.py
    write     = 1 + beats flits        AW header plus its W beats
    read      = beats flits            R beats only, AR rides the REQ plane
    flits/cyc = rate * (1 + beats) + rate * beats
    bytes/cyc = rate * beats * 64 * 2  payload beats both directions

`many_to_many` is write-only, so its read terms are omitted:

    flits/cyc = rate * (1 + beats)
    bytes/cyc = rate * beats * 64

64 B is the AXI data bus width (specgen DATA_WIDTH 512 b), which is what the
monitor charges per beat ($bits(r.data) in axi_bw_monitor.sv). Charging the
same per beat is what makes offered and accepted comparable; a run at AxSIZE 5
fills only half of each beat, so neither number is net payload.
"""
import argparse
import csv
import json
import pathlib
import re
import sys

# AXI data bus width in bytes (specgen DATA_WIDTH 512 b), the monitor's charge
# per beat.
BEAT_BYTES = 64
AI_WRITE_ONLY = frozenset({
    "broadcast", "gather", "alltoall", "neighbor_exchange", "pipeline",
    "many_to_many",
})

# [Monitor node0.master][Read] Latency: 98.30 +- 4.10, N: 200, BW: 107.02 Bits/cycle, Util: 41.80%
_MON = re.compile(
    r"\[Monitor[^\]]*\]\[(Read|Write)\]\s+Latency:\s*([\d.]+)\s*\+-\s*[\d.]+,\s*"
    r"N:\s*(\d+),\s*BW:\s*([\d.]+)\s*Bits/cycle",
    re.I,
)
# [SrcQueue node0][Read] mean: 10.00, N: 100
_SRCQ = re.compile(
    r"\[SrcQueue[^\]]*\]\[(Read|Write)\]\s+mean:\s*([\d.]+),\s*N:\s*(\d+)",
    re.I,
)
_CONFIG = re.compile(
    r"\[Config\]\s+max_unique_ids=(\d+)\s+max_outstanding=(\d+)\s+dat_num_vc=(\d+)"
    r"(?:\s+router_vc_depth=(\d+))?(?:\s+mst_stall_random=(\d+))?"
    r"(?:\s+ni_dat_rx_vc_depth=(\d+))?")
_CHANNEL_BEATS = re.compile(
    r"^\[ChannelCompare\]\s+case=(write|read)\s+node=(\d+)\s+"
    r"bursts=(\d+)\s+data_beats=(\d+)\s*$", re.M)
_CHANNEL_INTERVAL = re.compile(
    r"^\[ChannelCompareInterval\]\s+role=(control|background)\s+node=(\d+)\s+"
    r"start_cycle=(\d+)\s+completion_cycle=(\d+)\s*$", re.M)
_CHANNEL_CONTROL_SRCQ = re.compile(
    r"^\[SrcQueue node0\]\[(Read|Write)\]\s+mean:\s*([\d.]+),\s*N:\s*(\d+)\s*$",
    re.M | re.I)
_CHANNEL_OVERLAP = re.compile(
    r"^\[ChannelCompareOverlap\]\s+control_node=0\s+background_nodes=(\d+)\s+"
    r"status=PASS\s*$", re.M)
_CHANNEL_BARRIER = re.compile(
    r"^\[ChannelCompareBarrier\]\s+release_cycle=(\d+)\s*$", re.M)
_CHANNEL_PASS = re.compile(r"^PASS: all \d+ nodes done, non-vacuous$", re.M)
_TRAFFIC_META = re.compile(
    r"^\[TrafficMeta\]\s+active_sources=(\d+)\s+write_bursts=(\d+)\s*$", re.M)
_WINDOW_TRAFFIC_META = re.compile(
    r"^\[TrafficMeta\]\s+direction=(write|read)\s+axi_initiators=(\d+)\s+"
    r"source_requests=(\d+)\s*$", re.M)
_ROUND_PERF = re.compile(
    r"^\[RoundPerf\]\s+start_cycle=(\d+)\s+completion_cycle=(\d+)\s+"
    r"round_cycles=(\d+)\s+active_sources=(\d+)\s+write_bursts=(\d+)\s*$", re.M)
_SOURCE_WINDOW = re.compile(
    r"^\[SourceWindow node\d+\]\s+depth=(\d+)\s+max_outstanding=(\d+)\s*$", re.M)
_NMU_HWM = re.compile(
    r"^\[HWM\]\s+node=\d+\s+read_slot_hwm=(\d+)\s+order_list_hwm=(\d+)\s+"
    r"write_txns_hwm=(\d+)\s+read_txns_hwm=(\d+)\s+"
    r"aw_clause=\{idle=\d+\s+same_dest=\d+\s+alloc=(\d+)\}\s+"
    r"ar_clause=\{idle=\d+\s+same_dest=\d+\s+alloc=(\d+)\}\s*$", re.M)
_NSU_HWM = re.compile(
    r"^\[NSU_HWM\]\s+node=\d+\s+write_meta_hwm=(\d+)\s+read_meta_hwm=(\d+)\s*$",
    re.M)
_MEASUREMENT_WINDOW = re.compile(
    r"^\[MeasurementWindow\]\s+start_cycle=(\d+)\s+completion_cycle=(\d+)\s*$",
    re.M)


def parse_source_window(log_text, expected_depth):
    rows = [(int(depth), int(hwm)) for depth, hwm in _SOURCE_WINDOW.findall(log_text)]
    if not rows:
        sys.exit("emit_result_csv: mode 4 requires [SourceWindow] evidence")
    if any(depth != expected_depth or hwm > depth for depth, hwm in rows):
        sys.exit("emit_result_csv: inconsistent [SourceWindow] evidence")
    return max(hwm for _, hwm in rows)


def parse_capacity_hwm(log_text):
    nmu = [tuple(map(int, row)) for row in _NMU_HWM.findall(log_text)]
    nsu = [tuple(map(int, row)) for row in _NSU_HWM.findall(log_text)]
    result = {
        "nmu_read_slot_hwm_beats": "",
        "nmu_order_list_hwm_transactions": "",
        "nmu_write_txns_hwm_transactions": "",
        "nmu_read_txns_hwm_transactions": "",
        "nmu_aw_fallback_allocations": "",
        "nmu_ar_fallback_allocations": "",
        "nsu_write_meta_hwm_transactions": "",
        "nsu_read_meta_hwm_transactions": "",
    }
    if nmu:
        maxima = [max(row[index] for row in nmu) for index in range(4)]
        result.update({
            "nmu_read_slot_hwm_beats": str(maxima[0]),
            "nmu_order_list_hwm_transactions": str(maxima[1]),
            "nmu_write_txns_hwm_transactions": str(maxima[2]),
            "nmu_read_txns_hwm_transactions": str(maxima[3]),
            "nmu_aw_fallback_allocations": str(sum(row[4] for row in nmu)),
            "nmu_ar_fallback_allocations": str(sum(row[5] for row in nmu)),
        })
    if nsu:
        result.update({
            "nsu_write_meta_hwm_transactions": str(max(row[0] for row in nsu)),
            "nsu_read_meta_hwm_transactions": str(max(row[1] for row in nsu)),
        })
    return result


def parse_traffic_meta(log_text):
    legacy_rows = _TRAFFIC_META.findall(log_text)
    window_rows = _WINDOW_TRAFFIC_META.findall(log_text)
    tagged = sum(line.startswith("[TrafficMeta]") for line in log_text.splitlines())
    if tagged != 1 or len(legacy_rows) + len(window_rows) != 1:
        sys.exit("emit_result_csv: expected exactly one [TrafficMeta] line")
    if window_rows:
        direction, initiators, requests = window_rows[0]
        if int(initiators) <= 0 or int(requests) <= 0:
            sys.exit("emit_result_csv: TrafficMeta counts must be positive")
        return {
            "direction": direction,
            "axi_initiators": initiators,
            "source_requests": requests,
        }
    active_sources, write_bursts = map(int, legacy_rows[0])
    if active_sources <= 0 or write_bursts <= 0:
        sys.exit("emit_result_csv: TrafficMeta counts must be positive")
    return {
        "round_active_sources": str(active_sources),
        "round_write_bursts": str(write_bursts),
    }


def parse_round_perf(log_text):
    rows = _ROUND_PERF.findall(log_text)
    tagged = sum(line.startswith("[RoundPerf]") for line in log_text.splitlines())
    if tagged != 1 or len(rows) != 1:
        sys.exit("emit_result_csv: expected exactly one [RoundPerf] line")
    start, completion, elapsed, active_sources, write_bursts = map(int, rows[0])
    if completion < start or elapsed != completion - start:
        sys.exit("emit_result_csv: inconsistent RoundPerf cycle evidence")
    if active_sources <= 0 or write_bursts <= 0:
        sys.exit("emit_result_csv: RoundPerf counts must be positive")
    meta = parse_traffic_meta(log_text)
    if (meta["round_active_sources"] != str(active_sources) or
            meta["round_write_bursts"] != str(write_bursts)):
        sys.exit("emit_result_csv: RoundPerf does not match TrafficMeta")
    return {
        "round_completion_cycles": str(elapsed),
        "round_active_sources": str(active_sources),
        "round_write_bursts": str(write_bursts),
    }


def load_traffic_meta(path, log_text, pattern, direction=None,
                      multicast_mode=None, burst_beats=None, bytes_per_beat=None):
    try:
        payload = json.loads(pathlib.Path(path).read_text())
    except (OSError, json.JSONDecodeError) as error:
        sys.exit(f"emit_result_csv: invalid traffic metadata: {error}")
    new_required = {
        "direction", "rounds", "data_producers", "consumers", "axi_initiators",
        "source_requests", "payload_deliveries",
    }
    geometry = {
        "transactions_per_flow", "burst_beats", "bytes_per_beat",
        "bytes_per_flow_round",
    }
    allowed = new_required | geometry
    if pattern == "broadcast":
        allowed |= {"multicast_mode", "destinations_per_source"}
        if multicast_mode is not None and "multicast_mode" not in payload:
            sys.exit("emit_result_csv: traffic metadata is missing multicast mode")
        if new_required <= set(payload) and "destinations_per_source" not in payload:
            sys.exit("emit_result_csv: traffic metadata is missing destinations per source")
    if set(payload) in (new_required, allowed):
        if payload["direction"] not in ("write", "read") or any(
                not isinstance(payload[name], int) or payload[name] <= 0
                for name in new_required - {"direction"}):
            sys.exit("emit_result_csv: invalid Outstanding traffic metadata")
        if direction != payload["direction"]:
            sys.exit("emit_result_csv: traffic direction does not match metadata")
        if pattern == "broadcast" and direction == "read":
            sys.exit("emit_result_csv: Broadcast Read is not supported")
        log_meta = parse_traffic_meta(log_text)
        if (log_meta.get("direction") != direction or
                int(log_meta.get("axi_initiators", 0)) != payload["axi_initiators"] or
                int(log_meta.get("source_requests", 0)) != payload["source_requests"]):
            sys.exit("emit_result_csv: traffic metadata does not match the run log")
        mode = payload.get("multicast_mode", "hardware" if pattern == "broadcast" else None)
        if multicast_mode is not None and mode != multicast_mode:
            sys.exit("emit_result_csv: multicast mode does not match traffic metadata")
        if pattern == "broadcast":
            fanout = payload["destinations_per_source"]
            if not isinstance(fanout, int) or fanout <= 0:
                sys.exit("emit_result_csv: destinations per source must be positive")
            flows = (payload["data_producers"] * payload["rounds"] *
                     payload.get("transactions_per_flow", 1))
            expected_requests = flows if mode == "hardware" else flows * fanout
            if (mode not in ("hardware", "repeated_unicast") or
                    payload["source_requests"] != expected_requests or
                    payload["payload_deliveries"] != flows * fanout):
                sys.exit("emit_result_csv: traffic metadata payload fanout is inconsistent")
        elif payload["payload_deliveries"] != payload["source_requests"]:
            sys.exit("emit_result_csv: traffic metadata payload fanout is inconsistent")
        if geometry <= set(payload):
            if any(not isinstance(payload[name], int) or payload[name] <= 0
                   for name in geometry):
                sys.exit("emit_result_csv: invalid traffic metadata geometry")
            if (payload["bytes_per_flow_round"] !=
                    payload["transactions_per_flow"] * payload["burst_beats"] *
                    payload["bytes_per_beat"] or
                    (burst_beats is not None and payload["burst_beats"] != burst_beats) or
                    (bytes_per_beat is not None and
                     payload["bytes_per_beat"] != bytes_per_beat)):
                sys.exit("emit_result_csv: traffic metadata geometry disagrees with the run")
        return payload

    required = {"active_sources", "source_write_bursts", "destination_deliveries"}
    if set(payload) != required or any(
            not isinstance(payload[name], int) or payload[name] <= 0
            for name in required):
        sys.exit("emit_result_csv: traffic metadata requires three positive integer counts")
    log_meta = parse_traffic_meta(log_text)
    if (payload["active_sources"] != int(log_meta["round_active_sources"]) or
            payload["source_write_bursts"] != int(log_meta["round_write_bursts"])):
        sys.exit("emit_result_csv: traffic metadata does not match the run log")
    deliveries = payload["destination_deliveries"]
    writes = payload["source_write_bursts"]
    if (pattern == "broadcast" and deliveries <= writes) or \
            (pattern != "broadcast" and deliveries != writes):
        sys.exit("emit_result_csv: traffic metadata destination fanout is inconsistent")
    return payload


def _channel_means(samples):
    """{"read": mean, "write": mean, "all": mean} from (channel, mean, count)
    triples, each mean weighted by its own sample count. A channel that reported
    no sample is absent from the result, and so is "all" on an empty input."""
    weighted = {}
    counts = {}
    for channel, mean, count in samples:
        channel = channel.lower()
        counts[channel] = counts.get(channel, 0) + int(count)
        weighted[channel] = weighted.get(channel, 0.0) + float(mean) * int(count)
    out = {c: weighted[c] / counts[c] for c in counts if counts[c]}
    total = sum(counts.values())
    if total:
        out["all"] = sum(weighted.values()) / total
    return out


def parse_monitors(log_text):
    """Return (summed BW, {channel: mean latency}, total sample count)."""
    rows = _MON.findall(log_text)
    total_samples = sum(int(n) for _c, _m, n, _bw in rows)
    if total_samples == 0:
        sys.exit("emit_result_csv: no monitor line reported a sample; the run injected nothing")
    latency = _channel_means((c, m, n) for c, m, n, _bw in rows)
    return sum(float(bw) for _c, _m, _n, bw in rows), latency, total_samples


def run_window(log_path, log_text=None, require_measurement_window=False):
    """Cycles the run spanned, from the perf.json the tb writes beside the log.

    None when there is no perf.json, which is the caller's signal to fall back
    to the monitors' own spans."""
    perf = pathlib.Path(log_path).with_name("perf.json")
    if not perf.is_file():
        return None
    w = json.loads(perf.read_text())["window"]
    if require_measurement_window:
        rows = _MEASUREMENT_WINDOW.findall(log_text or "")
        if len(rows) != 1:
            sys.exit("emit_result_csv: performance window requires one MeasurementWindow marker")
        start, completion = map(int, rows[0])
        if (start == 0 or completion <= start or
                w.get("start_cyc") != start or w.get("end_cyc") != completion):
            sys.exit("emit_result_csv: performance window does not match workload markers")
    return w["end_cyc"] - w["start_cyc"] or None


def dat_link_stats(log_path):
    perf = pathlib.Path(log_path).with_name("perf.json")
    if not perf.is_file():
        return None
    payload = json.loads(perf.read_text())
    cycles = payload["window"]["end_cyc"] - payload["window"]["start_cyc"]
    if cycles <= 0:
        sys.exit("emit_result_csv: invalid performance window")
    utilization = [100.0 * link["flit_count"] / cycles
                   for link in payload["noc"]["links"]
                   if link["name"].startswith("dat_")]
    if not utilization:
        sys.exit("emit_result_csv: no DAT-link counters in perf.json")
    return {
        "busiest_dat_link_utilization_pct": f"{max(utilization):.2f}",
        "dat_links_total": str(len(utilization)),
        **{f"dat_links_ge_{threshold}_pct": str(sum(value >= threshold
                                                     for value in utilization))
           for threshold in (25, 50, 75, 90)},
    }


def router_dat_stats(log_path):
    perf = pathlib.Path(log_path).with_name("perf.json")
    payload = json.loads(perf.read_text())["noc"]
    input_vcs = payload["router_dat_input_vcs"]
    output_vcs = payload["router_dat_output_vcs"]
    if not input_vcs or not output_vcs:
        sys.exit("emit_result_csv: missing Router DAT per-VC diagnostics")
    peak = max(input_vcs, key=lambda entry: entry["hwm_flits"])
    return {
        "router_dat_input_vc_hwm_max_flits": str(peak["hwm_flits"]),
        "router_dat_input_vc_capacity_flits": str(peak["capacity_flits"]),
        "router_dat_output_vc_credit_block_cycles_sum": str(sum(
            entry["credit_block_cycles"] for entry in output_vcs)),
    }


def parse_source_queue(log_text):
    """{channel: sample-weighted mean source queue delay} over every node.

    Empty when the log has no [SrcQueue] line (a run from before the open-loop
    tb) or when nothing was paced, which makes mean_latency_open fall back to
    mean_latency_network rather than to a hole in the row."""
    return _channel_means(_SRCQ.findall(log_text))


def mesh_nodes(topology):
    """Node count named by a mesh_<x>x<y> topology, None for other names."""
    m = re.match(r"mesh_(\d+)x(\d+)$", topology)
    return int(m.group(1)) * int(m.group(2)) if m else None


def offered_load(injection_rate, burst_len, write_only=False):
    """(flits, bytes) offered per node per cycle on DAT. See the module docstring."""
    rate = float(injection_rate)
    beats = int(burst_len) + 1
    if write_only:
        return rate * (1 + beats), rate * beats * BEAT_BYTES
    return rate * (1 + beats) + rate * beats, rate * beats * BEAT_BYTES * 2


def parse_config(log_text, cli_max_unique_ids, cli_max_outstanding):
    """(max_unique_ids, max_outstanding, dat_num_vc) from the tb's own `[Config]`
    line, which reflects the ni_params_pkg values the tb actually ran with. A CLI
    arg wins for the two meta-buffer depths as an explicit override.

    dat_num_vc has no override and is read here only. It used to be parsed out of
    the `_vc<N>` in the configuration name; the VC count now lives in
    specgen/source/constants.yaml and the config files are named for geometry
    alone, so the log is where the number meets the run that produced it."""
    m = _CONFIG.search(log_text)
    if not m:
        sys.exit(
            "emit_result_csv: no [Config] max_unique_ids=... dat_num_vc=... line in the log"
        )
    return (
        cli_max_unique_ids if cli_max_unique_ids is not None else m.group(1),
        cli_max_outstanding if cli_max_outstanding is not None else m.group(2),
        int(m.group(3)),
        int(m.group(4)) if m.group(4) else None,
        int(m.group(5)) if m.group(5) else None,
        int(m.group(6)) if m.group(6) else None,
    )


def parse_channel_compare(log_path, log_text, channel_mapping, channel_case, seed,
                          traffic_meta_path):
    """Return the multi-burst comparison row after proving its log evidence."""
    if not _CHANNEL_PASS.search(log_text):
        sys.exit("emit_result_csv: channel comparison did not reach exact non-vacuous PASS")
    if re.search(r"channel_compare_fault|\bfatal\b|%Error", log_text, re.I):
        sys.exit("emit_result_csv: channel comparison log contains fault or fatal evidence")

    expected_channel = channel_case.capitalize()
    control = []
    for line in log_text.splitlines():
        if not line.startswith(f"[Monitor node0.master][{expected_channel}]"):
            continue
        match = _MON.search(line)
        if match:
            control.append(match.groups())

    data = []
    for line in log_text.splitlines():
        if "[ChannelCompare]" not in line:
            continue
        match = _CHANNEL_BEATS.fullmatch(line)
        if not match:
            sys.exit("emit_result_csv: malformed [ChannelCompare] data evidence")
        data.append(match.groups())

    try:
        meta = json.loads(pathlib.Path(traffic_meta_path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        sys.exit(f"emit_result_csv: invalid channel traffic metadata: {error}")
    required = {
        "direction", "rounds", "transactions_per_flow", "control_probes",
        "background_bursts_per_flow", "background_beats_per_flow",
        "background_nodes", "shared_directed_edge", "control_resource",
        "rr_background_resource", "rrd_background_resource",
    }
    if set(meta) != required or meta["direction"] != channel_case:
        sys.exit("emit_result_csv: invalid channel traffic metadata")
    if any(not isinstance(meta[name], int) or meta[name] <= 0 for name in (
            "rounds", "transactions_per_flow", "control_probes",
            "background_bursts_per_flow", "background_beats_per_flow",
            "background_nodes")):
        sys.exit("emit_result_csv: invalid channel traffic metadata counts")
    if (meta["background_bursts_per_flow"] !=
            meta["rounds"] * meta["transactions_per_flow"] or
            meta["background_beats_per_flow"] !=
            meta["background_bursts_per_flow"] * 256):
        sys.exit("emit_result_csv: inconsistent channel traffic metadata geometry")
    edge = meta["shared_directed_edge"]
    if (meta["control_resource"] != f"req_{edge}" or
            meta["rr_background_resource"] != meta["control_resource"] or
            meta["rrd_background_resource"] != f"dat_{edge}"):
        sys.exit("emit_result_csv: channel resources do not name the shared directed edge")

    if (len(control) != 1 or control[0][0].lower() != channel_case or
            int(control[0][2]) != meta["control_probes"]):
        sys.exit("emit_result_csv: expected exactly 64 node 0 Control samples")
    source_queue = [row for row in _CHANNEL_CONTROL_SRCQ.findall(log_text)
                    if row[0].lower() == channel_case]
    if (len(source_queue) != 1 or
            int(source_queue[0][2]) != meta["control_probes"]):
        sys.exit("emit_result_csv: expected source-queue timing for every Control sample")
    if len(data) != meta["background_nodes"] or any(
            row[0] != channel_case or
            int(row[2]) != meta["background_bursts_per_flow"] or
            int(row[3]) != meta["background_beats_per_flow"]
            for row in data):
        sys.exit("emit_result_csv: Pipeline background burst proof is inconsistent")

    intervals = [(role, int(node), int(start), int(done))
                 for role, node, start, done in _CHANNEL_INTERVAL.findall(log_text)]
    controls = [row for row in intervals if row[0] == "control"]
    backgrounds = [row for row in intervals if row[0] == "background"]
    if (len(controls) != 1 or controls[0][1] != 0 or
            len(backgrounds) != meta["background_nodes"] or
            {row[1] for row in backgrounds} != {int(row[1]) for row in data} or
            any(start > controls[0][2] or done < controls[0][3]
                for _role, _node, start, done in backgrounds)):
        sys.exit("emit_result_csv: background intervals do not contain Control interval")
    overlap = _CHANNEL_OVERLAP.findall(log_text)
    if overlap != [str(meta["background_nodes"])]:
        sys.exit("emit_result_csv: missing ChannelCompare overlap proof")
    barriers = [int(cycle) for cycle in _CHANNEL_BARRIER.findall(log_text)]
    if (len(barriers) != 1 or
            barriers[0] < max(row[3] for row in intervals) or
            all(row[3] == barriers[0] for row in intervals)):
        sys.exit("emit_result_csv: ChannelCompare barrier replaced an actual completion cycle")

    perf_path = pathlib.Path(log_path).with_name("perf.json")
    try:
        perf = json.loads(perf_path.read_text(encoding="utf-8"))
        links = {link["name"]: int(link["flit_count"])
                 for link in perf["noc"]["links"]}
        window = perf["window"]
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
        sys.exit("emit_result_csv: invalid channel comparison perf.json")
    if (window.get("start_cyc") != min(row[2] for row in backgrounds) or
            window.get("end_cyc") != max(row[3] for row in backgrounds)):
        sys.exit("emit_result_csv: channel comparison counter window misses background interval")
    background_resource = (meta["rr_background_resource"]
                           if channel_mapping == "2-channel" else
                           meta["rrd_background_resource"])
    control_flits = meta["control_probes"] * (2 if channel_case == "write" else 1)
    background_flits = meta["background_bursts_per_flow"] * (
        257 if channel_case == "write" else 1)
    if background_resource == meta["control_resource"]:
        valid_resources = links.get(background_resource) == control_flits + background_flits
    else:
        valid_resources = (links.get(meta["control_resource"]) == control_flits and
                           links.get(background_resource) == background_flits)
    if not valid_resources:
        sys.exit("emit_result_csv: shared-edge channel resources do not match traffic metadata")

    return {
        "channel_mapping": channel_mapping,
        "channel_case": channel_case,
        "seed": seed,
        "control_samples": str(meta["control_probes"]),
        "control_mean_latency_cycles": str(
            float(control[0][1]) + float(source_queue[0][1])),
        "data_bursts_per_flow": str(meta["background_bursts_per_flow"]),
        "data_beats_per_flow": str(meta["background_beats_per_flow"]),
        "shared_directed_edge": meta["shared_directed_edge"],
        "control_resource": meta["control_resource"],
        "background_resource": background_resource,
        "overlap_status": "PASS",
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--topology", required=True)
    ap.add_argument("--pattern")
    ap.add_argument("--injection-mode")
    ap.add_argument("--injection-rate")
    ap.add_argument("--injection-count")
    ap.add_argument("--source-outstanding-depth", type=int)
    ap.add_argument("--traffic-direction", choices=("write", "read"))
    ap.add_argument("--seed")
    ap.add_argument("--max-unique-ids", default=None)
    ap.add_argument("--max-outstanding", default=None)
    # These do not appear in the tb [Config] line; unset means the shipped
    # used the generator / ni_params_pkg default, which is what gets recorded.
    ap.add_argument("--r-rob-depth", default="128")
    ap.add_argument("--max-txns-per-id", default="32")
    ap.add_argument("--ids-per-initiator", default="1")
    ap.add_argument("--burst-len", default="0")
    ap.add_argument("--stim-size")
    ap.add_argument("--space", default="memory")
    ap.add_argument("--traffic-meta")
    ap.add_argument("--traffic-mapping")
    ap.add_argument("--multicast-mode", choices=("hardware", "repeated_unicast"))
    ap.add_argument("--require-round-perf", action="store_true")
    ap.add_argument("--channel-mapping", choices=("2-channel", "3-channel"))
    ap.add_argument("--channel-case", choices=("write", "read"))
    a = ap.parse_args()

    if bool(a.channel_mapping) != bool(a.channel_case):
        ap.error("--channel-mapping and --channel-case must be used together")
    if a.channel_mapping and a.seed is None:
        ap.error("channel comparison requires --seed")
    if a.channel_mapping and not a.traffic_meta:
        ap.error("channel comparison requires --traffic-meta")
    if a.channel_mapping and (a.source_outstanding_depth != 32 or
                              a.max_txns_per_id != "32"):
        ap.error("channel comparison requires fixed Outstanding Depth 32 and MAX_TXNS_PER_ID 32")

    log_text = pathlib.Path(a.log).read_text()
    parse_traffic_meta(log_text)
    if a.channel_mapping:
        row = parse_channel_compare(
            a.log, log_text, a.channel_mapping, a.channel_case, a.seed, a.traffic_meta)
        row["source_outstanding_depth"] = "32"
        row["max_txns_per_id"] = "32"
        with open(a.out, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(row))
            writer.writeheader()
            writer.writerow(row)
        print(f"wrote {a.out}: {a.channel_mapping} {a.channel_case} mean latency "
              f"{row['control_mean_latency_cycles']} cycles")
        return

    if any(value is None for value in (
            a.pattern, a.injection_mode, a.injection_count, a.seed, a.stim_size)):
        ap.error("throughput results require --pattern, --injection-mode, "
                 "--injection-count, --seed, and --stim-size")

    bw, latency, samples = parse_monitors(log_text)
    srcq = parse_source_queue(log_text)
    windowed = a.injection_mode == "4"
    if windowed:
        if not a.source_outstanding_depth or a.source_outstanding_depth <= 0:
            ap.error("injection mode 4 requires --source-outstanding-depth=<positive integer>")
        if a.traffic_direction is None:
            ap.error("injection mode 4 requires --traffic-direction=write|read")
        source_outstanding_hwm = parse_source_window(
            log_text, a.source_outstanding_depth)
        offered_flits, offered_bytes = None, None
    else:
        if a.injection_rate is None:
            ap.error("non-windowed throughput results require --injection-rate")
        source_outstanding_hwm = None
        offered_flits, offered_bytes = offered_load(
            a.injection_rate, a.burst_len,
            write_only=a.pattern in AI_WRITE_ONLY)
    nodes = mesh_nodes(a.topology)
    window = run_window(a.log, log_text, windowed)
    traffic_meta = None
    if a.pattern in AI_WRITE_ONLY:
        if not a.traffic_meta or not a.traffic_mapping:
            ap.error("AI traffic results require --traffic-meta and --traffic-mapping")
        traffic_meta = load_traffic_meta(
            a.traffic_meta, log_text, a.pattern,
            a.traffic_direction if windowed else None, a.multicast_mode,
            int(a.burst_len) + 1, 1 << int(a.stim_size))
    if window:
        bw = samples * (int(a.burst_len) + 1) * BEAT_BYTES * 8 / window
        window_source = "run"
    else:
        window_source = "monitor"
    accepted_bytes = bw / 8 / nodes if nodes else None
    active_sources = (traffic_meta.get("active_sources") if traffic_meta else None)
    offered_mesh_avg = (offered_flits * active_sources / nodes
                        if offered_flits is not None and
                        active_sources is not None and nodes else None)
    accepted_injection = (
        traffic_meta["source_write_bursts"] * (int(a.burst_len) + 2) / window / nodes
        if traffic_meta and "source_write_bursts" in traffic_meta and window and nodes
        else None)
    deliveries = (traffic_meta.get("payload_deliveries",
                                   traffic_meta.get("destination_deliveries"))
                  if traffic_meta else None)
    transfer_bytes = 1 << int(a.stim_size)
    delivered_payload = (
        deliveries * (int(a.burst_len) + 1) * transfer_bytes / window
        if deliveries is not None and window else None)
    (max_unique_ids, max_outstanding, dat_num_vc, router_vc_depth, mst_stall_random,
     ni_dat_rx_vc_depth) = parse_config(log_text, a.max_unique_ids, a.max_outstanding)

    def lat(channel, open_loop):
        mean = latency.get(channel)
        if mean is None:
            return ""
        return f"{mean + (srcq.get(channel, 0.0) if open_loop else 0.0):.1f}"

    if windowed:
        if window is None:
            sys.exit("emit_result_csv: mode 4 requires perf.json")
        if not _CHANNEL_PASS.search(log_text) or re.search(
                r"Unexpected RData|Unexpected W last|RLAST mismatch|%Error", log_text):
            sys.exit("emit_result_csv: mode 4 requires clean non-vacuous checker PASS")
        direction_latency = lat(a.traffic_direction, True)
        if not direction_latency:
            sys.exit("emit_result_csv: selected direction has no completion samples")
        row = {
            "topology": a.topology,
            "vc": dat_num_vc,
            "router_vc_depth": router_vc_depth,
            "ni_dat_rx_vc_depth": ni_dat_rx_vc_depth,
            "pattern": a.pattern,
            "traffic_mapping": a.traffic_mapping,
            "measurement_mode": "outstanding",
            "direction": a.traffic_direction,
            "data_producers": str(traffic_meta["data_producers"]),
            "consumers": str(traffic_meta["consumers"]),
            "axi_initiators": str(traffic_meta["axi_initiators"]),
            "source_requests": str(traffic_meta["source_requests"]),
            "payload_deliveries": str(traffic_meta["payload_deliveries"]),
            "rounds": str(traffic_meta["rounds"]),
            "transactions_per_flow": str(traffic_meta.get("transactions_per_flow", 1)),
            "bytes_per_flow_round": str(traffic_meta.get(
                "bytes_per_flow_round", (int(a.burst_len) + 1) * transfer_bytes)),
            "multicast_mode": (traffic_meta.get("multicast_mode", "")
                               if a.pattern == "broadcast" else ""),
            "destinations_per_source": str(
                traffic_meta.get("destinations_per_source", "")),
            "source_outstanding_depth": str(a.source_outstanding_depth),
            "source_outstanding_hwm": str(source_outstanding_hwm),
            "seed": a.seed,
            "max_unique_ids": max_unique_ids,
            "max_outstanding": max_outstanding,
            "r_rob_depth": a.r_rob_depth,
            "max_txns_per_id": a.max_txns_per_id,
            "ids_per_initiator": a.ids_per_initiator,
            "burst_length": a.burst_len,
            "burst_beats": str(int(a.burst_len) + 1),
            "bytes_per_beat": str(transfer_bytes),
            "transaction_bytes": str((int(a.burst_len) + 1) * transfer_bytes),
            "completion_cycles": str(window),
            "completion_latency_cycles": direction_latency,
            "delivered_payload_bytes_per_cycle": str(round(delivered_payload, 6)),
            "checker_status": "PASS",
            **parse_capacity_hwm(log_text),
            **dat_link_stats(a.log),
            **router_dat_stats(a.log),
        }
        with open(a.out, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(row))
            writer.writeheader()
            writer.writerow(row)
        print(f"wrote {a.out}: {delivered_payload:.1f} B/cycle, "
              f"latency {direction_latency} cycles, outstanding {a.source_outstanding_depth}")
        return

    round_fields = ({
        "round_completion_cycles": "",
        "round_active_sources": "",
        "round_write_bursts": "",
    } if not a.require_round_perf and "[RoundPerf]" not in log_text
                    else parse_round_perf(log_text))
    row = {
        "topology": a.topology,
        "vc": dat_num_vc,
        "router_vc_depth": router_vc_depth,
        "ni_dat_rx_vc_depth": ni_dat_rx_vc_depth,
        "pattern": a.pattern,
        "traffic_mapping": a.traffic_mapping or "",
        "active_sources": "" if active_sources is None else str(active_sources),
        "injection_mode": a.injection_mode,
        "injection_rate": a.injection_rate,
        "injection_count": a.injection_count,
        "source_outstanding_depth": (
            "" if a.source_outstanding_depth is None else str(a.source_outstanding_depth)),
        "source_outstanding_hwm": (
            "" if source_outstanding_hwm is None else str(source_outstanding_hwm)),
        "seed": a.seed,
        "max_unique_ids": max_unique_ids,
        "max_outstanding": max_outstanding,
        "r_rob_depth": a.r_rob_depth,
        "max_txns_per_id": a.max_txns_per_id,
        "ids_per_initiator": a.ids_per_initiator,
        "burst_len": a.burst_len,
        "stim_size": a.stim_size,
        "space": a.space,
        "mst_stall_random": mst_stall_random,
        "offered_flits_per_node_cycle": (
            "" if offered_flits is None else str(round(offered_flits, 6))),
        "offered_bytes_per_node_cycle": (
            "" if offered_bytes is None else str(round(offered_bytes, 6))),
        "offered_load_per_active_source": (
            "" if offered_flits is None else str(round(offered_flits, 6))),
        "offered_load_mesh_avg": "" if offered_mesh_avg is None else str(
            round(offered_mesh_avg, 6)),
        "accepted_injection_load_mesh_avg": "" if accepted_injection is None else str(
            round(accepted_injection, 6)),
        "delivered_payload_bytes_per_cycle": "" if delivered_payload is None else str(
            round(delivered_payload, 6)),
        "destination_deliveries": "" if traffic_meta is None else str(
            traffic_meta["destination_deliveries"]),
        "accepted_bits_per_cycle": f"{bw:.1f}",
        "accepted_bytes_per_node_cycle": "" if accepted_bytes is None else str(
            round(accepted_bytes, 6)),
        "window_source": window_source,
        "mean_latency_network": lat("all", False),
        "mean_latency_network_read": lat("read", False),
        "mean_latency_network_write": lat("write", False),
        "mean_latency_open": lat("all", True),
        "mean_latency_open_read": lat("read", True),
        "mean_latency_open_write": lat("write", True),
        **round_fields,
    }
    with open(a.out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
    load = (f"offered {offered_flits:g} flits/node/cyc" if offered_flits is not None
            else f"source outstanding depth {a.source_outstanding_depth}")
    print(f"wrote {a.out}: {bw:.1f} bits/cyc, nlat {row['mean_latency_network']}, "
          f"plat {row['mean_latency_open']}, {load}")


if __name__ == "__main__":
    main()
