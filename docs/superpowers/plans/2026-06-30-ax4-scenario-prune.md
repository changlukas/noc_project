# AX4 scenario prune Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete 12 non-standard / duplicate AX4 scenarios, reclassify STR-003→ORD-003, renumber each family gap-free, update every hard-coded reference, then re-run the co-sim regression.

**Architecture:** Pure stimulus-set surgery. Scenario dirs live under `sim/test_patterns/AX4-*/`; the Layer-2 gtest list globs them (`sim/test_patterns/CMakeLists.txt`, `CONFIGURE_DEPENDS`), so deletes/renames auto-propagate there. Hard-coded ids in c_model integration tests, the regression runner, sim tools, and `matrix.yaml` must be swept by the old→new map below. No production C++ changes.

**Tech Stack:** git, bash/sed, Python (run_regress dry-run), GNU Make (sim-regress).

## Global Constraints

- Reply to the user in Traditional Chinese; keep technical terms in English.
- Commit format `type(scope): description` (English). Type here: `test` (scenario set is test stimulus) / `docs` (backlog).
- Do NOT push. Stop at the working tree for review.
- **Host GCC ICE:** this mingw64 host cannot build the c_model gtest tree (`make check` / full ctest hits `cfgcleanup.cc:580`). c_model integration-test ref edits are therefore verified **statically** (every scenario-id string resolves to an existing dir; no stale/deleted id remains), not by compile. The co-sim path (`make sim-regress`, header-only c_model via `build-verilator`) IS runnable here and is the live check.
- `make sim-regress` invoked with `PYTHON3=python3` (mingw64 python3), never `py -3`.

## Canonical old→new id map

Deletes happen before renames (rename targets are freed by the deletes).

**Delete (12):** `BAS-001`, `BAS-002`, `BAS-004`, `BUR-007`, `BUR-008`, `BUR-009`, `BND-002`, `EXC-004`, `HSH-001`, `HSH-002`, `STR-001`, `INF-001`.

**Reclassify (1):** `STR-003_multi_dst_stress` → `ORD-003_same_id_multi_dst` (`category: stress`→`ordering`; `address_mode` stays `dependent`).

**Renumber:**

| old (full dir name) | new (full dir name) |
|---|---|
| `AX4-BAS-003_single_write_read_aligned` | `AX4-BAS-001_single_write_read_aligned` |
| `AX4-BAS-005_multi_id_single_beat_sequential` | `AX4-BAS-002_multi_id_single_beat_sequential` |
| `AX4-BND-003_narrow_aligned_multibeat` | `AX4-BND-002_narrow_aligned_multibeat` |
| `AX4-BND-004_unaligned_start` | `AX4-BND-003_unaligned_start` |
| `AX4-BND-005_sparse_multibeat` | `AX4-BND-004_sparse_multibeat` |
| `AX4-BND-006_cross_4kb_auto_split` | `AX4-BND-005_cross_4kb_auto_split` |
| `AX4-BND-007_4kb_boundary_edges` | `AX4-BND-006_4kb_boundary_edges` |
| `AX4-STR-002_multi_outstanding_stress` | `AX4-STR-001_multi_outstanding_stress` |

`BND-001`, `BUR-001..006`, `EXC-001..003`, `ORD-001/002`, `QOS-001`, `RSP-001..003` unchanged.

Every id above appears as a **full-suffix string** in refs (e.g. `AX4-BND-003_narrow_aligned_multibeat`), which is collision-safe to substitute (suffixes are unique). The only **bare-form** refs (no suffix) are: `matrix.yaml` exclusions `AX4-BND-006` / `AX4-BND-007`; `test_perf_collector.cpp` `AX4-BAS-003`; `test_check_perf_parity.py` `AX4-BAS-005`. Handle those explicitly (Task 3).

---

### Task 1: Delete 12 noise scenarios + repoint the broken fixture

**Files:**
- Delete: 12 dirs under `sim/test_patterns/AX4-*/`
- Modify: `c_model/tests/wrap/CMakeLists.txt:49`, `sim/vcs/Makefile:191`

**Interfaces:**
- Produces: a scenario tree with 25 dirs (21 wire + QOS-001 + RSP-001/002/003), no dangling fixture ref.

- [ ] **Step 1: Delete the 12 dirs**

```bash
cd /e/05_NoC/noc_project
git rm -r \
  sim/test_patterns/AX4-BAS-001_single_write_no_read \
  sim/test_patterns/AX4-BAS-002_single_read_default_fill \
  sim/test_patterns/AX4-BAS-004_conformity_write_read \
  sim/test_patterns/AX4-BUR-007_wrap_len_2 \
  sim/test_patterns/AX4-BUR-008_wrap_len_4 \
  sim/test_patterns/AX4-BUR-009_wrap_len_16 \
  sim/test_patterns/AX4-BND-002_narrow_transfer_size2 \
  sim/test_patterns/AX4-EXC-004_exclusive_wrap_pair_success \
  sim/test_patterns/AX4-HSH-001_backpressure_retry \
  sim/test_patterns/AX4-HSH-002_conformity_backpressure \
  sim/test_patterns/AX4-STR-001_latency_stress \
  sim/test_patterns/AX4-INF-001_dpi_fatal_on_init_failure
```

- [ ] **Step 2: Repoint the wrap test fixture off deleted BAS-001**

`c_model/tests/wrap/CMakeLists.txt:49` currently:

```cmake
        "CMODEL_TEST_SCENARIO_YAML=${CMAKE_SOURCE_DIR}/../sim/test_patterns/AX4-BAS-001_single_write_no_read/scenario.yaml")
```

Change the path to the surviving single write+read scenario (still `BAS-003` at this point; Task 3's sweep renames it to `BAS-001`):

```cmake
        "CMODEL_TEST_SCENARIO_YAML=${CMAKE_SOURCE_DIR}/../sim/test_patterns/AX4-BAS-003_single_write_read_aligned/scenario.yaml")
```

Also remove the now-stale deleted-id mention in `sim/vcs/Makefile:191` (comment `# debugging. AX4-INF-001 fails by design.`) — drop the `AX4-INF-001 fails by design.` sentence (the scenario is gone).

- [ ] **Step 3: Static check — no surviving source references a deleted id**

Run (must print nothing; the `_dummy` parser fixture in `test_scenario_metadata.cpp` is the only allowed `BAS-001` token and is excluded):

```bash
grep -rn -E 'AX4-(BAS-001_single_write_no_read|BAS-002_single_read_default_fill|BAS-004_conformity_write_read|BUR-007|BUR-008|BUR-009|BND-002_narrow_transfer_size2|EXC-004|HSH-001|HSH-002|STR-001_latency_stress|INF-001)' \
  c_model/include c_model/tests sim/regress sim/tools sim/test_patterns/CMakeLists.txt \
  --include='*.cpp' --include='*.hpp' --include='*.py' --include='*.yaml' --include='*.txt' --include='*.in' \
  | grep -v 'AX4-BAS-001_dummy'
```

Expected: empty output.

- [ ] **Step 4: Co-sim dry-run still resolves**

Run: `python3 sim/regress/run_regress.py --build mesh_4x4_vc1 --dry-run`
Expected: prints a `[regress] build=mesh_4x4_vc1 raw=... run=...` line with no traceback; `run` count drops below 74 (deleted scenarios gone).

- [ ] **Step 5: Commit**

```bash
git add -A sim/test_patterns c_model/tests/wrap/CMakeLists.txt
git commit -m "test(ax4): delete 12 non-standard/duplicate scenarios, repoint wrap fixture"
```

---

### Task 2: Reclassify STR-003 → ORD-003

**Files:**
- Rename: `sim/test_patterns/AX4-STR-003_multi_dst_stress` → `AX4-ORD-003_same_id_multi_dst`
- Modify: that scenario's `scenario.yaml` (name + category)
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp` (lines 135, 474, 506, 529, 552)

**Interfaces:**
- Consumes: Task 1's tree.
- Produces: the multi-dst ordering scenario under its ORD id; all integration refs point to it.

- [ ] **Step 1: Rename the dir**

```bash
git mv sim/test_patterns/AX4-STR-003_multi_dst_stress \
       sim/test_patterns/AX4-ORD-003_same_id_multi_dst
```

- [ ] **Step 2: Update the scenario metadata**

In `sim/test_patterns/AX4-ORD-003_same_id_multi_dst/scenario.yaml`, change the `name` and `category`:

```yaml
metadata:
  name: AX4-ORD-003_same_id_multi_dst
  category: ordering
  address_mode: dependent
```

(Keep the existing transactions and any comment block; only `name` + `category` change. `address_mode` stays `dependent`.)

- [ ] **Step 3: Update the integration-test refs**

In `c_model/tests/integration/test_request_response_loopback.cpp`, replace every `AX4-STR-003_multi_dst_stress` with `AX4-ORD-003_same_id_multi_dst`. This covers the path substring at line 135 and the four `RequireKnownScenario("AX4-STR-003_multi_dst_stress")` calls (lines 474, 506, 529, 552):

```bash
sed -i 's/AX4-STR-003_multi_dst_stress/AX4-ORD-003_same_id_multi_dst/g' \
  c_model/tests/integration/test_request_response_loopback.cpp
```

- [ ] **Step 4: Static check — no STR-003 / multi_dst_stress token remains**

Run (must be empty):

```bash
grep -rn -E 'STR-003|multi_dst_stress' c_model sim --include='*.cpp' --include='*.hpp' --include='*.py' --include='*.yaml' --include='*.txt' | grep -v '/_data/'
```

Expected: empty.

- [ ] **Step 5: Dry-run resolves ORD-003**

Run: `python3 sim/regress/run_regress.py --build mesh_4x4_vc1 --dry-run`
Expected: no traceback (ORD-003 now in the dependent group, runs on neighbor preserve_addr).

- [ ] **Step 6: Commit**

```bash
git add -A sim/test_patterns c_model/tests/integration/test_request_response_loopback.cpp
git commit -m "test(ax4): reclassify STR-003 multi-dst as ORD-003 ordering scenario"
```

---

### Task 3: Renumber families gap-free + sweep all refs

**Files:**
- Rename: 8 scenario dirs (per old→new map)
- Modify: each renamed scenario's `scenario.yaml` `name`
- Modify (full-suffix sweep): `c_model/tests/integration/test_request_response_loopback.cpp`, `test_port_pair_loopback.cpp`, `test_router_loopback.cpp`; `c_model/tests/common/test_isolated_scenario.cpp`; `c_model/tests/wrap/CMakeLists.txt`; `sim/regress/test_run_regress.py`; `sim/tools/run_benchmark.py`, `gen_test_patterns.py`, `test_gen_test_patterns.py`, `test_run_benchmark.py`; `sim/verilator/Makefile`, `sim/vcs/Makefile` (`SCENARIO ?=` default)
- Modify (bare-form): `sim/regress/matrix.yaml`; `c_model/tests/common/test_perf_collector.cpp`; `sim/tools/test_check_perf_parity.py`; `sim/regress/README.md`
- Do NOT touch: `sim/tools/perf_baseline/*.json` — dead golden data (no reader; backlog ponytail-audit deletion candidate). Excluded from the static check below, not swept.
- `sim/regress/run_regress.py` hard-codes no scenario id (pure glob/expand) — no edit (codex-confirmed no-op).

**Interfaces:**
- Consumes: Tasks 1-2 tree.
- Produces: gap-free families (BAS 001-002, BND 001-006, STR 001); all refs consistent.

- [ ] **Step 1: Rename the 8 dirs**

```bash
cd /e/05_NoC/noc_project/sim/test_patterns
git mv AX4-BAS-003_single_write_read_aligned       AX4-BAS-001_single_write_read_aligned
git mv AX4-BAS-005_multi_id_single_beat_sequential AX4-BAS-002_multi_id_single_beat_sequential
git mv AX4-BND-003_narrow_aligned_multibeat        AX4-BND-002_narrow_aligned_multibeat
git mv AX4-BND-004_unaligned_start                 AX4-BND-003_unaligned_start
git mv AX4-BND-005_sparse_multibeat                AX4-BND-004_sparse_multibeat
git mv AX4-BND-006_cross_4kb_auto_split            AX4-BND-005_cross_4kb_auto_split
git mv AX4-BND-007_4kb_boundary_edges              AX4-BND-006_4kb_boundary_edges
git mv AX4-STR-002_multi_outstanding_stress        AX4-STR-001_multi_outstanding_stress
cd /e/05_NoC/noc_project
```

- [ ] **Step 2: Update the renamed scenarios' `name` (top-level + nested node fixtures)**

`BAS-003` and `BAS-005` each carry tracked nested `node0/scenario.yaml` + `node1/scenario.yaml` whose `name:` also hard-codes the old id; the others have none. Sweep the **full old id string recursively** over each renamed dir so top-level and nested fixtures all update (suffix unchanged, collision-safe):

```bash
sed -i 's/AX4-BAS-003_single_write_read_aligned/AX4-BAS-001_single_write_read_aligned/g' \
  $(grep -rl AX4-BAS-003_single_write_read_aligned sim/test_patterns/AX4-BAS-001_single_write_read_aligned)
sed -i 's/AX4-BAS-005_multi_id_single_beat_sequential/AX4-BAS-002_multi_id_single_beat_sequential/g' \
  $(grep -rl AX4-BAS-005_multi_id_single_beat_sequential sim/test_patterns/AX4-BAS-002_multi_id_single_beat_sequential)
sed -i 's/name: AX4-BND-003_/name: AX4-BND-002_/' sim/test_patterns/AX4-BND-002_narrow_aligned_multibeat/scenario.yaml
sed -i 's/name: AX4-BND-004_/name: AX4-BND-003_/' sim/test_patterns/AX4-BND-003_unaligned_start/scenario.yaml
sed -i 's/name: AX4-BND-005_/name: AX4-BND-004_/' sim/test_patterns/AX4-BND-004_sparse_multibeat/scenario.yaml
sed -i 's/name: AX4-BND-006_/name: AX4-BND-005_/' sim/test_patterns/AX4-BND-005_cross_4kb_auto_split/scenario.yaml
sed -i 's/name: AX4-BND-007_/name: AX4-BND-006_/' sim/test_patterns/AX4-BND-006_4kb_boundary_edges/scenario.yaml
sed -i 's/name: AX4-STR-002_/name: AX4-STR-001_/' sim/test_patterns/AX4-STR-001_multi_outstanding_stress/scenario.yaml
```

- [ ] **Step 3: Sweep full-suffix refs across all source/test/tool files**

Full-suffix ids are collision-safe (unique suffixes). Apply every mapping to every referencing file:

```bash
FILES="c_model/tests/integration/test_request_response_loopback.cpp
c_model/tests/integration/test_port_pair_loopback.cpp
c_model/tests/integration/test_router_loopback.cpp
c_model/tests/common/test_isolated_scenario.cpp
c_model/tests/wrap/CMakeLists.txt
sim/regress/test_run_regress.py
sim/tools/run_benchmark.py
sim/tools/gen_test_patterns.py
sim/tools/test_gen_test_patterns.py
sim/tools/test_run_benchmark.py
sim/verilator/Makefile
sim/vcs/Makefile"
for f in $FILES; do
  sed -i \
    -e 's/AX4-BAS-003_single_write_read_aligned/AX4-BAS-001_single_write_read_aligned/g' \
    -e 's/AX4-BAS-005_multi_id_single_beat_sequential/AX4-BAS-002_multi_id_single_beat_sequential/g' \
    -e 's/AX4-BND-003_narrow_aligned_multibeat/AX4-BND-002_narrow_aligned_multibeat/g' \
    -e 's/AX4-BND-004_unaligned_start/AX4-BND-003_unaligned_start/g' \
    -e 's/AX4-BND-005_sparse_multibeat/AX4-BND-004_sparse_multibeat/g' \
    -e 's/AX4-BND-006_cross_4kb_auto_split/AX4-BND-005_cross_4kb_auto_split/g' \
    -e 's/AX4-BND-007_4kb_boundary_edges/AX4-BND-006_4kb_boundary_edges/g' \
    -e 's/AX4-STR-002_multi_outstanding_stress/AX4-STR-001_multi_outstanding_stress/g' \
    "$f"
done
```

- [ ] **Step 4: Handle the bare-form refs**

`test_run_regress.py` also carries bare `AX4-BAS-005` / `AX4-BAS-003` and `AX4-BND-007` in `Cell(...)` constructors and asserts; the suffix sweep missed bare forms. Plus the three bare-only files. Apply bare maps (order BND-006→005 before BND-007→006 so the freed slot is not re-matched):

```bash
# regression runner unit test: bare ids in Cell() / asserts
sed -i -e 's/"AX4-BAS-003"/"AX4-BAS-001"/g' \
       -e 's/"AX4-BAS-005"/"AX4-BAS-002"/g' \
       -e 's/"AX4-BND-007"/"AX4-BND-006"/g' \
       -e 's/"AX4-STR-002"/"AX4-STR-001"/g' sim/regress/test_run_regress.py

# matrix exclusions (bare, order matters)
sed -i -e 's/AX4-BND-006/AX4-BND-005/g' sim/regress/matrix.yaml
sed -i -e 's/AX4-BND-007/AX4-BND-006/g' sim/regress/matrix.yaml
sed -i -e 's/from: AX4-BAS-005/from: AX4-BAS-002/g' sim/regress/matrix.yaml

# perf collector + perf-parity test (bare)
sed -i 's/"AX4-BAS-003"/"AX4-BAS-001"/g' c_model/tests/common/test_perf_collector.cpp
sed -i 's/"AX4-BAS-005"/"AX4-BAS-002"/g' sim/tools/test_check_perf_parity.py
```

Note: `gen_test_patterns.py:226` and `test_gen_test_patterns.py:117` mention `AX4-BAS-005` in **comments** only; the suffix sweep in Step 3 does not touch bare comment tokens. Update them for accuracy:

```bash
sed -i 's/AX4-BAS-005/AX4-BAS-002/g' sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns.py
```

The regression README (`sim/regress/README.md:106,130,131`) documents bare ids; update for accuracy (order BND-006 before BND-007):

```bash
sed -i -e 's/AX4-BAS-005/AX4-BAS-002/g' \
       -e 's/AX4-BND-006/AX4-BND-005/g' \
       -e 's/AX4-BND-007/AX4-BND-006/g' sim/regress/README.md
```

- [ ] **Step 5: Static check — no old id remains, gap-free dirs**

Patterns are **suffix-anchored** (e.g. `BND-006_cross`, not bare `BND-006`) because bare `BND-005`/`BND-006` are now valid renamed ids — a bare check would false-positive. The scan is path-scoped (no `--include`) so it reaches the extension-less Makefiles and the nested `node*/scenario.yaml`; `perf_baseline` is dead golden data and excluded.

```bash
# (a) no old full-id string survives anywhere actionable
grep -rn -E 'AX4-(BAS-003_single|BAS-005_multi|BND-003_narrow|BND-004_unaligned|BND-005_sparse|BND-006_cross|BND-007_4kb|STR-002_multi|STR-003_multi)' \
  c_model sim docs/backlog.md docs/development.md \
  | grep -vE '/(perf_baseline|_data|output|__pycache__|\.git)/'
# expected: empty

# (b) families are gap-free
ls sim/test_patterns | grep -oE 'AX4-(BAS|BND|STR)-[0-9]+' | sort -u
# expected: BAS-001 BAS-002  BND-001 BND-002 BND-003 BND-004 BND-005 BND-006  STR-001  (no gaps)

# (c) every matrix-exclusion id resolves to a real dir
for id in $(grep -oE 'AX4-[A-Z]{3}-[0-9]{3}' sim/regress/matrix.yaml | sort -u); do
  ls -d sim/test_patterns/${id}_* >/dev/null 2>&1 || echo "DANGLING exclusion id: $id"
done
# expected: no DANGLING lines
```

- [ ] **Step 6: Co-sim dry-run + build still good**

Run: `python3 sim/regress/run_regress.py --build mesh_4x4_vc1 --dry-run`
Expected: no traceback; exclusions now key on `AX4-BND-005` / `AX4-BND-006` and still match.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "test(ax4): renumber BAS/BND/STR families gap-free, sweep all id refs"
```

---

### Task 4: Update backlog

**Files:**
- Modify: `docs/backlog.md`

**Interfaces:**
- Consumes: the final id set.
- Produces: a backlog reflecting the hybrid prune (not the stale strict-15 plan), with the old→new map and the deferred slave-latency axis.

- [ ] **Step 1: Rewrite the prune sections**

In `docs/backlog.md`:
- Replace the "Next-session plan (2): prune scenario set to four shapes" + "Full cross" + "Resolved — BAS-005 drop" blocks with a short "AX4 scenario prune (done)" entry: hybrid standard ∩ marginal-value result, link `docs/superpowers/specs/2026-06-30-ax4-scenario-prune-design.md`, and the old→new id map (copy the table from this plan's Canonical map).
- Update the discovery-run rows and fabric-bug rows to renumbered ids: `BUR-002`/`BUR-003` unchanged; `STR-002`→`STR-001`; the 4KB carriers `BND-006`→`BND-005` (cross_4kb) and `BND-007`→`BND-006` (edges); note the rename so the old git/backlog references stay traceable.
- Add a one-line item under "Verification methodology gaps": **slave-latency testbench axis** — sweep a base scenario's `write_latency`/`read_latency` as a matrix axis (like `rob_modes`) if slave-backpressure coverage is wanted; do not reintroduce duplicate scenario files.
- Strike the matrix-harness H1 row (hotspot default target already landed in `run_regress.py:149`).

- [ ] **Step 2: Commit**

```bash
git add docs/backlog.md
git commit -m "docs(backlog): record AX4 hybrid prune, defer slave-latency axis"
```

---

### Task 5: Re-run regression + triage

**Files:** none (produces a triage note for the user; no commit unless matrix.yaml needs a new exclusion).

**Interfaces:**
- Consumes: the pruned tree.
- Produces: pass/fail accounting + the real-fabric-bug worklist.

- [ ] **Step 1: Run the build's full set**

Run: `make sim-regress BUILD=mesh_4x4_vc1 PYTHON3=python3`
Expected: builds `build-verilator` (no GCC ICE — sim is header-only), then runs every cell. Captures `[regress] pass=.. fail=.. xfail=..` and a `matrix.json`.

- [ ] **Step 2: Triage the fails**

Compare against the expected real-fabric-bug set: `BUR-003` (all patterns, non-rob), `BUR-002`@hotspot, `STR-001`(was STR-002)@neighbor, plus excluded `ORD-002`. The deleted `HSH-001` fail must be gone.
- Any fail in the expected set → real fabric bug, leave for a later debugging round (do not fix here).
- Any **new** fail outside the set → add to `matrix.yaml` exclusions with a reason (commit `docs/backlog.md` + `matrix.yaml`), per the discovery-run protocol.

- [ ] **Step 3: Report to the user**

Summarize pass/fail counts, confirm `HSH-001` gone, list the surviving fabric-bug worklist. Do not push.

---

## Self-Review

- **Spec coverage:** delete-12 → Task 1; STR-003→ORD-003 → Task 2; renumber + ref sweep (F2) → Task 3; wrap-CMakeLists repoint (F1) → Task 1 Step 2 + Task 3 sweep; backlog + slave-latency axis (out-of-scope) → Task 4; verification → Task 5. Layer-2 QOS/RSP untouched (kept on disk) — no task needed, correct.
- **Placeholder scan:** all id maps are concrete; every sed/grep command is exact. No TBD.
- **Type/id consistency:** the wrap fixture is set to `BAS-003` in Task 1 then swept to `BAS-001` in Task 3 (Step 3 `FILES` includes `c_model/tests/wrap/CMakeLists.txt`) — consistent final state `AX4-BAS-001_single_write_read_aligned`. Matrix exclusions end on `BND-005`/`BND-006`, matching the renamed 4KB dirs.
- **ICE caveat:** no step runs `make check` / full ctest; gtest ref edits gated by static resolution (Task 3 Step 5) — honest about what this host can verify.
