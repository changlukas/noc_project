# NoC co-sim regression matrix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A declarative matrix sweeps the wire-level mesh co-sim across topology x rob x stimulus and gates each cell on the scoreboard PASS marker.

**Architecture:** Declarative `sim/regress/matrix.yaml` + thin `sim/regress/run_regress.py` that shells out to the existing one-cell runner `sim/tools/run_benchmark.py` (topology + `--from` + `--pattern`). Two stages: (1) make ROB a generated build axis (`gen_tb_top --rob` from the `_rob` name suffix) and add an opt-in `--preserve-addr` mode so address-sensitive AX4 scenarios survive the neighbor spread; (2) the matrix runner + `make sim-regress`.

**Tech Stack:** Python 3 (existing sim tooling), GNU Make, Verilator, pytest, YAML.

**Spec:** `docs/superpowers/specs/2026-06-27-regression-matrix-design.md`

## Global Constraints

- Run python via mingw64: pass `PYTHON3=python3` to every `make`; invoke scripts with `python3`. Never `py -3`.
- Before any `make`/`pytest` in Git Bash: `export PATH="$PATH:/c/Windows/System32"` (gtest/subprocess discovery).
- VCS (`sim/vcs/`) is Linux-only; cannot build on this Windows host. VCS Makefile edits are verified by inspection/grep, not a local build.
- `ADDR_DST_SHIFT = 32` (`gen_test_patterns.py:77`): the dst tile lives in `addr[63:32]`, the local offset in `addr[31:0]`.
- The DUT-agnostic gate string is exactly `PASS: scenario complete, scoreboard clean` (`run_benchmark.py:33`).
- New files live in `sim/regress/`. Per-run output goes under `sim/regress/output/` (already gitignored by `sim/*/output/`).
- Commit messages: `type(scope): description` (English), ending with the Co-Authored-By trailer. Branch `feature/regression-matrix`. Do not push.

---

### Task 1: `--preserve-addr` stimulus mode

Add an opt-in mode that keeps each transaction's original local offset (only OR-ing the dst tile into `addr[63:32]`) instead of reallocating offsets, so address-sensitive AX4 scenarios (OOB / 4KB-boundary) keep their condition. The default reallocation path is unchanged.

**Files:**
- Modify: `sim/tools/gen_test_patterns.py` (`_emit_node` signature + addr line; argparse; 2 call sites)
- Modify: `sim/tools/run_benchmark.py` (argparse + forward to gen_test_patterns)
- Test: `sim/tools/test_gen_test_patterns.py`, `sim/tools/test_run_benchmark.py`

**Interfaces:**
- Produces: `gen_test_patterns.py` CLI flag `--preserve-addr` (store_true) and `_emit_node(..., preserve_addr=False)`. `run_benchmark.py` CLI flag `--preserve-addr` forwarded downstream. Task 3 sets it per cell from `matrix.yaml`.

- [ ] **Step 1: Write the failing test (gen_test_patterns preserve-addr)**

In `sim/tools/test_gen_test_patterns.py` add (use the file's existing import of the module under test and any tmp-dir helper it already uses):
```python
def test_preserve_addr_keeps_local_offset(tmp_path):
    # A base scenario whose transaction sits at a non-realloc local offset (e.g. OOB 0x2000).
    base = {"config": {"memory_base": 0, "memory_size": 0x1000},
            "transactions": [{"op": "write", "addr": 0x2000, "id": 1, "len": 0, "size": 2,
                              "burst": "INCR", "data_file": "d.txt"}]}
    out = tmp_path / "node0"
    # dst tile cid=5, src cid=0; preserve_addr keeps 0x2000, ORs cid into addr[63:32].
    gen_test_patterns._emit_node(base, str(tmp_path), str(out), 0, 5, 0,
                                 16, 0, 0x1000, preserve_addr=True)
    import yaml
    sc = yaml.safe_load((out / "scenario.yaml").read_text())
    assert sc["transactions"][0]["addr"] == (5 << 32) + 0x2000

def test_default_reallocates_offset(tmp_path):
    base = {"config": {"memory_base": 0, "memory_size": 0x1000},
            "transactions": [{"op": "write", "addr": 0x2000, "id": 1, "len": 0, "size": 2,
                              "burst": "INCR", "data_file": "d.txt"}]}
    out = tmp_path / "node0"
    gen_test_patterns._emit_node(base, str(tmp_path), str(out), 0, 5, 0,
                                 16, 0, 0x1000, preserve_addr=False)
    import yaml
    sc = yaml.safe_load((out / "scenario.yaml").read_text())
    # default path reallocates -> low 32 bits are NOT the original 0x2000.
    assert (sc["transactions"][0]["addr"] & 0xFFFFFFFF) != 0x2000
```

- [ ] **Step 2: Run it to verify it fails**

Run: `export PATH="$PATH:/c/Windows/System32"; python3 -m pytest sim/tools/test_gen_test_patterns.py -k preserve_addr -v`
Expected: FAIL — `_emit_node() got an unexpected keyword argument 'preserve_addr'`.

- [ ] **Step 3: Implement preserve_addr in `_emit_node`**

In `sim/tools/gen_test_patterns.py`, change the signature at line 361-362:
```python
def _emit_node(base_sc, src_dir, out_dir, src_idx, dst_cid, src_cid,
               n_nodes, base_local, memory_size, preserve_addr=False):
```
And the address assignment at lines 393-396:
```python
    for t in sc.get("transactions", []):
        orig = _as_int(t["addr"]) & 0xFFFFFFFF
        local_off = orig if preserve_addr else pair_offset[orig]
        t["addr"] = (dst_cid << ADDR_DST_SHIFT) + local_off
```

- [ ] **Step 4: Add the CLI flag + thread it to both call sites**

In `sim/tools/gen_test_patterns.py` `main()` argparse (near line 583, beside `--exclude-self`):
```python
    ap.add_argument("--preserve-addr", action="store_true",
                    help="Keep each transaction's original local offset (OR dst tile into "
                         "addr[63:32]) instead of reallocating; safe only with bijective "
                         "patterns like neighbor.")
```
At both `_emit_node(...)` call sites (lines 624 and 651), add the trailing kwarg:
```python
                       memory_size, preserve_addr=a.preserve_addr)
```

- [ ] **Step 5: Run the gen_test_patterns tests to verify they pass**

Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py -v`
Expected: PASS (the two new tests plus all existing ones).

- [ ] **Step 6: Write the failing test (run_benchmark forwards the flag)**

`sim/tools/test_run_benchmark.py` currently imports only `summarize` (line ~14) and has NO subprocess mocking. Add a module import at the top of the file (`import run_benchmark`, matching how `summarize` is reached) and the test below, which stubs `subprocess.run` + `_find_exe` + `_node_count` so no real sim runs:
```python
def test_preserve_addr_forwarded(monkeypatch):
    captured = {}
    def fake_run(args, **kw):
        if "gen_test_patterns.py" in " ".join(map(str, args)):
            captured["gen"] = list(map(str, args))
        class R: returncode = 0; stdout = "PASS: scenario complete, scoreboard clean"; stderr = ""
        return R()
    monkeypatch.setattr(run_benchmark.subprocess, "run", fake_run)
    monkeypatch.setattr(run_benchmark, "_find_exe", lambda t: __import__("pathlib").Path("Vtb_top"))
    monkeypatch.setattr(run_benchmark, "_node_count", lambda t: 1)
    run_benchmark.main(["--topology", "mesh_4x4_vc1", "--pattern", "neighbor",
                        "--preserve-addr", "--out-root", "x"])
    assert "--preserve-addr" in captured["gen"]
```
(Adapt the monkeypatch targets to whatever `test_run_benchmark.py` already stubs — match its existing style; the assertion is the point.)

- [ ] **Step 7: Run it to verify it fails**

Run: `python3 -m pytest sim/tools/test_run_benchmark.py -k preserve_addr -v`
Expected: FAIL — `unrecognized arguments: --preserve-addr`.

- [ ] **Step 8: Implement the run_benchmark flag + forward**

In `sim/tools/run_benchmark.py` argparse (after `--exclude-self`, near line 145):
```python
    ap.add_argument("--preserve-addr", action="store_true",
                    help="Forward to gen_test_patterns --preserve-addr (AX4 conformity cells).")
```
After the `--exclude-self` forward (line 207-208):
```python
    if a.preserve_addr:
        gen_args.append("--preserve-addr")
```

- [ ] **Step 9: Run both test files to verify they pass**

Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py sim/tools/test_run_benchmark.py -v`
Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add sim/tools/gen_test_patterns.py sim/tools/run_benchmark.py \
        sim/tools/test_gen_test_patterns.py sim/tools/test_run_benchmark.py
git commit -m "feat(sim): add --preserve-addr stimulus mode for AX4 conformity cells

Keep each transaction's original local offset (OR dst tile into addr[63:32]) so
OOB/4KB-boundary AX4 scenarios survive the neighbor spread; default reallocation
path unchanged. run_benchmark forwards the flag to gen_test_patterns.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: ROB generated axis

Make `{rob_disabled, rob_enabled}` a generated build axis for all 4 vc counts via the `_rob` name suffix, sourced from one base YAML per vc count. Retire the hand-copied `mesh_4x4_vc2_rob.yaml`.

**Files:**
- Modify: `sim/tools/gen_tb_top.py` (load base YAML for `_rob` names; derive `rob_enabled` from the suffix)
- Modify: `sim/tools/run_benchmark.py` (`_load_topology` strips `_rob` for the YAML path)
- Modify: `sim/tools/gen_test_patterns.py` (`_load_topology` strips `_rob` for the topology-name path)
- Modify: `sim/build_config.mk` (`TOPOLOGY_BASE` for the YAML dependency), `sim/verilator/Makefile`, `sim/vcs/Makefile`
- Delete: `sim/topologies/mesh_4x4_vc2_rob.yaml`, `sim/filelist_mesh_4x4_vc2_rob.f`
- Modify: `sim/test_patterns/README.md` (drop the `mesh_4x4_vc2_rob` YAML-topology description)

**Interfaces:**
- Produces: requesting `TOPOLOGY=mesh_4x4_vc<N>_rob` builds a ROB-enabled tb from `mesh_4x4_vc<N>.yaml`; all three Python tools resolve `<name>_rob` to the base `<name>.yaml` for topology data while keeping the full name for obj-dir/binary. Task 3 builds `mesh_4x4_vc<N>_rob` for rob-enabled cells.

- [ ] **Step 1: Add a shared base-name helper + strip in the three loaders**

In each of the three tools add a one-liner where the YAML path is built (`gen_tb_top.py:54`, `run_benchmark.py:41`, `gen_test_patterns.py:272`). Example for `gen_tb_top.py:52-55`:
```python
def load_topology(name: str) -> dict:
    import yaml
    base = name[:-4] if name.endswith("_rob") else name
    path = ROOT / "sim" / "topologies" / f"{base}.yaml"
    topo = yaml.safe_load(path.read_text())
    _check_flit_capacity(topo, path)
    return topo
```
`run_benchmark.py:40-42`:
```python
def _load_topology(topology: str) -> dict:
    base = topology[:-4] if topology.endswith("_rob") else topology
    topo_path = ROOT / "sim" / "topologies" / f"{base}.yaml"
    return yaml.safe_load(topo_path.read_text())
```
`gen_test_patterns.py:270-272` (the name->path branch only):
```python
        here = os.path.dirname(os.path.abspath(__file__))
        base = name[:-4] if name.endswith("_rob") else name
        topo_path = os.path.join(here, "..", "topologies", f"{base}.yaml")
```

- [ ] **Step 2: Derive `rob_enabled` from the requested name in gen_tb_top**

`requested_name` is NOT currently in scope at the ROB decision (`gen_tb_top.py:380` reads the YAML key, and the emit function only has the loaded `topo`). Thread it down:
1. Change the emit function's signature (the function containing line 380, which is called from `main()`) to accept `requested_name`, e.g. `def emit_tb_top(topo, requested_name):`.
2. In `main()`, pass the `--topology` CLI value at the call site: `emit_tb_top(topo, a.topology)`.
3. Replace the YAML-key read at line 380 with:
```python
    rob_enabled = requested_name.endswith("_rob")
```
The `_ex` ctor selection downstream is already gated on `rob_enabled` (imports at `gen_tb_top.py:455-458`, create at `:507`), so suffix-derivation flows through unchanged.

- [ ] **Step 3: Build smoke — all 8 variants verilate, rob picks the `_ex` ctor**

Run (build two representative variants, one base + one rob, to keep it quick):
```
export PATH="$PATH:/c/Windows/System32"
make build-verilator TOPOLOGY=mesh_4x4_vc4 PYTHON3=python3
make build-verilator TOPOLOGY=mesh_4x4_vc4_rob PYTHON3=python3
grep -c cmodel_nmu_create_ex sim/sv/tb_top_mesh_4x4_vc4_rob.sv
grep -c cmodel_nmu_create_ex sim/sv/tb_top_mesh_4x4_vc4.sv
```
Expected: both build; the `_rob` tb has `cmodel_nmu_create_ex` (>0), the base tb has 0.

- [ ] **Step 4: Wire `TOPOLOGY_BASE` through the Makefiles**

In `sim/build_config.mk`, ensure `TOPOLOGY_BASE = $(TOPOLOGY:_rob=)` exists (it is already used to pick `noc_types_pkg`). In `sim/verilator/Makefile:87` and `sim/vcs/Makefile:100`, change the topology-YAML dependency from `$(TOPOLOGY).yaml` to `$(TOPOLOGY_BASE).yaml` so the `.yaml` prerequisite resolves for `_rob` variants. Keep the full `$(TOPOLOGY)` for obj-dir, tb name, and the `.topology` stamp.
Run: `make build-verilator TOPOLOGY=mesh_4x4_vc8_rob PYTHON3=python3`
Expected: builds with no "no rule to make `mesh_4x4_vc8_rob.yaml`" error.

- [ ] **Step 5: Retire the hand-copied rob topology + sync docs**

```bash
git rm sim/topologies/mesh_4x4_vc2_rob.yaml          # tracked -> git rm
rm -f sim/filelist_mesh_4x4_vc2_rob.f                # gitignored generated (.gitignore:42) -> plain rm
```
In `sim/test_patterns/README.md` (lines ~93-97), remove the lines documenting `mesh_4x4_vc2_rob` as a YAML-controlled topology / `_ex` ctor path (it is now generated from the suffix).
Re-verify the previously-hand-copied variant still builds from the base:
Run: `make build-verilator TOPOLOGY=mesh_4x4_vc2_rob PYTHON3=python3`
Expected: builds from `mesh_4x4_vc2.yaml`, no missing-yaml error.

- [ ] **Step 6: ctest + default build unaffected**

Run: `make test PYTHON3=python3`
Expected: ctest 0 failed (rob axis touches only sim build plumbing, not c_model). `make check` still builds default `mesh_4x4_vc1`.

- [ ] **Step 7: Commit**

```bash
git add -A sim/tools sim/build_config.mk sim/verilator/Makefile sim/vcs/Makefile \
        sim/test_patterns/README.md
git commit -m "feat(sim): make ROB a generated build axis via _rob name suffix

gen_tb_top derives rob_enabled from the requested topology suffix and loads the base
YAML; run_benchmark/gen_test_patterns/Makefiles resolve <name>_rob to <name>.yaml.
Retire hand-copied mesh_4x4_vc2_rob.yaml + filelist.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `matrix.yaml` + `run_regress.py`

The declarative matrix and the thin runner that expands it, applies exclusions, runs each cell via `run_benchmark.py`, and aggregates results.

**Files:**
- Create: `sim/regress/matrix.yaml`, `sim/regress/run_regress.py`, `sim/regress/test_run_regress.py`

**Interfaces:**
- Consumes: `run_benchmark.py` CLI (`--topology --pattern --from --preserve-addr --out-root`) from Tasks 1-2; `mesh_4x4_vc<N>_rob` builds from Task 2.
- Produces: `expand_tier(matrix, tier) -> list[Cell]`, `resolve_scenario(scenario_id) -> str`, `is_excluded(cell, exclusions) -> str|None`, `run_cell(cell, out_root, run_cmd=...) -> bool`, `main(argv) -> int`. `Cell` is a dataclass with `topology, rob_mode, from_id, pattern, preserve_addr`. Task 4's `make sim-regress` calls `main`.

- [ ] **Step 1: Write `matrix.yaml`**

`sim/regress/matrix.yaml`:
```yaml
tiers:
  smoke:
    topologies: [mesh_4x4_vc1, mesh_4x4_vc2]
    rob_modes: [disabled]
    stimuli:
      - from: AX4-BAS-003
        patterns: [neighbor]
      - from: [AX4-ORD-002, AX4-RSP-002]   # ORD = multi-id; RSP-002 = OOB decerr (preserve-addr)
        patterns: [neighbor]
        preserve_addr: true
    # cell count = 2 topo x 1 rob x (1 + 2) = 6
  nightly:
    topologies: [mesh_4x4_vc1, mesh_4x4_vc2, mesh_4x4_vc4, mesh_4x4_vc8]
    rob_modes: [disabled, enabled]
    stimuli:
      - from: AX4-BAS-003
        patterns: [neighbor, uniform_random, transpose, hotspot]
      - from: all_curated_ax4      # AX4-* minus AX4-INF-*
        patterns: [neighbor]
        preserve_addr: true
exclusions:
  - when: {rob_mode: enabled, from: AX4-BUR-003}
    reason: "burst len 256 > ROB_CAPACITY 32"
```

- [ ] **Step 2: Write the failing tests (expansion, exclusion, scenario resolution)**

`sim/regress/test_run_regress.py`:
```python
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_regress

MATRIX = {
    "tiers": {"t": {"topologies": ["mesh_4x4_vc1", "mesh_4x4_vc4"],
                    "rob_modes": ["disabled", "enabled"],
                    "stimuli": [{"from": "AX4-BAS-003", "patterns": ["neighbor", "hotspot"]},
                                {"from": ["AX4-BUR-003"], "patterns": ["neighbor"],
                                 "preserve_addr": True}]}},
    "exclusions": [{"when": {"rob_mode": "enabled", "from": "AX4-BUR-003"},
                    "reason": "len256>ROB"}],
}

def test_expand_counts_cells():
    cells = run_regress.expand_tier(MATRIX, "t")
    # 2 topo x 2 rob x (2 patterns for BAS-003 + 1 for BUR-003) = 2*2*3 = 12
    assert len(cells) == 12

def test_rob_topology_suffix():
    cells = run_regress.expand_tier(MATRIX, "t")
    rob_cell = next(c for c in cells if c.rob_mode == "enabled")
    assert rob_cell.effective_topology().endswith("_rob")

def test_exclusion_marks_reason():
    cells = run_regress.expand_tier(MATRIX, "t")
    ex = [c for c in cells
          if run_regress.is_excluded(c, MATRIX["exclusions"])]
    # only the rob-enabled x BUR-003 cells (2 topo) are excluded
    assert len(ex) == 2
    assert all(run_regress.is_excluded(c, MATRIX["exclusions"]) == "len256>ROB" for c in ex)

def test_preserve_addr_flag_carried():
    cells = run_regress.expand_tier(MATRIX, "t")
    assert any(c.preserve_addr for c in cells)
    assert any(not c.preserve_addr for c in cells)
```

- [ ] **Step 3: Run it to verify it fails**

Run: `export PATH="$PATH:/c/Windows/System32"; python3 -m pytest sim/regress/test_run_regress.py -v`
Expected: FAIL — `No module named 'run_regress'`.

- [ ] **Step 4: Implement `run_regress.py`**

`sim/regress/run_regress.py`:
```python
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
```

- [ ] **Step 5: Run the unit tests to verify they pass**

Run: `python3 -m pytest sim/regress/test_run_regress.py -v`
Expected: PASS (4 tests).

- [ ] **Step 6: Commit**

```bash
git add sim/regress/matrix.yaml sim/regress/run_regress.py sim/regress/test_run_regress.py
git commit -m "feat(sim): regression matrix declaration + runner

matrix.yaml declares tiers/axes/exclusions; run_regress.py expands, applies
exclusions, prebuilds topology binaries serially, runs each cell via run_benchmark.py
with a unique out-root, gates on the PASS marker, writes matrix.json.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: wire-verifiable filter + `make sim-regress` + end-to-end smoke

**Design correction (from Task 4 e2e findings):** the wire scoreboard verifies via write->readback (BIST-style) -- it counts only OKAY reads of written data (`scoreboard.hpp:86`), and skips DECERR/SLVERR (`:29`, `:65`). So write-only and error-response AX4 scenarios produce zero verified reads and trip the non-vacuous PASS guard while NOT actually being checked. These belong to the Layer 2 c_model integration suite, not the wire matrix. The matrix therefore AUTO-SKIPS non-wire-verifiable scenarios (a finding, surfaced in `make check`'s console output, not silently dropped). Separately, `AX4-ORD-002` (multi-id concurrent write) hits a PRE-EXISTING co-sim hang (reproduces without preserve_addr) -- it is excluded with a backlog reason.

**Files:**
- Modify: `sim/regress/run_regress.py` (add `is_wire_verifiable` filter + apply in `main`), `sim/regress/test_run_regress.py` (filter test), `sim/regress/matrix.yaml` (smoke tier + ORD-002 exclusion)
- Modify: root `Makefile` (add `sim-regress` target + help line)

**Interfaces:**
- Consumes: `Cell`, `resolve_scenario`, `main` from Task 3.
- Produces: `is_wire_verifiable(scenario_path) -> bool`.

- [ ] **Step 1: Write the failing test for the wire-verifiable filter**

In `sim/regress/test_run_regress.py` add (uses the real `sim/test_patterns/` tree):
```python
def test_wire_verifiable_filter():
    rsp_read = run_regress.resolve_scenario("AX4-RSP-001")   # category: response (decerr read)
    rsp_write = run_regress.resolve_scenario("AX4-RSP-002")  # write-only OOB (0 reads)
    data = run_regress.resolve_scenario("AX4-BAS-003")       # write+read data scenario
    assert run_regress.is_wire_verifiable(rsp_read) is False
    assert run_regress.is_wire_verifiable(rsp_write) is False
    assert run_regress.is_wire_verifiable(data) is True
```
Run: `export PATH="$PATH:/c/Windows/System32"; python3 -m pytest sim/regress/test_run_regress.py -k wire_verifiable -v`
Expected: FAIL — `module 'run_regress' has no attribute 'is_wire_verifiable'`.

- [ ] **Step 2: Implement the filter and apply it in `main`**

Add to `sim/regress/run_regress.py`:
```python
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
```
In `main`, inside the per-cell loop, AFTER the exclusion check and BEFORE `run_cell`:
```python
        if not is_wire_verifiable(resolve_scenario(cell.from_id)):
            results.append({**asdict(cell), "status": "skipped",
                            "reason": "non-wire-verifiable (write-only/error-response); Layer 2 covers"})
            continue
```
Run: `python3 -m pytest sim/regress/test_run_regress.py -v`
Expected: PASS (5 tests now).

- [ ] **Step 3: Update matrix.yaml — smoke tier + ORD-002 exclusion**

Replace the `smoke` tier and extend `exclusions` in `sim/regress/matrix.yaml`:
```yaml
  smoke:
    topologies: [mesh_4x4_vc1, mesh_4x4_vc2]
    rob_modes: [disabled]
    stimuli:
      - from: AX4-BAS-003
        patterns: [neighbor]
      - from: [AX4-BND-006]          # read-bearing + 4KB-boundary: exercises preserve_addr
        patterns: [neighbor]
        preserve_addr: true
      - from: [AX4-RSP-002]          # write-only OOB: auto-skipped (demonstrates the filter)
        patterns: [neighbor]
        preserve_addr: true
    # expected: pass=4 (BAS-003 + BND-006, x2 topo), skip=2 (RSP-002), fail=0
```
```yaml
exclusions:
  - when: {rob_mode: enabled, from: AX4-BUR-003}
    reason: "burst len 256 > ROB_CAPACITY 32"
  - when: {from: AX4-ORD-002}
    reason: "pre-existing multi-id concurrent-write co-sim hang (backlog); re-include when fixed"
```

- [ ] **Step 4: Add the `sim-regress` Make target**

In the root `Makefile`, add (near the `sim:` target) and register in `.PHONY`:
```make
.PHONY: sim-regress
sim-regress:
	$(TOOLPATH) python3 sim/regress/run_regress.py --tier $(TIER)
```
Add `TIER ?= nightly` near the other `?=` defaults, and a `help:` echo line:
```make
	@echo "  make sim-regress TIER=nightly            run the co-sim regression matrix"
```

- [ ] **Step 5: End-to-end smoke tier runs green**

Run:
```
export PATH="$PATH:/c/Windows/System32"
make sim-regress TIER=smoke PYTHON3=python3
```
Expected: `pass=4 fail=0 skip=2` and `sim/regress/output/smoke/matrix.json` written. The two `AX4-RSP-002` cells are `skipped` (non-wire-verifiable); `AX4-BAS-003` and `AX4-BND-006` cells `pass`. The `BND-006` pass confirms preserve_addr carries the 4KB-boundary offset through the fabric meaningfully. If `BND-006` FAILS, STOP and report BLOCKED with the run log — it is a real fabric/preserve_addr finding, not something to work around.

- [ ] **Step 6: Commit**

```bash
git add sim/regress/run_regress.py sim/regress/test_run_regress.py sim/regress/matrix.yaml Makefile
git commit -m "feat(sim): wire-verifiable filter + make sim-regress + e2e smoke

Auto-skip non-wire-verifiable AX4 (write-only / error-response category) since the
wire scoreboard checks via write->readback (BIST-style); those stay at Layer 2.
Exclude AX4-ORD-002 (pre-existing multi-id-write co-sim hang, backlog). Wire
run_regress into Make; smoke tier green (pass=4 skip=2) incl. a read-bearing
4KB-boundary cell exercising preserve_addr.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:** Stage 1 rob axis -> Task 2; `--preserve-addr` -> Task 1; matrix.yaml + run_regress + reporting -> Task 3; `make sim-regress` + e2e -> Task 4. The `_rob` resolution across the 3 Python loaders + 2 Makefiles, the `all_curated_ax4` INF exclusion, the `AX4-BUR-003` rob exclusion, unique `--out-root`, serial prebuild, and `matrix.json`+console (no JUnit) are all covered. Deferred items (multi-id stimulus, per-id binding re-eval, JUnit) are correctly absent from the tasks.

**Placeholder scan:** every code step shows real code; every run step has an expected result. No TBD/TODO. The one "adapt to existing test style" note (Task 1 Step 6) names the exact assertion so it is not a placeholder.

**Type consistency:** `Cell(topology, rob_mode, from_id, pattern, preserve_addr)` + `effective_topology()`/`label()` are defined in Task 3 Step 4 and used consistently in its tests (Step 2) and `run_cell`/`main`. `--preserve-addr` is the same flag name across gen_test_patterns, run_benchmark (Task 1), and run_regress (Task 3). `effective_topology()` appends `_rob`, matching Task 2's suffix convention.
