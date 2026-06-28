# Regression Coverage Round Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Widen the co-sim regression to a transaction x spatial cross plus multi-id and a non-square topology, on a per-build execution model, without fixing the excluded fabric bugs.

**Architecture:** Drive everything through the existing single-point runner `run_benchmark.py`. Classify AX4 scenarios by a capacity-aware `metadata.address_mode` tag, flatten `matrix.yaml` (flat), and make `run_regress.py` run one build per invocation (`BUILD=<build>`) or all builds when no `BUILD` is given. Add an emit-path-agnostic multi-id rewrite to `gen_test_patterns.py`. The VC axis (vc1/2/4/8) is kept complete on purpose.

**Tech Stack:** Python 3 (stdlib + PyYAML), GNU Make, Verilator, pytest.

**Spec:** `docs/superpowers/specs/2026-06-28-regression-coverage-round-design.md`

## Global Constraints

- Python tooling only this round. Do NOT modify `c_model/` (the id<->VC binding and fabric bugs are next round).
- Build / run with `PYTHON3=python3` (the mingw64 interpreter). Never `py -3`.
- Commit message format `type(scope): description` (English). Valid types: feat, fix, docs, test, refactor, chore.
- Do NOT push. Stop at the working tree for review.
- Verilator only. VCS regression is out of scope.
- Terminology: use `conformance`, `self-checking`, `xfail`, `base scenario`, `address-independent | address-dependent`. Do NOT introduce `smoke` / `nightly` / `tier`.
- Each commit compiles, passes existing tests, and includes tests for new behavior.
- Run a Python test with: `python3 -m pytest <path> -v`.

---

## File map

| File | Responsibility | Tasks |
|---|---|---|
| `sim/regress/run_regress.py` | matrix expand / exclude / classify / run / accounting | 1, 2, 3, 4, 7 |
| `sim/regress/test_run_regress.py` | runner unit tests | 1, 2, 3, 4, 7 |
| `sim/regress/matrix.yaml` | flat declarative matrix | 4, 7 |
| `sim/tools/gen_test_patterns.py` | id-policy rewrite | 6 |
| `sim/tools/test_gen_test_patterns.py` | generator unit tests | 6 |
| `sim/tools/run_benchmark.py` | `--id-policy` passthrough | 6 |
| `sim/topologies/mesh_2x4_vc1.yaml` | non-square topology | 5 |
| `Makefile` | `sim-regress` target -> `--build` | 3 |
| `sim/test_patterns/AX4-*/scenario.yaml` | `metadata.address_mode` tag | 2 |

---

## Task 1: H1 — hotspot cells pass a default target

**Files:**
- Modify: `sim/regress/run_regress.py` (`run_cell`, add `_topology_dims` / `_interior_hotspot`)
- Test: `sim/regress/test_run_regress.py`

**Interfaces:**
- Produces: `run_regress._interior_hotspot(topology: str) -> int`; `run_cell` now appends `--hotspot <id>` when `cell.pattern == "hotspot"`.

- [ ] **Step 1: Write the failing test**

Add to `sim/regress/test_run_regress.py`:

```python
def test_interior_hotspot_4x4():
    # 4x4 interior linear id = (y//2)*x + (x//2) = 2*4 + 2 = 10
    assert run_regress._interior_hotspot("mesh_4x4_vc1") == 10
    assert run_regress._interior_hotspot("mesh_4x4_vc8_rob") == 10  # _rob suffix stripped

def test_hotspot_cell_emits_default_target():
    cell = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-BAS-003", "hotspot", False)
    captured = {}
    run_regress.run_cell(cell, pathlib.Path("out"),
                         run_cmd=lambda a: (captured.setdefault("args", a), True)[1])
    assert "--hotspot" in captured["args"]
    idx = captured["args"].index("--hotspot")
    assert captured["args"][idx + 1] == "10"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest sim/regress/test_run_regress.py::test_interior_hotspot_4x4 -v`
Expected: FAIL with `AttributeError: module 'run_regress' has no attribute '_interior_hotspot'`

- [ ] **Step 3: Add the helpers and wire `run_cell`**

In `sim/regress/run_regress.py`, after the `TEST_PATTERNS` constant (near line 13) add:

```python
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
```

In `run_cell`, after building `args` and before the `preserve_addr` check, add:

```python
    if cell.pattern == "hotspot":
        args += ["--hotspot", str(_interior_hotspot(cell.effective_topology()))]
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m pytest sim/regress/test_run_regress.py -v`
Expected: PASS (all, incl. the two new tests)

- [ ] **Step 5: Commit**

```bash
git add sim/regress/run_regress.py sim/regress/test_run_regress.py
git commit -m "fix(regress): hotspot cells emit a default interior target (H1)"
```

---

## Task 2: Capacity-aware classification (`metadata.address_mode`)

**Files:**
- Modify: `sim/regress/run_regress.py` (add `unique_addr_count`, `_ax4_by_address_mode`)
- Modify: 15 `sim/test_patterns/AX4-*/scenario.yaml` (tag `address_mode: independent`)
- Test: `sim/regress/test_run_regress.py`

**Interfaces:**
- Produces: `run_regress.unique_addr_count(scenario_path: str) -> int`; `run_regress._ax4_by_address_mode(mode: str) -> list[str]` where `mode in {"independent","dependent"}`. A scenario tagged `independent` with >4 unique addresses raises `ValueError` (capacity self-check).

- [ ] **Step 1: Write the failing test**

Add to `sim/regress/test_run_regress.py`:

```python
def test_unique_addr_count():
    assert run_regress.unique_addr_count(
        run_regress.resolve_scenario("AX4-STR-002")) == 8
    assert run_regress.unique_addr_count(
        run_regress.resolve_scenario("AX4-BAS-003")) == 1

def test_str002_classified_dependent():
    # 8 unique addrs > 4-slot bound -> must NOT be in the independent set
    assert "AX4-STR-002" not in run_regress._ax4_by_address_mode("independent")
    assert "AX4-STR-002" in run_regress._ax4_by_address_mode("dependent")

def test_independent_set_all_fit_capacity():
    for sid in run_regress._ax4_by_address_mode("independent"):
        n = run_regress.unique_addr_count(run_regress.resolve_scenario(sid))
        assert n <= 4, f"{sid} has {n} unique addrs but is tagged independent"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest sim/regress/test_run_regress.py::test_unique_addr_count -v`
Expected: FAIL with `AttributeError: ... has no attribute 'unique_addr_count'`

- [ ] **Step 3: Add classification helpers**

In `sim/regress/run_regress.py`, add after `_interior_hotspot`:

```python
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
```

- [ ] **Step 4: Tag the address-independent scenarios**

Add `address_mode: independent` under `metadata:` in each of these `scenario.yaml` files (leave all others untagged so they default to `dependent`):

```
AX4-BAS-002, AX4-BAS-003, AX4-BAS-004, AX4-BAS-005,
AX4-BUR-001, AX4-BUR-002, AX4-BUR-003, AX4-BUR-004,
AX4-EXC-001, AX4-EXC-002, AX4-EXC-003,
AX4-HSH-001, AX4-HSH-002,
AX4-ORD-001, AX4-STR-001
```

Each edit looks like (example `AX4-BAS-003`):

```yaml
metadata:
  name: AX4-BAS-003_single_write_read_aligned
  category: basic
  address_mode: independent
```

- [ ] **Step 5: Run tests; let the self-check catch any misclassification**

Run: `python3 -m pytest sim/regress/test_run_regress.py -v`
Expected: PASS. If `test_independent_set_all_fit_capacity` fails for some `AX4-X`, that scenario has >4 unique addrs — remove its `address_mode: independent` tag (it stays `dependent`) and re-run.

- [ ] **Step 6: Commit**

```bash
git add sim/regress/run_regress.py sim/regress/test_run_regress.py sim/test_patterns/AX4-*/scenario.yaml
git commit -m "feat(regress): capacity-aware address_mode classification"
```

---

## Task 3: Per-build execution model + accounting

**Files:**
- Modify: `sim/regress/run_regress.py` (drop tier, add `--build`, flat expand, status enum, dry-run accounting)
- Modify: `sim/regress/test_run_regress.py` (flat matrix fixture)
- Modify: `Makefile` (`sim-regress` -> `--build`)

**Interfaces:**
- Produces: `run_regress.expand(matrix: dict) -> list[Cell]` (replaces `expand_tier`); `run_regress.main(["--build","mesh_4x4_vc1"])`; `--dry-run` prints accounting without running. matrix.json cell `status in {pass, fail, xfail, excluded, skipped_self_check}`.

- [ ] **Step 1: Write the failing test (flat expand + build filter)**

Replace the `MATRIX` fixture and `expand_tier` tests in `sim/regress/test_run_regress.py` with:

```python
MATRIX = {
    "topologies": ["mesh_4x4_vc1", "mesh_4x4_vc4"],
    "rob_modes": ["disabled", "enabled"],
    "stimuli": [
        {"from": "AX4-BAS-003", "patterns": ["neighbor", "hotspot"]},
        {"from": ["AX4-BUR-003"], "patterns": ["neighbor"], "preserve_addr": True},
    ],
    "exclusions": [{"when": {"rob_mode": "enabled", "from": "AX4-BUR-003"},
                    "reason": "len256>ROB"}],
}

def test_expand_counts_cells():
    cells = run_regress.expand(MATRIX)
    # 2 topo x 2 rob x (2 + 1) = 12
    assert len(cells) == 12

def test_build_filter():
    cells = [c for c in run_regress.expand(MATRIX)
             if c.effective_topology() == "mesh_4x4_vc1"]
    assert cells and all(c.effective_topology() == "mesh_4x4_vc1" for c in cells)

def test_is_xfail_matches():
    xfails = [{"when": {"from": "AX4-ORD-002"}, "reason": "known multi-id hang"}]
    hit = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-ORD-002", "neighbor", False)
    miss = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-BAS-003", "neighbor", False)
    assert run_regress.is_xfail(hit, xfails) == "known multi-id hang"
    assert run_regress.is_xfail(miss, xfails) is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest sim/regress/test_run_regress.py::test_expand_counts_cells -v`
Expected: FAIL with `AttributeError: ... has no attribute 'expand'`

- [ ] **Step 3: Replace `expand_tier` with flat `expand`**

In `sim/regress/run_regress.py`, replace the `expand_tier` function with:

```python
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
                for fr in froms:
                    for pat in st["patterns"]:
                        cells.append(Cell(topo, rob, fr, pat, pa))
    return cells
```

(Note: `expand` gains an `id_policy` field in Task 6; this version is replaced there.)

- [ ] **Step 3b: Rename `is_wire_verifiable` -> `is_self_checking`**

In `sim/regress/run_regress.py`, rename `def is_wire_verifiable(scenario_path)` to
`def is_self_checking(scenario_path)` (body unchanged). In `sim/regress/test_run_regress.py`, rename
the existing `test_wire_verifiable_filter` to `test_self_checking_filter` and update its three
`run_regress.is_wire_verifiable(...)` calls to `run_regress.is_self_checking(...)`. `main` (Step 4)
already calls `is_self_checking`.

Run: `python3 -m pytest sim/regress/test_run_regress.py -k self_checking -v`
Expected: PASS

- [ ] **Step 4: Rewrite `main` for per-build + accounting**

Replace `main` in `sim/regress/run_regress.py` with:

```python
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
        reason = is_excluded(cell, matrix.get("exclusions"))
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
        xfail_reason = is_xfail(cell, matrix.get("xfails"))
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
```

Add the `is_xfail` helper next to `is_excluded`:

```python
def is_xfail(cell: Cell, xfails: list):
    for ex in xfails or []:
        w = ex["when"]
        if all(getattr(cell, {"rob_mode": "rob_mode", "from": "from_id",
                              "pattern": "pattern", "topology": "topology"}[k]) == v
               for k, v in w.items()):
            return ex["reason"]
    return None
```

- [ ] **Step 5: Point the Makefile target at `--build`**

In `Makefile`, replace the `sim-regress` recipe (line ~190) with:

```makefile
sim-regress:
	$(TOOLPATH) $(PYTHON3) sim/regress/run_regress.py $(if $(BUILD),--build $(BUILD))
```

And add near the other vars (line ~182):

```makefile
BUILD ?=
```

(Use `BUILD` not `TB` so the `TB ?= mesh_4x4_vc1` default for `make sim` does not force a build here. `make sim-regress` runs all builds; `make sim-regress BUILD=mesh_4x4_vc1` runs one.)

Also update the help line (Makefile ~39): replace the `sim-regress TIER=nightly` line with:

```makefile
	@echo "  make sim-regress [BUILD=<build>]    run the co-sim regression (one build, or all)"
```

- [ ] **Step 6: Run tests**

Run: `python3 -m pytest sim/regress/test_run_regress.py -v`
Expected: PASS (note: this task removes the old `expand_tier` tests; the new `expand`/`build` tests pass).

- [ ] **Step 7: Commit**

```bash
git add sim/regress/run_regress.py sim/regress/test_run_regress.py Makefile
git commit -m "feat(regress): per-build execution model with cell accounting"
```

---

## Task 4: Flatten matrix.yaml and populate the cross

**Files:**
- Modify: `sim/regress/matrix.yaml`
- Test: `sim/regress/test_run_regress.py`

**Interfaces:**
- Consumes: `expand`, `_ax4_by_address_mode` (Tasks 2-3).

- [ ] **Step 1: Write the failing test (real matrix expands)**

Add to `sim/regress/test_run_regress.py`:

```python
import pathlib as _pl
def test_real_matrix_expands():
    m = run_regress.yaml.safe_load(
        (_pl.Path(run_regress.__file__).parent / "matrix.yaml").read_text())
    cells = run_regress.expand(m)
    # independent set runs 4 patterns; dependent set runs neighbor only
    assert any(c.pattern == "uniform_random" for c in cells)
    assert any(c.pattern == "neighbor" and c.preserve_addr for c in cells)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest sim/regress/test_run_regress.py::test_real_matrix_expands -v`
Expected: FAIL (current matrix.yaml is still tier-shaped / has no independent group)

- [ ] **Step 3: Rewrite matrix.yaml flat**

Replace `sim/regress/matrix.yaml` with:

```yaml
# Flat co-sim regression matrix (flat). Run one build with
# `make sim-regress BUILD=<build>`, or all builds with `make sim-regress`.
# all_independent_ax4 / all_dependent_ax4 expand from metadata.address_mode.
topologies: [mesh_4x4_vc1, mesh_4x4_vc2, mesh_4x4_vc4, mesh_4x4_vc8]
rob_modes:  [disabled, enabled]
stimuli:
  - {from: all_independent_ax4, patterns: [neighbor, uniform_random, transpose, hotspot]}
  - {from: all_dependent_ax4,   patterns: [neighbor], preserve_addr: true}
exclusions:
  - {when: {rob_mode: enabled, from: AX4-BUR-003},
     reason: "burst len 256 > ROB_CAPACITY 32"}
  - {when: {from: AX4-ORD-002},
     reason: "pre-existing multi-id concurrent-write co-sim hang (backlog); re-include when fixed"}
  - {when: {from: AX4-BND-006},
     reason: "pre-existing NMU 4KB read-split hang under 16-node load (backlog); re-include when fixed"}
```

- [ ] **Step 4: Run tests**

Run: `python3 -m pytest sim/regress/test_run_regress.py -v`
Expected: PASS

- [ ] **Step 5: Sanity-check the accounting (no sim)**

Run: `python3 sim/regress/run_regress.py --build mesh_4x4_vc1 --dry-run`
Expected: a line `[regress] build=mesh_4x4_vc1 raw=... excluded=... skipped_self_check=... run=...` with non-zero `run`.

- [ ] **Step 6: Commit**

```bash
git add sim/regress/matrix.yaml sim/regress/test_run_regress.py
git commit -m "feat(regress): flat matrix with transaction x spatial cross"
```

---

## Task 5: Non-square 2x4 topology proof

**Files:**
- Create: `sim/topologies/mesh_2x4_vc1.yaml`
- Test: shell proof (generator + build)

**Interfaces:**
- Consumes: `gen_tb_top.py`, `gen_test_patterns.py` (existing N x M support).

- [ ] **Step 1: Create the 2x4 YAML**

The topology YAML has NO node list — the generator derives all nodes from `x_dim * y_dim`
(`gen_test_patterns.py` `_load_topology`, `gen_tb_top.py:108-122`). Create
`sim/topologies/mesh_2x4_vc1.yaml` with exactly these four keys (8 nodes = 4 x 2):

```yaml
topology:
  name: mesh_2x4_vc1
  x_dim: 4
  y_dim: 2
  num_vc: 1
```

- [ ] **Step 2: Prove the generator builds it (neighbor, no transpose)**

`mesh_2x4_vc1` is intentionally not in `matrix.yaml.topologies`, so prove the generator + build
directly through the single-point runner:

Run: `python3 sim/tools/run_benchmark.py --topology mesh_2x4_vc1 --pattern neighbor --from sim/test_patterns/AX4-BAS-003_single_write_read_aligned/scenario.yaml`
Expected: build succeeds, generator emits node0..node7, sim prints `PASS: scenario complete, scoreboard clean`.

- [ ] **Step 3: Confirm transpose is correctly rejected**

Run: `python3 sim/tools/gen_test_patterns.py --pattern transpose --topology mesh_2x4_vc1 --out /tmp/t --from sim/test_patterns/AX4-BAS-003_single_write_read_aligned/scenario.yaml`
Expected: non-zero exit citing the `x_dim == y_dim` guard (`gen_test_patterns.py:329`).

- [ ] **Step 4: Commit**

```bash
git add sim/topologies/mesh_2x4_vc1.yaml
git commit -m "test(regress): non-square mesh_2x4 generator + build proof"
```

---

## Task 6: Multi-id stimulus (`--id-policy`)

**Files:**
- Modify: `sim/tools/gen_test_patterns.py` (add `--id-policy`, `_rewrite_ids`, call after base load)
- Modify: `sim/tools/run_benchmark.py` (forward `--id-policy`)
- Modify: `sim/regress/run_regress.py` (`Cell.id_policy`, `expand`, `run_cell` forward)
- Modify: `sim/regress/matrix.yaml` (multi-id stimulus)
- Test: `sim/tools/test_gen_test_patterns.py`, `sim/regress/test_run_regress.py`

**Interfaces:**
- Produces: `gen_test_patterns._rewrite_ids(base_sc: dict, n_ids: int) -> None` — in-place; assigns AXI id round-robin across `n_ids`, same unique base address keeps one id (W/R pair preserved). `Cell` gains `id_policy: str = ""`, threaded through `expand` -> `run_cell` -> `run_benchmark --id-policy`.

- [ ] **Step 1: Write the failing test**

Add to `sim/tools/test_gen_test_patterns.py`:

```python
def test_rewrite_ids_preserves_pairs():
    import gen_test_patterns as g
    base = {"transactions": [
        {"op": "write", "addr": 0x1000, "id": 0},
        {"op": "read",  "addr": 0x1000, "id": 0},
        {"op": "write", "addr": 0x1040, "id": 0},
        {"op": "read",  "addr": 0x1040, "id": 0},
    ]}
    g._rewrite_ids(base, 2)
    t = base["transactions"]
    # same addr -> same id (W/R pair stays on one id)
    assert t[0]["id"] == t[1]["id"]
    assert t[2]["id"] == t[3]["id"]
    # round-robin across 2 ids -> the two pairs differ
    assert t[0]["id"] != t[2]["id"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py::test_rewrite_ids_preserves_pairs -v`
Expected: FAIL with `AttributeError: ... has no attribute '_rewrite_ids'`

- [ ] **Step 3: Implement `_rewrite_ids` and the CLI**

In `sim/tools/gen_test_patterns.py`, add the helper (near `_emit_base_driven_node`):

```python
def _rewrite_ids(base_sc, n_ids):
    """In-place: assign AXI id round-robin across n_ids, grouped by unique base
    address so each write+read pair stays on a single id (emit-path-agnostic;
    both _emit_node and _emit_base_driven_node inherit by deep-copy)."""
    addr_to_id = {}
    next_slot = 0
    for t in base_sc.get("transactions", []):
        oa = _as_int(t["addr"]) & 0xFFFFFFFF
        if oa not in addr_to_id:
            addr_to_id[oa] = next_slot % n_ids
            next_slot += 1
        t["id"] = addr_to_id[oa]
```

Add the CLI argument (in `main`, near `--seed`):

```python
    ap.add_argument("--id-policy", default=None,
                    help="round_robin:N — rewrite base AXI ids round-robin across "
                         "N ids (W/R pair preserved). Default: keep base ids.")
```

After the base scenario is loaded in EACH pattern branch that reads `base_sc` (neighbor `:618`, transpose `:642`, random `:677`), apply the rewrite. To keep it DRY, add this helper and call it right after each `base_sc = yaml.safe_load(...)`:

```python
def _apply_id_policy(base_sc, id_policy):
    if base_sc is None or not id_policy:
        return
    kind, _, n = id_policy.partition(":")
    if kind != "round_robin" or not n.isdigit() or int(n) < 1:
        raise SystemExit(f"--id-policy: unsupported value {id_policy!r} "
                         f"(expected round_robin:N)")
    _rewrite_ids(base_sc, int(n))
```

Call `_apply_id_policy(base_sc, a.id_policy)` immediately after each `base_sc = yaml.safe_load(f)` in the three branches.

- [ ] **Step 4: Run tests**

Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py -v`
Expected: PASS

- [ ] **Step 5: Forward `--id-policy` from run_benchmark**

While editing this file, fix the stale term in the existing `--preserve-addr` help text
(`run_benchmark.py:147-148`) so it reads "AX4 conformance cells".

In `sim/tools/run_benchmark.py`, add the argparse line (near `--preserve-addr`, line ~147):

```python
    ap.add_argument("--id-policy", default=None,
                    help="Forward to gen_test_patterns --id-policy (round_robin:N).")
```

And in the `gen_args` build (after the `--preserve-addr` block, line ~213):

```python
    if a.id_policy:
        gen_args += ["--id-policy", a.id_policy]
```

- [ ] **Step 6: Write the failing wiring test**

Add to `sim/regress/test_run_regress.py`:

```python
def test_id_policy_forwarded_and_labeled():
    cell = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-BAS-005",
                            "neighbor", False, "round_robin:4")
    assert "round_robin4" in cell.label()
    captured = {}
    run_regress.run_cell(cell, pathlib.Path("out"),
                         run_cmd=lambda a: (captured.setdefault("args", a), True)[1])
    assert "--id-policy" in captured["args"]
    assert captured["args"][captured["args"].index("--id-policy") + 1] == "round_robin:4"
```

- [ ] **Step 7: Wire `id_policy` through the regression**

Without this the rewrite never reaches a matrix cell (it would stay a CLI-only feature). In
`sim/regress/run_regress.py`, add the `id_policy` field to `Cell` and update `label`:

```python
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
```

In `expand` (from Task 3), read `id_policy` from the stimulus and pass it to `Cell`:

```python
                pa = bool(st.get("preserve_addr", False))
                ip = st.get("id_policy", "")
                for fr in froms:
                    for pat in st["patterns"]:
                        cells.append(Cell(topo, rob, fr, pat, pa, ip))
```

In `run_cell`, forward it (after the `preserve_addr` block):

```python
    if cell.id_policy:
        args += ["--id-policy", cell.id_policy]
```

In `sim/regress/matrix.yaml`, add a multi-id stimulus under `stimuli:` (BAS-005 carries 4 W/R pairs;
`round_robin:4` makes them concurrent different-id on one neighbor dst):

```yaml
  - {from: AX4-BAS-005, patterns: [neighbor], id_policy: round_robin:4}
```

- [ ] **Step 8: Run tests**

Run: `python3 -m pytest sim/tools/test_gen_test_patterns.py sim/regress/test_run_regress.py -v`
Expected: PASS

- [ ] **Step 9: Commit**

```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns.py sim/tools/run_benchmark.py sim/regress/run_regress.py sim/regress/matrix.yaml sim/regress/test_run_regress.py
git commit -m "feat(regress): multi-id round-robin id-policy wired into the matrix"
```

---

## Task 7: xfail status + BND-007 exclusion

**Files:**
- Modify: `sim/regress/matrix.yaml` (add `xfails:` block + BND-007 exclusion)
- Test: `sim/regress/test_run_regress.py`

**Interfaces:**
- Consumes: `is_xfail` (Task 3).

- [ ] **Step 1: xfail policy (mechanism already tested in Task 3)**

`is_xfail` and the `xfail` status branch landed in Task 3 (`test_is_xfail_matches`); no duplicate unit
test here. Policy: an `xfails:` entry marks a cell that RUNS and may fail without reddening the run —
for a known-fail that fails FAST (a mismatch). It is NOT for hangs: ORD-002 / BND-006 / BND-007 drain
to the 100k-cycle timeout, so they stay in `exclusions` (not run at all). The multi-id discovery cell
(Task 6) graduates to `xfails` ONLY if the first full run shows it fails fast; if it hangs it moves to
`exclusions`. This round ships no `xfails` entry by design — the mechanism is in place and unit-tested,
ready for the discovery run.

- [ ] **Step 2: Write the failing exclusion test**

Add to `sim/regress/test_run_regress.py`:

```python
def test_bnd007_excluded():
    m = run_regress.yaml.safe_load(
        (_pl.Path(run_regress.__file__).parent / "matrix.yaml").read_text())
    cell = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-BND-007", "neighbor", True)
    assert run_regress.is_excluded(cell, m["exclusions"])
```

Run: `python3 -m pytest sim/regress/test_run_regress.py::test_bnd007_excluded -v`
Expected: FAIL (BND-007 not yet in `matrix.yaml`)

- [ ] **Step 3: Add BND-007 exclusion to matrix.yaml**

In `sim/regress/matrix.yaml`, append under `exclusions:`:

```yaml
  - {when: {from: AX4-BND-007},
     reason: "same 4KB-boundary class as BND-006, unconfirmed (backlog); re-check on first full run"}
```

- [ ] **Step 4: Run tests + accounting**

Run: `python3 -m pytest sim/regress/test_run_regress.py -v`
Run: `python3 sim/regress/run_regress.py --build mesh_4x4_vc1 --dry-run`
Expected: tests PASS (incl. `test_bnd007_excluded`); the dry-run `excluded` count rises by the BND-007 cells.

- [ ] **Step 5: Update the backlog**

In `docs/backlog.md`, move BND-007 from "re-check" to the excluded set (mirror the BND-006 row) and note it is now in `matrix.yaml`.

- [ ] **Step 6: Commit**

```bash
git add sim/regress/matrix.yaml sim/regress/test_run_regress.py docs/backlog.md
git commit -m "feat(regress): xfail status and BND-007 exclusion"
```

---

## Self-review notes

- **Spec coverage:** WI-0 -> Task 1; WI-0b + WI-A classification -> Task 2; WI-A accounting + WI-B -> Task 3; WI-1 -> Task 4; WI-3 -> Task 5; WI-2 -> Task 6 (incl. matrix wiring: `Cell.id_policy` + stimulus, not CLI-only); WI-4 -> Task 7 (mechanism + BND-007; ships no `xfails` entry by design — fabric bugs hang, so they `exclude`). All spec work items map to a task.
- **xfail wiring:** `is_xfail` is defined AND unit-tested in Task 3 (`test_is_xfail_matches`); Task 7 documents the policy and tests BND-007 exclusion. No duplicate test.
- **Terminology:** no `smoke`/`nightly`/`tier` introduced. `is_self_checking` (renamed from `is_wire_verifiable`), `--build`/`BUILD`, `--dry-run`, `address_mode`, `xfail`, `skipped_self_check`, `--id-policy round_robin:N` used throughout.
- **Deferred (not in any task, by spec):** allocator variant (b) memory-size bump; transpose for the dependent group; SVA/covergroup/CRV; injection-rate sweep.
- **First full run is a discovery run:** sweeping the cross through the 16-node fabric may surface new pre-existing co-sim bugs. Add each to `matrix.yaml` exclusions with a reason as confirmed (per `docs/backlog.md`).
```
