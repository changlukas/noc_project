"""Emit one CSV row for a completed continuous-injection run.

Parses the axi_bw_monitor lines out of one run.log and writes result.csv beside
it. The row carries every parameter that bounds the measurement, so a number is
never separated from the configuration that produced it.

accepted_bits_per_cycle sums BW across all monitors, which is correct because
every monitor shares one cycle_cnt window.

Both latency columns are weighted by each monitor's sample count. A plain
average of the printed means is wrong: each is already a mean over that
monitor's own transaction count, and those counts differ.

mean_latency_network is the monitor value, measured from the AX handshake.
mean_latency_open adds the source queue delay the tb prints per node
([SrcQueue ...]), so it runs from the cycle the open-loop injection process
intended to issue, booksim2's packet latency. Under backpressure the two
diverge; below saturation they agree.

Offered load is analytic, from the injection rate and the burst length, on the
DAT plane (the spec's convention):

    beats     = burst_len + 1          AxLEN + 1, per gen_test_patterns.py
    write     = 1 + beats flits        AW header plus its W beats
    read      = beats flits            R beats only, AR rides the REQ plane
    flits/cyc = rate * (1 + beats) + rate * beats
    bytes/cyc = rate * beats * 64 * 2  payload beats both directions

64 B is the AXI data bus width (specgen DATA_WIDTH 512 b), which is what the
monitor charges per beat ($bits(r.data) in axi_bw_monitor.sv). Charging the
same per beat is what makes offered and accepted comparable; a run at AxSIZE 5
fills only half of each beat, so neither number is net payload.
"""
import argparse
import csv
import pathlib
import re
import sys

# AXI data bus width in bytes (specgen DATA_WIDTH 512 b), the monitor's charge
# per beat.
BEAT_BYTES = 64

# [Monitor node0.master][Read] Latency: 98.30 +- 4.10, N: 200, BW: 107.02 Bits/cycle, Util: 41.80%
_MON = re.compile(
    r"\[Monitor[^\]]*\]\[(?:Read|Write)\]\s+Latency:\s*([\d.]+)\s*\+-\s*[\d.]+,\s*"
    r"N:\s*(\d+),\s*BW:\s*([\d.]+)\s*Bits/cycle",
    re.I,
)
# [SrcQueue node0][Read] mean: 10.00, N: 100
_SRCQ = re.compile(
    r"\[SrcQueue[^\]]*\]\[(?:Read|Write)\]\s+mean:\s*([\d.]+),\s*N:\s*(\d+)",
    re.I,
)
_CONFIG = re.compile(
    r"\[Config\]\s+max_unique_ids=(\d+)\s+max_outstanding=(\d+)\s+dat_num_vc=(\d+)"
    r"(?:\s+router_vc_depth=(\d+))?(?:\s+mst_stall_random=(\d+))?"
    r"(?:\s+ni_dat_rx_vc_depth=(\d+))?")


def parse_monitors(log_text):
    """Return (summed BW, sample-weighted mean latency)."""
    total_bw = 0.0
    weighted_latency = 0.0
    total_samples = 0
    for mean, count, bw in _MON.findall(log_text):
        count = int(count)
        total_bw += float(bw)
        weighted_latency += float(mean) * count
        total_samples += count
    if total_samples == 0:
        sys.exit("emit_result_csv: no monitor line reported a sample; the run injected nothing")
    return total_bw, weighted_latency / total_samples


def parse_source_queue(log_text):
    """Sample-weighted mean source queue delay over every node and channel.

    Zero when the log has no [SrcQueue] line (a run from before the open-loop
    tb) or when nothing was paced, which makes mean_latency_open fall back to
    mean_latency_network rather than to a hole in the row."""
    weighted = 0.0
    samples = 0
    for mean, count in _SRCQ.findall(log_text):
        count = int(count)
        weighted += float(mean) * count
        samples += count
    return weighted / samples if samples else 0.0


def offered_load(injection_rate, burst_len):
    """(flits, bytes) offered per node per cycle on DAT. See the module docstring."""
    rate = float(injection_rate)
    beats = int(burst_len) + 1
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--topology", required=True)
    ap.add_argument("--pattern", required=True)
    ap.add_argument("--injection-mode", required=True)
    ap.add_argument("--injection-rate", required=True)
    ap.add_argument("--injection-count", required=True)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--max-unique-ids", default=None)
    ap.add_argument("--max-outstanding", default=None)
    # The next three do not appear in the tb [Config] line; unset means the run
    # used the generator / ni_params_pkg default, which is what gets recorded.
    ap.add_argument("--max-txns-per-id", default="32")
    ap.add_argument("--ids-per-initiator", default="1")
    ap.add_argument("--burst-len", default="0")
    ap.add_argument("--space", default="memory")
    a = ap.parse_args()

    log_text = pathlib.Path(a.log).read_text()
    bw, latency = parse_monitors(log_text)
    srcq = parse_source_queue(log_text)
    offered_flits, offered_bytes = offered_load(a.injection_rate, a.burst_len)
    (max_unique_ids, max_outstanding, dat_num_vc, router_vc_depth, mst_stall_random,
     ni_dat_rx_vc_depth) = parse_config(log_text, a.max_unique_ids, a.max_outstanding)
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
        "space": a.space,
        "mst_stall_random": mst_stall_random,
        "offered_flits_per_node_cycle": str(round(offered_flits, 6)),
        "offered_bytes_per_node_cycle": str(round(offered_bytes, 6)),
        "accepted_bits_per_cycle": f"{bw:.1f}",
        "mean_latency_network": f"{latency:.1f}",
        "mean_latency_open": f"{latency + srcq:.1f}",
    }
    with open(a.out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
    print(f"wrote {a.out}: {bw:.1f} bits/cyc, nlat {latency:.1f}, plat {latency + srcq:.1f}, "
          f"offered {offered_flits:g} flits/node/cyc")


if __name__ == "__main__":
    main()
