# VC and Buffer Trade-off Report Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure the DAT VC-count and Router VC-buffer-depth trade-off under hardware-like outstanding-driven AI traffic and add one compact, interpretable comparison to the main performance report.

**Architecture:** Keep the RTL datapath and Mode 4 source behavior unchanged. Use the existing Makefile, result CSV, and report generator; add only an output-root override, Mode 4 result collection, and report rendering. Screen VC count at the shipped Router depth, refine buffer depth only for the two selected VC candidates, then confirm finalists across seeds.

**Tech Stack:** SystemVerilog/Verilator co-simulation, GNU Make, Python 3 standard library, pytest, specgen YAML constants.

**Spec:** `docs/superpowers/specs/2026-08-25-perf-report-standard.md`, with the approved AI-traffic structure recorded in `docs/superpowers/plans/2026-09-02-ai-inference-traffic.md` and the Mode 4 contract in `IMPLEMENTATION_PLAN.md`.

## Global Constraints

- Do not change the RTL datapath, VC allocator, routing, packet format, AXI protocol behavior, or Mode 0-3 behavior.
- Use `INJECTION_MODE=4`, 4x4 mesh, 4 KB AXI writes, 64 beats, burst length 63, 16 rounds, and `NOC_DAT_VC_MODE=SHARED`.
- Screening uses `DAT_NUM_VC={1,2,4,8}`, `NOC_ROUTER_VC_DEPTH=8`, `NOC_NI_DAT_RX_VC_DEPTH=8`, and source outstanding depth `{1,2,4,8,16,32}`.
- Screening traffic is Broadcast Global, Gather Global with root 0, and Many-to-Many. Existing VC=2/depth=8 rows are reusable only when all recorded settings match.
- Depth refinement applies `NOC_ROUTER_VC_DEPTH={16,32}` only to the performance winner and the cost winner from screening.
- Performance winner means highest geometric mean of the three mappings' normalized peak delivered payload bandwidth. Cost winner means the smallest `VC count x Router VC depth` reaching at least 95% of the performance winner. The user selects the final design from the Pareto candidates stated in the conclusion.
- Human-facing label format is `m4_<traffic>_v<VC>_b<router-depth>_o<outstanding>`. Use `bcg`, `gg0`, and `m2m` for the three traffic abbreviations. Seed is recorded in CSV and the parent path, never in the label.
- Output layout is `sim/verilator/output/vc_buffer/s<seed>/<run-label>/`. Preserve all existing output outside the validated `vc_buffer` subtree.
- Before every co-sim batch, remove generated filelists/tops and the resolved `/home/lucas/noc_build/verilator/obj_dir_*` targets. Restore `DAT_NUM_VC=2`, Router VC depth 8, and NI DAT RX depth 8 after the batch, then require `codegen.py --check` to pass.
- Report units remain explicit: cycles/transaction, B/cycle, flits/VC, entries/router-input, and percent.
- Do not claim area, power, or frequency results from the storage proxy. Those remain unavailable until synthesis.

---

### Task 1: Freeze output isolation and Mode 4 collection

**Files:**

- Modify: `sim/verilator/Makefile`
- Modify: `sim/tools/perf_report.py`
- Modify: `sim/tools/test_outstanding_injection_mode.py`
- Modify: `sim/tools/test_perf_report.py`

**Interfaces:**

- `OUTPUT_ROOT ?= output` controls only run artifacts; existing invocations remain unchanged.
- `SIM_TAG` remains overridable. VC/buffer runs pass the approved short label explicitly.
- `collect()` handles the existing continuous and directed rows only.
- `collect_vc_buffer()` recursively reads only `injection_mode=4` rows and returns their recorded VC, Router depth, NI depth, outstanding depth, seed, traffic mapping, completion latency, delivered payload bandwidth, link utilization, and checker evidence.

- [x] **Step 1: Add failing Makefile-path tests**

Assert that all run log, `perf.json`, and `result.csv` paths derive from `$(OUTPUT_ROOT)/$(SIM_TAG)`, while `OUTPUT_ROOT` defaults to `output`.

- [x] **Step 2: Run the focused test and verify RED**

```text
python -m pytest sim/tools/test_outstanding_injection_mode.py -q
```

Expected: FAIL because the Makefile currently hardcodes `output/$(SIM_TAG)`.

- [x] **Step 3: Implement the output-root override**

Add `OUTPUT_ROOT ?= output`, define one run-directory variable, and replace only the run recipe's repeated hardcoded paths. Do not change the existing default tags.

- [x] **Step 4: Add failing Mode 4 collection tests**

Create nested fixtures such as:

```text
vc_buffer/s1/m4_m2m_v4_b16_o8/result.csv
```

Require the ordinary report collector to ignore Mode 4 rows. Require the Mode 4 collector to reject missing configuration fields, non-positive depths, duplicate `(mapping, VC, Router depth, NI depth, outstanding, seed)` keys, and mismatched 4 KB/16-round geometry.

- [x] **Step 5: Implement the minimum collector split**

Filter by the CSV `injection_mode` field before offered-load parsing. Reuse `_integer()`, `_number()`, `_dat_link_utils()`, `mapping_label()`, and `USEFUL_RATIO`; do not create a second parser framework.

- [x] **Step 6: Run focused tests**

```text
python -m pytest sim/tools/test_outstanding_injection_mode.py sim/tools/test_perf_report.py -q
```

Expected: PASS.

---

### Task 2: Add the compact trade-off section

**Files:**

- Modify: `sim/tools/perf_report.py`
- Modify: `sim/tools/test_perf_report.py`
- Generate: `sim/verilator/output/perf_report.md`

**Interfaces:**

- The report explains that VC depth is FIFO capacity and current credit is remaining free space.
- Storage proxy:

```text
DAT buffer entries per Router input = DAT_NUM_VC x NOC_ROUTER_VC_DEPTH
Initial credits per VC = NOC_ROUTER_VC_DEPTH
```

- One results table uses one row per measured communication type and `(VC count, Router depth)` configuration at its selected outstanding point. Its four measured or derived result fields are delivered payload bandwidth, completion latency, busiest DAT-link utilization, and buffer entries per Router input.
- Detailed per-outstanding latency, bandwidth, and link utilization stay in CSV/figures rather than creating another wide Markdown table.
- Compute overlap coverage remains a separate workload-derived analysis. It must state the selected offered load and PE compute budget, and must not be presented as measured PE utilization.

- [x] **Step 1: Add failing report-contract tests**

Require the new heading, both formulas, short-label explanation, screening/refinement/confirmation flow, units, and one-sentence table interpretation. Require that seed does not appear in the displayed run label.

- [x] **Step 2: Run the report test and verify RED**

```text
python -m pytest sim/tools/test_perf_report.py -q
```

Expected: FAIL because the trade-off section is absent.

- [x] **Step 3: Render the approved method and result summary**

Append one `VC and Buffer Trade-off` section to the existing four-section AI report. When no complete sweep exists, render the method only and state that no selection has been made. Once data exists, compute per-mapping peaks, normalized values, performance winner, cost winner, and Pareto candidates directly from CSV rows. Put Pareto candidates only in the conclusion, not in the measurement table.

- [x] **Step 4: Regenerate and inspect the report**

```text
python sim/tools/perf_report.py sim/verilator/output
```

Verify that Sections 1-4 are unchanged except for any required Mode 4 isolation fix, Section 5 is concise, and no unmeasured value is printed as a result.

- [x] **Step 5: Run focused tests**

```text
python -m pytest sim/tools/test_perf_report.py -q
```

Expected: PASS.

---

### Task 3: Run screening and depth refinement

**Files:**

- Temporarily modify and restore: `specgen/source/constants.yaml`
- Generate: `sim/verilator/output/vc_buffer/`
- Preserve: all other `sim/verilator/output/` content

**Interfaces:**

- A hardware configuration is built once, then reused for all traffic/outstanding runs.
- Every run must finish with the non-vacuous scoreboard PASS and record the observed outstanding high-water mark.
- Screening contains 72 cells. The 18 existing VC=2/depth=8 cells may replace new cells only after exact field comparison.

- [x] **Step 1: Validate cleanup targets**

Resolve the repository and `/home/lucas/noc_build/verilator` paths. Remove only `sim/verilator/output/vc_buffer`, `sim/filelist_*.f`, `sim/tb/test/tb_top_*.sv`, `sim/tb/soc/tb_top_dma_*.sv`, and `/home/lucas/noc_build/verilator/obj_dir_*` after confirming each resolved path is inside its named parent.

- [x] **Step 2: Run the four screening builds**

For VC 1, 2, 4, and 8, set Router depth 8 and NI depth 8 in `constants.yaml`, regenerate, clean-build once, then run all six outstanding depths for `bcg`, `gg0`, and `m2m`. Use paths such as:

```text
OUTPUT_ROOT=output/vc_buffer/s1 SIM_TAG=m4_m2m_v4_b8_o8
```

- [x] **Step 3: Select the two refinement candidates**

Generate the intermediate report. Select the performance winner and cost winner using the Global Constraints definitions. Record the selected VC values and supporting numbers in `.planning/2026-09-03-vc-buffer-tradeoff/findings.md` before changing depth.

- [x] **Step 4: Run depth 16 and 32 only for the selected VCs**

For each selected VC and Router depth, run each traffic at its screening peak outstanding depth and at outstanding depth 32. If both are 32, run that cell once. This produces at most 24 new cells.

- [x] **Step 5: Restore shipped constants**

Restore VC 2, Router depth 8, and NI depth 8. Regenerate generated parameters and run:

```text
python specgen/tools/codegen.py --check
```

Expected: PASS with no specgen diff.

---

### Task 4: Confirm finalists and close the report

**Files:**

- Generate: `sim/verilator/output/vc_buffer/`
- Generate: `sim/verilator/output/perf_report.md`
- Modify: `docs/backlog.md`
- Modify: `IMPLEMENTATION_PLAN.md`

**Interfaces:**

- Confirmation repeats only Pareto finalists at their selected load points with seeds 1, 2, and 3.
- One finalist-only NI-depth check compares 8 and 16 at the same traffic/load/seed. It is labeled model sensitivity, not a production target recommendation.
- Final selection remains an owner decision from the Pareto rows; the report does not hide the performance/cost trade-off behind one opaque score.

- [x] **Step 1: Run confirmation seeds**

Use `output/vc_buffer/s1`, `s2`, and `s3`; keep identical short labels beneath each seed directory. Reject any row whose generated traffic metadata, transfer geometry, or hardware parameters differ.

- [x] **Step 2: Run the NI-depth sensitivity check**

For the highest-bandwidth finalist only, compare NI DAT RX depth 8 and 16 using Many-to-Many at the selected load and seed 1. Restore NI depth 8 immediately afterward.

- [x] **Step 3: Generate the final report**

```text
python sim/tools/perf_report.py sim/verilator/output
```

Verify that the report names the performance winner, cost winner, Pareto rows, seed spread, and NI sensitivity without claiming synthesis PPA.

- [x] **Step 4: Run final automated gates**

```text
python -m pytest sim/tools/test_outstanding_injection_mode.py sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py -q
python specgen/tools/codegen.py --check
git diff --check
```

Expected: PASS.

- [x] **Step 5: Run clean co-sim smoke gates**

After the mandatory generated-file and obj-dir pre-clean, run one 2x2 `neighbor` smoke and one 4x4 Mode 4 Many-to-Many smoke at the restored defaults. Expected: scoreboard PASS, non-vacuous traffic, and bounded outstanding high-water mark.

- [x] **Step 6: Record and close the round**

Add the measured decision and limitations to `docs/backlog.md`. Mark the auxiliary stages complete in `IMPLEMENTATION_PLAN.md`. List every `.md` referencing `DAT_NUM_VC`, `NOC_ROUTER_VC_DEPTH`, or the short-label grammar and reconcile any changed definition before committing exact paths only.

---

## Planned Commits

1. `test(sim): define VC buffer sweep reporting`
2. `feat(sim): isolate VC buffer sweep outputs`
3. `docs(perf): report VC buffer trade-off`

Each commit must pass its focused tests. Because the worktree already contains overlapping uncommitted Mode 4 changes, inspect and stage exact hunks only; do not use directory-wide `git add`.
