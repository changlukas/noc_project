#!/usr/bin/env python3
"""Co-sim regression runner. Launches Vtb_top per validated scenario, checks PASS.
Replaces the retired ctest test_cosim_integration.cpp (sim != test: co-sim is a
simulation regression, run here, not in the C++ ctest suite)."""
import subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent          # repo root
PATTERNS = ROOT / "sim" / "test_patterns"
EXE = next((ROOT / "build/verilator").rglob("Vtb_top.exe"), None) \
      or next((ROOT / "build/verilator").rglob("Vtb_top"), None)
PASS = "PASS: scenario complete, scoreboard clean"

def main():
    if EXE is None:
        print("Vtb_top not built — run `make build-verilator` first"); return 2
    scns = sorted(p.name for p in PATTERNS.glob("AX4-*") if p.is_dir())
    run = fail = skip = 0
    for scn in scns:
        d = PATTERNS / scn
        # skip rules ported from test_cosim_integration.cpp:
        if scn.startswith("AX4-INF-"):            skip += 1; continue   # dedicated test
        n0 = d / "node0" / "scenario.yaml"; n1 = d / "node1" / "scenario.yaml"
        if not (n0.exists() and n1.exists()):     skip += 1; continue   # not in bidirectional subset
        run += 1
        out = (PATTERNS.parent / "verilator" / "output" / scn); out.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([str(EXE),
            f"+scenario_node0={n0}", f"+scenario_node1={n1}",
            f"+perf_out={out/'perf.json'}", f"+perf_scenario={scn}"],
            capture_output=True, text=True, cwd=str(PATTERNS.parent / "verilator"))
        ok = r.returncode == 0 and PASS in r.stdout
        print(f"{'PASS' if ok else 'FAIL'}  {scn}")
        if not ok:
            fail += 1
            sys.stdout.write(r.stdout[-800:] + r.stderr[-400:])
    print(f"\nco-sim regress: {run-fail}/{run} passed, {skip} skipped")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
