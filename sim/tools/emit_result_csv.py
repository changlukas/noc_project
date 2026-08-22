"""Emit one CSV row for a completed continuous-injection run.

Parses the axi_bw_monitor lines out of one run.log and writes result.csv beside
it. The row carries every parameter that bounds the measurement, so a number is
never separated from the configuration that produced it.

accepted_bits_per_cycle sums BW across all monitors, which is correct because
every monitor shares one cycle_cnt window.

mean_latency is weighted by each monitor's sample count. A plain average of the
printed means is wrong: each is already a mean over that monitor's own
transaction count, and those counts differ.
"""
import argparse
import csv
import pathlib
import re
import sys

# [Monitor node0.master][Read] Latency: 98.30 +- 4.10, N: 200, BW: 107.02 Bits/cycle, Util: 41.80%
_MON = re.compile(
    r"\[Monitor[^\]]*\]\[(?:Read|Write)\]\s+Latency:\s*([\d.]+)\s*\+-\s*[\d.]+,\s*"
    r"N:\s*(\d+),\s*BW:\s*([\d.]+)\s*Bits/cycle",
    re.I,
)
_CONFIG = re.compile(
    r"\[Config\]\s+max_unique_ids=(\d+)\s+max_outstanding=(\d+)\s+dat_num_vc=(\d+)"
    r"(?:\s+router_vc_depth=(\d+))?(?:\s+mst_stall_random=(\d+))?")


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
    max_unique_ids, max_outstanding, dat_num_vc, router_vc_depth, mst_stall_random = parse_config(
        log_text, a.max_unique_ids, a.max_outstanding
    )
    row = {
        "topology": a.topology,
        "vc": dat_num_vc,
        "router_vc_depth": router_vc_depth,
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
        "accepted_bits_per_cycle": f"{bw:.1f}",
        "mean_latency": f"{latency:.1f}",
    }
    with open(a.out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
    print(f"wrote {a.out}: {bw:.1f} bits/cyc, latency {latency:.1f}")


if __name__ == "__main__":
    main()
