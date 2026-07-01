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
def _topology_dims(topology: str) -> tuple:
    base = topology[:-4] if topology.endswith("_rob") else topology
    topo = yaml.safe_load((TOPOLOGIES / f"{base}.yaml").read_text())
    t = topo["topology"]
    return t["x_dim"], t["y_dim"]


def _interior_hotspot(topology: str) -> int:
    """Default hotspot target: a fixed interior linear node id (avoids edge tiles)."""
    x_dim, y_dim = _topology_dims(topology)
    return (y_dim // 2) * x_dim + (x_dim // 2)


# Coverage gate on independent-mode scenarios: caps unique write addresses per
# scenario. Historically also the alloc_unique_offset window bound (16*4*0x40 =
# 0x1000); since alloc_unique_offset now auto-grows the window to hold disjoint
# slots this is only a coverage cap and could be raised (see gen_test_patterns.py
# _write_footprint / memory_size auto-grow).
CAPACITY_SLOTS = 4


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
    id_policy: str = ""    # "" = none; "round_robin:N" forwarded to the generator

    def effective_topology(self) -> str:
        return self.topology + ("_rob" if self.rob_mode == "enabled" else "")

    def label(self) -> str:
        pa = "_pa" if self.preserve_addr else ""
        idp = f"_id{self.id_policy.replace(':', '')}" if self.id_policy else ""
        return f"{self.effective_topology()}__{self.from_id}__{self.pattern}{pa}{idp}"


def expand(matrix: dict) -> list:
    cells = []
    for topo in matrix["topologies"]:
        for rob in matrix["rob_modes"]:
            for st in matrix["stimuli"]:
                froms = st["from"]
                if froms == "all_independent_ax4":
                    froms = _ax4_by_address_mode("independent")
                elif froms == "all_dependent_ax4":
                    froms = _ax4_by_address_mode("dependent")
                elif isinstance(froms, str):
                    froms = [froms]
                pa = bool(st.get("preserve_addr", False))
                ip = st.get("id_policy", "")
                for fr in froms:
                    for pat in st["patterns"]:
                        cells.append(Cell(topo, rob, fr, pat, pa, ip))
    return cells


def _match(cell: Cell, rules: list):
    """Return the reason of the first rule whose `when` fully matches, else None.
    Shared by exclusions and xfails (same when-key schema)."""
    attr = {"rob_mode": "rob_mode", "from": "from_id",
            "pattern": "pattern", "topology": "topology"}
    for r in rules or []:
        if all(getattr(cell, attr[k]) == v for k, v in r["when"].items()):
            return r["reason"]
    return None


def resolve_scenario(scenario_id: str) -> str:
    hits = sorted(glob.glob(str(TEST_PATTERNS / f"{scenario_id}*" / "scenario.yaml")))
    if not hits:
        raise FileNotFoundError(f"no scenario for id {scenario_id}")
    return hits[0]


def is_self_checking(scenario_path: str) -> bool:
    """The wire scoreboard verifies via write->readback (BIST-style): only scenarios
    that produce OKAY reads of written data are checked here. Write-only scenarios (no
    read op) and error-response scenarios (metadata.category == 'response', whose
    accesses intentionally DECERR) are NOT self-checking -- they are covered by the
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
    if cell.id_policy:
        args += ["--id-policy", cell.id_policy]
    runner = run_cmd or (lambda a: subprocess.run(a).returncode == 0)
    return runner(args)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="NoC regression matrix runner.")
    ap.add_argument("--build", default=None,
                    help="Run one build (e.g. mesh_4x4_vc1 or mesh_4x4_vc1_rob). "
                         "Omit to run every build.")
    ap.add_argument("--matrix", default=str(Path(__file__).parent / "matrix.yaml"))
    ap.add_argument("--out", default=str(ROOT / "sim" / "regress" / "output"))
    ap.add_argument("--dry-run", action="store_true",
                    help="Print the cell accounting and exit without running.")
    a = ap.parse_args(argv)

    matrix = yaml.safe_load(Path(a.matrix).read_text())
    cells = expand(matrix)
    if a.build:
        cells = [c for c in cells if c.effective_topology() == a.build]
        if not cells:
            sys.exit(f"[regress] no cells for BUILD={a.build}")

    # Classify every cell up front (no sim) for accounting.
    planned = []
    for cell in cells:
        reason = _match(cell, matrix.get("exclusions"))
        if reason:
            planned.append((cell, "excluded", reason))
        elif not is_self_checking(resolve_scenario(cell.from_id)):
            planned.append((cell, "skipped_self_check",
                            "non-self-checking (write-only/error-response); Layer 2 covers"))
        else:
            planned.append((cell, "run", None))

    n_raw = len(planned)
    n_excluded = sum(s == "excluded" for _, s, _ in planned)
    n_skip = sum(s == "skipped_self_check" for _, s, _ in planned)
    n_run = sum(s == "run" for _, s, _ in planned)
    print(f"[regress] build={a.build or 'ALL'}  raw={n_raw} excluded={n_excluded} "
          f"skipped_self_check={n_skip} run={n_run}")
    if a.dry_run:
        return 0

    out_base = Path(a.out) / (a.build or "all")
    out_base.mkdir(parents=True, exist_ok=True)

    for topo in sorted({c.effective_topology() for c, s, _ in planned if s == "run"}):
        subprocess.run(["make", "-C", str(ROOT), "build-verilator",
                        f"TOPOLOGY={topo}", "PYTHON3=python3"], check=True)

    results = []
    for cell, status, reason in planned:
        if status != "run":
            results.append({**asdict(cell), "status": status, "reason": reason})
            continue
        ok = run_cell(cell, out_base / cell.label())
        xfail_reason = _match(cell, matrix.get("xfails"))
        if ok:
            results.append({**asdict(cell), "status": "pass"})
        elif xfail_reason:
            results.append({**asdict(cell), "status": "xfail", "reason": xfail_reason})
        else:
            results.append({**asdict(cell), "status": "fail"})

    npass = sum(r["status"] == "pass" for r in results)
    nfail = sum(r["status"] == "fail" for r in results)
    nxfail = sum(r["status"] == "xfail" for r in results)
    (out_base / "matrix.json").write_text(json.dumps(results, indent=2))
    print(f"[regress] pass={npass} fail={nfail} xfail={nxfail} "
          f"excluded={n_excluded} skipped_self_check={n_skip} "
          f"(coverage denom = pass+fail+xfail = {npass + nfail + nxfail})")
    for r in results:
        if r["status"] == "fail":
            print(f"  FAIL  {r['topology']}/{r['rob_mode']} {r['from_id']} {r['pattern']}")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
