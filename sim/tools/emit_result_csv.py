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
    r"^\[ChannelCompare\]\s+case=(write|read)\s+node=([12])\s+"
    r"bursts=(\d+)\s+data_beats=(\d+)\s*$", re.M)
_CHANNEL_PASS = re.compile(r"^PASS: all \d+ nodes done, non-vacuous$", re.M)
_TRAFFIC_META = re.compile(
    r"^\[TrafficMeta\]\s+active_sources=(\d+)\s+write_bursts=(\d+)\s*$", re.M)
_ROUND_PERF = re.compile(
    r"^\[RoundPerf\]\s+start_cycle=(\d+)\s+completion_cycle=(\d+)\s+"
    r"round_cycles=(\d+)\s+active_sources=(\d+)\s+write_bursts=(\d+)\s*$", re.M)


def parse_traffic_meta(log_text):
    rows = _TRAFFIC_META.findall(log_text)
    tagged = sum(line.startswith("[TrafficMeta]") for line in log_text.splitlines())
    if tagged != 1 or len(rows) != 1:
        sys.exit("emit_result_csv: expected exactly one [TrafficMeta] line")
    active_sources, write_bursts = map(int, rows[0])
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


def run_window(log_path):
    """Cycles the run spanned, from the perf.json the tb writes beside the log.

    None when there is no perf.json, which is the caller's signal to fall back
    to the monitors' own spans."""
    perf = pathlib.Path(log_path).with_name("perf.json")
    if not perf.is_file():
        return None
    w = json.loads(perf.read_text())["window"]
    return w["end_cyc"] - w["start_cyc"] or None


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


def parse_channel_compare(log_text, channel_mapping, channel_case, seed):
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

    if len(control) != 1 or control[0][0].lower() != channel_case or int(control[0][2]) != 64:
        sys.exit("emit_result_csv: expected exactly 64 node 0 Control samples")
    if len(data) != 2 or {row[1] for row in data} != {"1", "2"} or any(
            row[0] != channel_case or int(row[2]) != 64 or int(row[3]) != 16384
            for row in data):
        sys.exit("emit_result_csv: node 1/2 Data burst proof is inconsistent")

    return {
        "channel_mapping": channel_mapping,
        "channel_case": channel_case,
        "seed": seed,
        "control_samples": "64",
        "control_mean_latency_cycles": str(float(control[0][1])),
        "data_bursts_per_node": "64",
        "data_beats_per_node": "16384",
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
    ap.add_argument("--seed")
    ap.add_argument("--max-unique-ids", default=None)
    ap.add_argument("--max-outstanding", default=None)
    # The next three do not appear in the tb [Config] line; unset means the run
    # used the generator / ni_params_pkg default, which is what gets recorded.
    ap.add_argument("--max-txns-per-id", default="32")
    ap.add_argument("--ids-per-initiator", default="1")
    ap.add_argument("--burst-len", default="0")
    ap.add_argument("--stim-size")
    ap.add_argument("--space", default="memory")
    ap.add_argument("--require-round-perf", action="store_true")
    ap.add_argument("--channel-mapping", choices=("2-channel", "3-channel"))
    ap.add_argument("--channel-case", choices=("write", "read"))
    a = ap.parse_args()

    if bool(a.channel_mapping) != bool(a.channel_case):
        ap.error("--channel-mapping and --channel-case must be used together")
    if a.channel_mapping and a.seed is None:
        ap.error("channel comparison requires --seed")

    log_text = pathlib.Path(a.log).read_text()
    parse_traffic_meta(log_text)
    if a.channel_mapping:
        row = parse_channel_compare(log_text, a.channel_mapping, a.channel_case, a.seed)
        with open(a.out, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(row))
            writer.writeheader()
            writer.writerow(row)
        print(f"wrote {a.out}: {a.channel_mapping} {a.channel_case} mean latency "
              f"{row['control_mean_latency_cycles']} cycles")
        return

    if any(value is None for value in (
            a.pattern, a.injection_mode, a.injection_rate, a.injection_count, a.seed,
            a.stim_size)):
        ap.error("throughput results require --pattern, --injection-mode, --injection-rate, "
                 "--injection-count, --seed, and --stim-size")

    bw, latency, samples = parse_monitors(log_text)
    srcq = parse_source_queue(log_text)
    offered_flits, offered_bytes = offered_load(
        a.injection_rate, a.burst_len,
        write_only=a.pattern in {"broadcast", "gather", "alltoall",
                                 "neighbor_exchange", "pipeline", "many_to_many"})
    nodes = mesh_nodes(a.topology)
    window = run_window(a.log)
    if window:
        bw = samples * (int(a.burst_len) + 1) * BEAT_BYTES * 8 / window
        window_source = "run"
    else:
        window_source = "monitor"
    accepted_bytes = bw / 8 / nodes if nodes else None
    (max_unique_ids, max_outstanding, dat_num_vc, router_vc_depth, mst_stall_random,
     ni_dat_rx_vc_depth) = parse_config(log_text, a.max_unique_ids, a.max_outstanding)

    def lat(channel, open_loop):
        mean = latency.get(channel)
        if mean is None:
            return ""
        return f"{mean + (srcq.get(channel, 0.0) if open_loop else 0.0):.1f}"

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
        "injection_mode": a.injection_mode,
        "injection_rate": a.injection_rate,
        "injection_count": a.injection_count,
        "seed": a.seed,
        "max_unique_ids": max_unique_ids,
        "max_outstanding": max_outstanding,
        "max_txns_per_id": a.max_txns_per_id,
        "ids_per_initiator": a.ids_per_initiator,
        "burst_len": a.burst_len,
        "stim_size": a.stim_size,
        "space": a.space,
        "mst_stall_random": mst_stall_random,
        "offered_flits_per_node_cycle": str(round(offered_flits, 6)),
        "offered_bytes_per_node_cycle": str(round(offered_bytes, 6)),
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
    print(f"wrote {a.out}: {bw:.1f} bits/cyc, nlat {row['mean_latency_network']}, "
          f"plat {row['mean_latency_open']}, offered {offered_flits:g} flits/node/cyc")


if __name__ == "__main__":
    main()
