#!/usr/bin/env python3
"""Co-sim regression runner. Launches Vtb_top per validated scenario, checks PASS.
Replaces the retired ctest test_cosim_integration.cpp (sim != test: co-sim is a
simulation regression, run here, not in the C++ ctest suite).

Topology-aware: $TOPOLOGY (default mesh_4x4_vc1) selects the node count and the
per-topology Vtb_top binary (build/verilator/obj_dir_<TOPOLOGY>/Vtb_top). A 2-node
topology uses the committed node0/node1 variant dirs; an N>2 topology materializes
node0..node<N-1> coordinate variants on the fly from each pattern's base
scenario.yaml via gen_coordinate_scenarios.py, then drives all N +scenario_node<i>
plusargs. Either way the run is non-vacuous: every node both sends and receives,
and the PASS guard inside tb_top asserts master_count == N and reads_checked >= N."""
import os
import subprocess
import sys
import yaml
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # repo root
PATTERNS = ROOT / "sim" / "test_patterns"
COORD_GEN = ROOT / "sim" / "tools" / "gen_coordinate_scenarios.py"
PASS = "PASS: scenario complete, scoreboard clean"

TOPOLOGY = os.environ.get("TOPOLOGY", "mesh_4x4_vc1")


def _find_exe(topology: str):
    """Locate Vtb_top under build/verilator/obj_dir_<topology>/."""
    obj_dir = ROOT / "build" / "verilator" / f"obj_dir_{topology}"
    for name in ("Vtb_top.exe", "Vtb_top"):
        p = obj_dir / name
        if p.exists():
            return p
    return None


def node_count(topology: str) -> int:
    topo = yaml.safe_load((ROOT / "sim" / "topologies" / f"{topology}.yaml").read_text())
    return topo["topology"]["x_dim"] * topo["topology"]["y_dim"]


def node_scenarios(scn_dir: Path, n: int, out_dir: Path):
    """Return the list of per-node scenario.yaml paths, or None to skip.

    The validated bidirectional subset is encoded by the presence of committed
    node0/node1 dirs (the patterns the 2-node co-sim runs). A pattern WITHOUT
    those dirs is skipped for every topology, so N>2 stays on the same curated,
    clean write->read subset rather than forcing every pattern (write-only,
    OOB-DECERR, extreme-outstanding) through a multi-hop ring.

    2-node uses the committed node0/node1 dirs unchanged; N>2 materializes
    node0..node<N-1> coordinate variants from the base scenario.yaml.
    """
    if not all((scn_dir / f"node{i}" / "scenario.yaml").exists() for i in range(2)):
        return None  # not in the validated bidirectional subset
    if n == 2:
        return [scn_dir / f"node{i}" / "scenario.yaml" for i in range(2)]
    base = scn_dir / "scenario.yaml"
    if not base.exists():
        return None
    coord = out_dir / "coord"
    coord.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(COORD_GEN), str(base), str(coord),
                    "--topology", TOPOLOGY], check=True)
    return [coord / f"node{i}" / "scenario.yaml" for i in range(n)]


def main():
    EXE = _find_exe(TOPOLOGY)
    if EXE is None:
        print(f"Vtb_top not built for {TOPOLOGY} — run `make build-verilator TOPOLOGY={TOPOLOGY}` first")
        return 2
    n = node_count(TOPOLOGY)
    scns = sorted(p.name for p in PATTERNS.glob("AX4-*") if p.is_dir())
    run = fail = skip = 0
    for scn in scns:
        d = PATTERNS / scn
        if scn.startswith("AX4-INF-"):            skip += 1; continue   # dedicated test
        # Both 2-node and N>2 restrict to the curated bidirectional subset
        # (node0/node1 dirs committed); N>2 materializes variants from that subset.
        out = (PATTERNS.parent / "verilator" / "output" / scn)
        node_paths = node_scenarios(d, n, out)
        if node_paths is None:
            skip += 1; continue
        run += 1
        out.mkdir(parents=True, exist_ok=True)
        args = [str(EXE)]
        for i, p in enumerate(node_paths):
            args.append(f"+scenario_node{i}={p}")
        args += [f"+perf_out={out/'perf.json'}", f"+perf_scenario={scn}"]
        r = subprocess.run(args, capture_output=True, text=True,
                           cwd=str(PATTERNS.parent / "verilator"))
        ok = r.returncode == 0 and PASS in r.stdout
        print(f"{'PASS' if ok else 'FAIL'}  {scn}")
        if not ok:
            fail += 1
            sys.stdout.write(r.stdout[-800:] + r.stderr[-400:])
    print(f"\nco-sim regress ({TOPOLOGY}, {n} nodes): {run-fail}/{run} passed, {skip} skipped")
    if run == 0:
        print("FAIL: 0 scenarios ran (expected >=1) — vacuous pass guard"); return 1
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
