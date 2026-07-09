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

# [Monitor node0.manager][Read] Latency: 98.30 +- 4.10, N: 200, BW: 107.02 Bits/cycle, Util: 41.80%
_MON = re.compile(
    r"\[Monitor[^\]]*\]\[(?:Read|Write)\]\s+Latency:\s*([\d.]+)\s*\+-\s*[\d.]+,\s*"
    r"N:\s*(\d+),\s*BW:\s*([\d.]+)\s*Bits/cycle",
    re.I,
)
_VC = re.compile(r"_vc(\d+)")


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


def vc_count(topology):
    m = _VC.search(topology)
    if not m:
        sys.exit(f"emit_result_csv: no _vc<N> in topology name {topology!r}")
    return int(m.group(1))


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
    ap.add_argument("--max-unique-ids", required=True)
    ap.add_argument("--max-outstanding", required=True)
    a = ap.parse_args()

    bw, latency = parse_monitors(pathlib.Path(a.log).read_text())
    row = {
        "topology": a.topology,
        "vc": vc_count(a.topology),
        "pattern": a.pattern,
        "injection_mode": a.injection_mode,
        "injection_rate": a.injection_rate,
        "injection_count": a.injection_count,
        "seed": a.seed,
        "max_unique_ids": a.max_unique_ids,
        "max_outstanding": a.max_outstanding,
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
