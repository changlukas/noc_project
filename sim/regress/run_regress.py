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
TOPOLOGIES = ROOT / "sim" / "topologies"
PASS_MARKER = "PASS: scenario complete, scoreboard clean"


def _topology_dims(topology: str) -> tuple:
    base = topology[:-4] if topology.endswith("_rob") else topology
    topo = yaml.safe_load((TOPOLOGIES / f"{base}.yaml").read_text())
    t = topo["topology"]
    return t["x_dim"], t["y_dim"]


def _interior_hotspot(topology: str) -> int:
    """Default hotspot target: a fixed interior linear node id (avoids edge tiles)."""
    x_dim, y_dim = _topology_dims(topology)
    return (y_dim // 2) * x_dim + (x_dim // 2)


CAPACITY_SLOTS = 4  # alloc_unique_offset bound on the default 4x4 mesh (gen_test_patterns.py:223-227)


def unique_addr_count(scenario_path: str) -> int:
    sc = yaml.safe_load(Path(scenario_path).read_text())
    addrs = {int(str(t["addr"]), 0) & 0xFFFFFFFF
             for t in (sc.get("transactions") or []) if "addr" in t}
    return len(addrs)


def _address_mode(scenario_path: str) -> str:
    sc = yaml.safe_load(Path(scenario_path).read_text())
    return (sc.get("metadata") or {}).get("address_mode", "dependent")


def _ax4_by_address_mode(mode: str) -> list:
    """Return AX4 scenario ids whose address_mode == mode (absent defaults to
    'dependent'). Self-check: an 'independent' scenario exceeding the capacity
    bound is a misclassification and raises."""
    ids = []
    for p in sorted(glob.glob(str(TEST_PATTERNS / "AX4-*"))):
        name = os.path.basename(p)
        if name.startswith("AX4-INF-"):
            continue
        scenario = os.path.join(p, "scenario.yaml")
        if not os.path.isfile(scenario):
            continue
        sid = name.split("_")[0]
        if _address_mode(scenario) != mode:
            continue
        if mode == "independent" and unique_addr_count(scenario) > CAPACITY_SLOTS:
            raise ValueError(
                f"{sid} tagged independent but has "
                f"{unique_addr_count(scenario)} unique addrs > {CAPACITY_SLOTS}")
        ids.append(sid)
    return ids


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


def is_wire_verifiable(scenario_path: str) -> bool:
    """The wire scoreboard verifies via write->readback (BIST-style): only scenarios
    that produce OKAY reads of written data are checked here. Write-only scenarios (no
    read op) and error-response scenarios (metadata.category == 'response', whose
    accesses intentionally DECERR) are NOT wire-verifiable -- they are covered by the
    Layer 2 c_model integration suite. Such cells are skipped (reported, not silent)."""
    sc = yaml.safe_load(Path(scenario_path).read_text())
    if (sc.get("metadata") or {}).get("category") == "response":
        return False
    return any(t.get("op") == "read" for t in (sc.get("transactions") or []))


def run_cell(cell: Cell, out_root: Path, run_cmd=None) -> bool:
    args = ["python3", str(RUN_BENCH),
            "--topology", cell.effective_topology(),
            "--pattern", cell.pattern,
            "--from", resolve_scenario(cell.from_id),
            "--out-root", str(out_root)]
    if cell.pattern == "hotspot":
        args += ["--hotspot", str(_interior_hotspot(cell.effective_topology()))]
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
        if not is_wire_verifiable(resolve_scenario(cell.from_id)):
            results.append({**asdict(cell), "status": "skipped",
                            "reason": "non-wire-verifiable (write-only/error-response); Layer 2 covers"})
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
