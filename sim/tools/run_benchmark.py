#!/usr/bin/env python3
"""Benchmark runner: generate pattern, launch Vtb_top, gate on PASS, summarize perf.

Usage:
    python3 sim/tools/run_benchmark.py --topology <t> --pattern <p> \\
        [--seed S] [--transactions-per-node N] [--hotspot ids...] \\
        [--hotspot-rates r...] [--memory-size M] [--size AxSIZE] [--len AxLEN]

Flow:
    1. build topology tb if Vtb_top is absent (make build-verilator TOPOLOGY=<t>)
    2. gen_test_patterns.py -> output/<scenario>/coord/node<i>/scenario.yaml
    3. Vtb_top with all N +scenario_node<i>= plusargs
    4. PASS gate: require 'PASS: scenario complete, scoreboard clean' in stdout
    5. summarize perf.json -> output/<scenario>/bench_summary.json

Measurement window note: perf.json window=[0, total_cyc) spans the full run
including reset and drain cycles, not offered-load only. bench_summary.json
surfaces this window as-is with the 'injection' tag 'greedy-finite-trace-stress'.
"""

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent.parent   # repo root
GEN_TEST_PATTERNS = ROOT / "sim" / "tools" / "gen_test_patterns.py"
PASS_MARKER = "PASS: scenario complete, scoreboard clean"


# ---------------------------------------------------------------------------
# Topology helpers
# ---------------------------------------------------------------------------

def _load_topology(topology: str) -> dict:
    base = topology[:-4] if topology.endswith("_rob") else topology
    topo_path = ROOT / "sim" / "topologies" / f"{base}.yaml"
    return yaml.safe_load(topo_path.read_text())


def _node_count(topology: str) -> int:
    topo = _load_topology(topology)
    return topo["topology"]["x_dim"] * topo["topology"]["y_dim"]


def _find_exe(topology: str) -> Path | None:
    obj_dir = ROOT / "build" / "verilator" / f"obj_dir_{topology}"
    for name in ("Vtb_top.exe", "Vtb_top"):
        p = obj_dir / name
        if p.exists():
            return p
    return None


# ---------------------------------------------------------------------------
# Build helper
# ---------------------------------------------------------------------------

def _build(topology: str) -> None:
    print(f"[bench] building Vtb_top for {topology} ...")
    r = subprocess.run(
        ["make", "build-verilator", f"TOPOLOGY={topology}", "PYTHON3=python3"],
        cwd=str(ROOT),
    )
    if r.returncode != 0:
        sys.exit(f"[bench] ERROR: make build-verilator TOPOLOGY={topology} failed")


# ---------------------------------------------------------------------------
# perf summarize
# ---------------------------------------------------------------------------

def summarize(perf_json: dict) -> dict:
    """Compute bench_summary from a perf.json dict.

    Latency: mean / p95 (nearest-rank on latency.transactions[].latency) / max
    over ALL transactions in the run.
    Per-slot throughput: bytes_wr + bytes_rd per AXI slot.
    injection tag: 'greedy-finite-trace-stress' (single operating point, no
    injection-rate sweep).
    window: passed through from perf_json['window'] (full run incl. reset/drain).
    """
    txns = perf_json.get("latency", {}).get("transactions", [])
    latency_summary: dict = {}
    if txns:
        lats = sorted(t["latency"] for t in txns)
        n = len(lats)
        mean_val = sum(lats) / n
        # nearest-rank p95: ceil(0.95 * n) -> 1-based index
        p95_idx = math.ceil(0.95 * n) - 1   # 0-based
        p95_val = lats[p95_idx]
        latency_summary = {
            "count": n,
            "mean": round(mean_val, 2),
            "p95": p95_val,
            "max": lats[-1],
        }

    slots = perf_json.get("axi_slots", [])
    slot_throughput = []
    for s in slots:
        slot_throughput.append({
            "name": s.get("name", "?"),
            "bytes_wr": s.get("write_byte_count", 0),
            "bytes_rd": s.get("read_byte_count", 0),
            "txn_wr": s.get("write_txn_count", 0),
            "txn_rd": s.get("read_txn_count", 0),
        })

    noc = perf_json.get("noc", {})

    return {
        "injection": "greedy-finite-trace-stress",
        "scenario": perf_json.get("scenario", ""),
        "window": perf_json.get("window", {}),
        "latency": latency_summary,
        "slot_throughput": slot_throughput,
        "noc": {
            "link_count": len(noc.get("links", [])),
            "router_count": len(noc.get("routers", [])),
        },
    }


# ---------------------------------------------------------------------------
# Main runner
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="NoC benchmark runner (greedy single-point).")
    ap.add_argument("--topology", default="mesh_4x4_vc1")
    ap.add_argument("--pattern", required=True,
                    choices=["neighbor", "transpose", "uniform_random", "hotspot"])
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--transactions-per-node", type=int, default=1)
    ap.add_argument("--hotspot", type=int, nargs="+", default=None)
    ap.add_argument("--hotspot-rates", type=int, nargs="+", default=None)
    ap.add_argument("--memory-size", type=lambda v: int(str(v), 0), default=None)
    ap.add_argument("--size", type=int, default=2)
    ap.add_argument("--len", type=int, default=0, dest="burst_len")
    ap.add_argument("--exclude-self", action="store_true")
    ap.add_argument("--preserve-addr", action="store_true",
                    help="Forward to gen_test_patterns --preserve-addr (AX4 conformance cells).")
    ap.add_argument("--id-policy", default=None,
                    help="Forward to gen_test_patterns --id-policy (round_robin:N).")
    ap.add_argument("--out-root", default=None,
                    help="Root for output dirs (default: sim/verilator/output/bench_<scenario>)")
    _DEFAULT_BASE = str(
        ROOT / "sim" / "test_patterns"
        / "AX4-BAS-003_single_write_read_aligned" / "scenario.yaml"
    )
    ap.add_argument(
        "--from", dest="base", default=_DEFAULT_BASE,
        metavar="BASE_YAML",
        help="Base scenario.yaml forwarded to gen_test_patterns --from "
             "(default: AX4-BAS-003_single_write_read_aligned/scenario.yaml)",
    )
    a = ap.parse_args(argv)

    topology = a.topology
    n = _node_count(topology)

    # Build scenario name for output directory
    parts = [topology, a.pattern]
    if a.hotspot:
        parts.append("hs" + "_".join(str(h) for h in a.hotspot))
    parts.append(f"txn{a.transactions_per_node}")
    if a.seed:
        parts.append(f"seed{a.seed}")
    scenario_name = "_".join(parts)

    if a.out_root:
        out_root = Path(a.out_root)
    else:
        out_root = ROOT / "sim" / "verilator" / "output" / f"bench_{scenario_name}"

    coord_dir = out_root / "coord"
    perf_path = out_root / "perf.json"
    summary_path = out_root / "bench_summary.json"

    # 1. Build if needed
    exe = _find_exe(topology)
    if exe is None:
        _build(topology)
        exe = _find_exe(topology)
    if exe is None:
        sys.exit(f"[bench] ERROR: Vtb_top for {topology} not found after build")

    # 2. Generate pattern
    print(f"[bench] generating pattern={a.pattern} topology={topology} n={n} ...")
    gen_args = [
        sys.executable, str(GEN_TEST_PATTERNS),
        "--pattern", a.pattern,
        "--topology", topology,
        "--out", str(coord_dir),
        "--transactions-per-node", str(a.transactions_per_node),
        "--seed", str(a.seed),
        "--size", str(a.size),
        "--len", str(a.burst_len),
    ]
    if a.hotspot:
        gen_args += ["--hotspot"] + [str(h) for h in a.hotspot]
    if a.hotspot_rates:
        gen_args += ["--hotspot-rates"] + [str(r) for r in a.hotspot_rates]
    if a.memory_size is not None:
        gen_args += ["--memory-size", hex(a.memory_size)]
    if a.exclude_self:
        gen_args.append("--exclude-self")
    if a.preserve_addr:
        gen_args.append("--preserve-addr")
    if a.id_policy:
        gen_args += ["--id-policy", a.id_policy]
    if a.base:
        gen_args += ["--from", a.base]

    r = subprocess.run(gen_args, check=True)

    # 3. Launch simulation
    print(f"[bench] running {exe.name} ({n} nodes) ...")
    sim_args = [str(exe)]
    for i in range(n):
        node_yaml = coord_dir / f"node{i}" / "scenario.yaml"
        sim_args.append(f"+scenario_node{i}={node_yaml}")
    sim_args += [f"+perf_out={perf_path}", f"+perf_scenario={scenario_name}"]

    out_root.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(sim_args, capture_output=True, text=True,
                       cwd=str(ROOT / "sim" / "verilator"))

    # Move stray dump files if any (overwrite on Windows where rename() fails on exist)
    for f in (ROOT / "sim" / "verilator").glob("master_wrap_read_dump*.txt"):
        dst = out_root / f.name
        if dst.exists():
            dst.unlink()
        f.rename(dst)

    # Print stdout/stderr for visibility
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="", file=sys.stderr)

    # 4. Correctness gate
    if PASS_MARKER not in r.stdout:
        sys.exit(
            f"[bench] FAIL: PASS marker not found in stdout\n"
            f"  expected: {PASS_MARKER!r}\n"
            f"  returncode={r.returncode}"
        )
    print(f"[bench] correctness gate: PASS")

    # 5. Summarize
    if not perf_path.exists():
        sys.exit(f"[bench] ERROR: perf.json not produced at {perf_path}")

    with open(perf_path) as f:
        perf_json = json.load(f)

    summary = summarize(perf_json)
    with open(summary_path, "w") as f:
        json.dump(summary, f, indent=2)

    print(f"[bench] bench_summary.json -> {summary_path}")
    lat = summary.get("latency", {})
    if lat:
        print(f"[bench] latency: mean={lat['mean']} p95={lat['p95']} max={lat['max']} "
              f"(n={lat['count']} txns, window={summary['window']})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
