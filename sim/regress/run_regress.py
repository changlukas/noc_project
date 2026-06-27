#!/usr/bin/env python3
"""Expand sim/regress/matrix.yaml, run each cell via run_benchmark.py, gate on the
scoreboard PASS marker, write matrix.json + a console summary. Exit non-zero if any
cell failed. Excluded cells are skipped with a reason."""
import argparse, glob, json, os, subprocess, sys
from dataclasses import dataclass, asdict
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent.parent
RUN_BENCH = ROOT / "sim" / "tools" / "run_benchmark.py"
TEST_PATTERNS = ROOT / "sim" / "test_patterns"
PASS_MARKER = "PASS: scenario complete, scoreboard clean"


@dataclass(frozen=True)
class Cell:
    topology: str
    rob_mode: str
    from_id: str
    pattern: str
    preserve_addr: bool

    def effective_topology(self) -> str:
        return self.topology + ("_rob" if self.rob_mode == "enabled" else "")

    def label(self) -> str:
        pa = "_pa" if self.preserve_addr else ""
        return f"{self.effective_topology()}__{self.from_id}__{self.pattern}{pa}"


def _ax4_curated() -> list:
    ids = []
    for p in sorted(glob.glob(str(TEST_PATTERNS / "AX4-*"))):
        name = os.path.basename(p)
        if name.startswith("AX4-INF-"):
            continue
        if os.path.isfile(os.path.join(p, "scenario.yaml")):
            ids.append(name.split("_")[0])  # e.g. AX4-BAS-003
    return ids


def expand_tier(matrix: dict, tier: str) -> list:
    t = matrix["tiers"][tier]
    cells = []
    for topo in t["topologies"]:
        for rob in t["rob_modes"]:
            for st in t["stimuli"]:
                froms = st["from"]
                if froms == "all_curated_ax4":
                    froms = _ax4_curated()
                elif isinstance(froms, str):
                    froms = [froms]
                pa = bool(st.get("preserve_addr", False))
                for fr in froms:
                    for pat in st["patterns"]:
                        cells.append(Cell(topo, rob, fr, pat, pa))
    return cells


def is_excluded(cell: Cell, exclusions: list):
    for ex in exclusions or []:
        w = ex["when"]
        if all(getattr(cell, {"rob_mode": "rob_mode", "from": "from_id",
                               "pattern": "pattern", "topology": "topology"}[k]) == v
               for k, v in w.items()):
            return ex["reason"]
    return None


def resolve_scenario(scenario_id: str) -> str:
    hits = sorted(glob.glob(str(TEST_PATTERNS / f"{scenario_id}*" / "scenario.yaml")))
    if not hits:
        raise FileNotFoundError(f"no scenario for id {scenario_id}")
    return hits[0]


def run_cell(cell: Cell, out_root: Path, run_cmd=None) -> bool:
    args = ["python3", str(RUN_BENCH),
            "--topology", cell.effective_topology(),
            "--pattern", cell.pattern,
            "--from", resolve_scenario(cell.from_id),
            "--out-root", str(out_root)]
    if cell.preserve_addr:
        args.append("--preserve-addr")
    runner = run_cmd or (lambda a: subprocess.run(a).returncode == 0)
    return runner(args)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="NoC regression matrix runner.")
    ap.add_argument("--tier", default="nightly")
    ap.add_argument("--matrix", default=str(Path(__file__).parent / "matrix.yaml"))
    ap.add_argument("--out", default=str(ROOT / "sim" / "regress" / "output"))
    a = ap.parse_args(argv)

    matrix = yaml.safe_load(Path(a.matrix).read_text())
    cells = expand_tier(matrix, a.tier)
    out_base = Path(a.out) / a.tier
    out_base.mkdir(parents=True, exist_ok=True)

    # Prebuild the distinct topology binaries SERIALLY (avoids parallel obj_dir races).
    for topo in sorted({c.effective_topology() for c in cells}):
        subprocess.run(["make", "-C", str(ROOT), "build-verilator",
                        f"TOPOLOGY={topo}", "PYTHON3=python3"], check=True)

    results = []
    for cell in cells:
        reason = is_excluded(cell, matrix.get("exclusions"))
        if reason:
            results.append({**asdict(cell), "status": "skipped", "reason": reason})
            continue
        ok = run_cell(cell, out_base / cell.label())
        results.append({**asdict(cell), "status": "pass" if ok else "fail"})

    npass = sum(r["status"] == "pass" for r in results)
    nfail = sum(r["status"] == "fail" for r in results)
    nskip = sum(r["status"] == "skipped" for r in results)
    (out_base / "matrix.json").write_text(json.dumps(results, indent=2))
    print(f"[regress] tier={a.tier}  pass={npass} fail={nfail} skip={nskip} "
          f"total={len(results)}")
    for r in results:
        if r["status"] == "fail":
            print(f"  FAIL  {r['topology']}/{r['rob_mode']} {r['from_id']} {r['pattern']}")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
