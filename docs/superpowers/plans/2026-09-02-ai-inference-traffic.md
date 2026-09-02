# AI Inference Traffic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add pure, parameterized AI inference traffic patterns and one-round completion measurement for a 4x4 NoC performance report.

**Architecture:** Keep the existing BookSim-style patterns unchanged and add separate AI-semantic patterns that emit write-only traffic. Reuse the existing file-master format and destination allocator after sizing it from the expanded per-source destination lists. Extend the testbench non-vacuity check so an intentionally idle root or terminal pipeline stage is legal only when its loaded stimulus is empty. Measure end-to-end `AXI write round completion time` on a synchronized directed round; use the continuous sweep only for latency, delivered bandwidth, and DAT-link utilization.

**Tech Stack:** Python 3 generators and pytest, SystemVerilog testbench, GNU Make, Verilator co-simulation, Markdown report generation.

**Spec:** `.planning/2026-09-02-ai-mapping-survey/findings.md` (`Approved mapping decisions`)

## Global Constraints

- Scope is AI inference traffic only. Do not add gradient traffic or claim AllReduce support.
- Keep `neighbor`, `all_to_all`, `multicast`, and `many_to_many` behavior compatible with existing regressions.
- Add AI-facing CLI patterns: `broadcast`, `gather`, `alltoall`, `neighbor_exchange`, and `pipeline`.
- Display existing `many_to_many` as `Regional Exchange`; keep its CLI name for compatibility.
- All AI patterns are write-only. Completion means every expected B response returned.
- `STIM_SIZE`, `BURST_LEN`, and round count remain inputs. The generator must not hardcode 4 KB.
- Global Gather defaults to corner `ROOT_NODE=0`; root selection remains parameterized.
- Local Gather uses leaders 5, 6, 9, and 10 on the approved 4x4 mapping.
- `Neighbor Exchange` uses valid west/east/north/south neighbors without wraparound.
- `Pipeline P2P` is steady-state forward traffic on the row-snake path; it is not single-tensor dependent forwarding.
- `AXI write round completion time` is measured only on a synchronized directed round in which all source data is ready before issue. Its boundary is reset-release issue start through the final expected B response, so it includes source AXI issue, NI, NoC, destination handling, and the B return path. Do not label it as NoC-only or loaded-background time.
- Do not add a dependency or change a generated protocol parameter.
- Before every commit, run `python specgen/tools/codegen.py --check`, `make build`, `make test`, `make pytest`, `make -C sim/verilator hello`, and the clean 2x2 `neighbor` smoke required by `docs/backlog.md`. Commit only after all gates pass.
- At execution start, inspect the existing root `IMPLEMENTATION_PLAN.md`. Do not overwrite an active unrelated campaign; ask the user whether to merge or wait. Once available, mirror these four stage statuses there, update `docs/backlog.md` when the round closes, and delete the root campaign plan after every stage is complete.

---

## Stage 1: Idle-source completion contract

Goal: Permit intentionally idle AI participants without weakening the non-vacuous pass guard.

Success Criteria: The testbench accepts a node with an empty stimulus file, rejects an expected-active node that transfers zero transactions, and rejects a run whose total loaded stimulus is zero.

Status: Complete

### Task 1: Derive non-vacuity from loaded stimulus

**Files:**

- Modify: `sim/tb/test/user_node_endpoint.sv:80-90, 1070-1085`
- Modify: `sim/tb/noc_tb_top.sv:335-375, 420-445`
- Modify: `sim/tools/gen_tb_top.py:998-1050, 1110-1135`
- Test: `sim/tools/test_gen_tb_top.py`

**Interfaces:**

- Produces: `output int unsigned expected_txn_cnt_o` from `user_node_endpoint`.
- Definition: `expected_txn_cnt_o = file_master.num_writes + file_master.num_reads` immediately after `load_files()`.
- Consumers: both hand-written and generated top-level pass guards and the watchdog timeout calculation.

- [ ] **Step 1: Add failing generated-top assertions**

```python
def test_user_endpoint_exports_loaded_stimulus_count():
    text = g.emit_tb_top(g.load_topology("mesh_4x4"))
    assert "int unsigned expected_txn_cnt" in text
    assert ".expected_txn_cnt_o(expected_txn_cnt[i])" in text


def test_non_vacuity_allows_declared_idle_nodes_but_not_empty_run():
    text = g.emit_tb_top(g.load_topology("mesh_4x4"))
    assert "expected_total += expected_txn_cnt[i];" in text
    assert "expected_txn_cnt[i] > 0 && txn_cnt[i] == 0" in text
    assert 'if (expected_total == 0) $fatal(1, "tb_top: empty stimulus");' in text


def test_watchdog_uses_loaded_stimulus_count():
    text = g.emit_tb_top(g.load_topology("mesh_4x4"))
    assert "expected_total" in text
    assert "tb_num_reads + tb_num_writes" not in text
```

- [ ] **Step 2: Run the focused tests and confirm RED**

Run:

```text
python -m pytest sim/tools/test_gen_tb_top.py -q
```

Expected: the new assertions fail because `expected_txn_cnt` does not exist.

- [ ] **Step 3: Export the loaded count from the endpoint**

Add the port:

```systemverilog
output int unsigned expected_txn_cnt_o,
```

Set it after file loading:

```systemverilog
file_master.load_files(read_path, write_path);
expected_txn_cnt_o = int'(file_master.num_writes + file_master.num_reads);
```

- [ ] **Step 4: Replace the per-node non-vacuity assumption**

Use this logic in `noc_tb_top.sv` and the matching `gen_tb_top.py` template:

```systemverilog
expected_total = 0;
for (int i = 0; i < NUM_ENDPOINTS; i++) begin
    expected_total += expected_txn_cnt[i];
    if ((injection_mode != 3 || i < 3) &&
        expected_txn_cnt[i] > 0 && txn_cnt[i] == 0) begin
        vacuous = 1'b1;
        $display("FAIL: node%0d completed zero of %0d loaded transactions",
                 i, expected_txn_cnt[i]);
    end
end
if (expected_total == 0) $fatal(1, "tb_top: empty stimulus");
if (vacuous) $fatal(1, "tb_top: vacuous run");
```

After reset release, the watchdog must sum the loaded `expected_txn_cnt` values and size its default timeout from that total instead of `+num_reads/+num_writes`. Preserve `+timeout_cycles` as the explicit override. Apply the same change to the hand-written and generated top levels.

- [ ] **Step 5: Run focused tests and a generated-top drift check**

```text
python -m pytest sim/tools/test_gen_tb_top.py -q
python specgen/tools/codegen.py --check
```

Expected: PASS.

- [ ] **Step 6: Commit the completion contract**

```text
git add sim/tb/test/user_node_endpoint.sv sim/tb/noc_tb_top.sv sim/tools/gen_tb_top.py sim/tools/test_gen_tb_top.py
git commit -m "test(sim): allow declared idle traffic sources"
```

---

## Stage 2: AI-semantic destination mappings

Goal: Generate the approved Broadcast, Gather, AlltoAll, Neighbor Exchange, and Pipeline P2P traffic without changing the synthetic patterns.

Success Criteria: One-round fixtures exactly match the approved 4x4 source/destination sets, all burst geometry comes from CLI parameters, and every AI pattern emits empty read files.

Status: Complete

### Task 2: Add pure AI pattern helpers and CLI wiring

**Files:**

- Modify: `sim/tools/gen_test_patterns.py:300-510, 700-820, 1095-1315`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py`
- Modify: `sim/verilator/Makefile:50-90, 280-390, 430-555`
- Modify: `README.md:120-130, 180-230`
- Modify: `docs/verification-environment.md:295-355, 390-410`
- Test: `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**

- Produces: `gather_dsts`, `neighbor_exchange_dsts`, `pipeline_order`, and `pipeline_dsts`.
- Extends: `mcast_groups("global", 4, 4)`.
- Adds CLI: `--gather-shape global|submesh` and `--root-node`.
- Adds CLI: `--rounds`, used only by AI-semantic patterns and the report's `many_to_many` alias.
- Adds Make variables: `GATHER_SHAPE`, `ROOT_NODE`, and optional `AI_ROUNDS`.
- Keep `INJECTION_COUNT` as the legacy per-node transaction-count input. When `AI_ROUNDS` is set, the Makefile passes `--rounds`; each AI helper expands one round into the required writes for that source. When it is unset, new AI patterns treat `--transactions-per-node` as their round count, while `many_to_many` retains its legacy exact transaction-count behavior.
- Before sizing the address window, build the destination list for every source. Set allocator capacity from `max(len(dsts_per_source))`, not the unexpanded round count. Include the multicast window required by Broadcast in the checked extent.
- Generate one `traffic_meta.json` beside the node stimulus directories from those same destination lists. It records `active_sources`, `source_write_bursts`, and `destination_deliveries`; for unicast, one write is one delivery, while Broadcast adds the exact member count of each multicast AW.

- [x] **Step 1: Write exact mapping tests**

```python
def test_global_gather_defaults_to_corner_root_zero():
    assert g.gather_dsts(0, 4, 4, 2, "global", 0) == []
    assert g.gather_dsts(7, 4, 4, 2, "global", 0) == [0, 0]


def test_global_gather_accepts_a_non_default_root_and_rejects_bad_roots():
    assert g.gather_dsts(0, 4, 4, 1, "global", 15) == [15]
    with pytest.raises(ValueError):
        g.gather_dsts(0, 4, 4, 1, "global", 16)


def test_local_gather_uses_approved_region_leaders():
    expected = {0: 5, 1: 5, 4: 5, 2: 6, 3: 6, 7: 6,
                8: 9, 12: 9, 13: 9, 11: 10, 14: 10, 15: 10}
    for source, leader in expected.items():
        assert g.gather_dsts(source, 4, 4, 1, "submesh", 0) == [leader]
    for leader in (5, 6, 9, 10):
        assert g.gather_dsts(leader, 4, 4, 1, "submesh", 0) == []


def test_local_gather_rejects_non_4x4_topology():
    with pytest.raises(ValueError, match="4x4"):
        g.gather_dsts(0, 2, 2, 1, "submesh", 0)


def test_neighbor_exchange_is_non_periodic_cartesian():
    assert g.neighbor_exchange_dsts(0, 4, 4, 1) == [1, 4]
    assert g.neighbor_exchange_dsts(5, 4, 4, 1) == [4, 6, 1, 9]
    assert sum(len(g.neighbor_exchange_dsts(n, 4, 4, 1)) for n in range(16)) == 48


def test_pipeline_uses_one_hop_row_snake():
    order = [0, 1, 2, 3, 7, 6, 5, 4, 8, 9, 10, 11, 15, 14, 13, 12]
    assert g.pipeline_order(4, 4) == order
    for source, destination in zip(order, order[1:]):
        assert g.pipeline_dsts(source, 4, 4, 1) == [destination]
    assert g.pipeline_dsts(order[-1], 4, 4, 1) == []


def test_global_broadcast_has_one_source_and_all_members():
    assert g.mcast_groups("global", 4, 4) == [
        ((0, 0), [(x, y) for y in range(4) for x in range(4)])
    ]


def test_ai_alltoall_expands_one_round_to_every_remote_rank():
    assert g.ai_alltoall_dsts(0, 16, 1) == list(range(1, 16))
    assert sum(len(g.ai_alltoall_dsts(n, 16, 1)) for n in range(16)) == 240
```

- [x] **Step 2: Run the mapping tests and confirm RED**

```text
python -m pytest sim/tools/test_gen_test_patterns_filemaster.py -k "gather or neighbor_exchange or pipeline or global_broadcast" -q
```

Expected: FAIL because the AI helpers and CLI choices do not exist.

- [x] **Step 3: Implement topology-derived mappings**

Use west/east/north/south order for Neighbor Exchange:

```python
def neighbor_exchange_dsts(src_node, x_dim, y_dim, rounds):
    x, y = _coords(src_node, x_dim)
    neighbors = []
    if x > 0:
        neighbors.append(_linear(x - 1, y, x_dim))
    if x + 1 < x_dim:
        neighbors.append(_linear(x + 1, y, x_dim))
    if y > 0:
        neighbors.append(_linear(x, y - 1, x_dim))
    if y + 1 < y_dim:
        neighbors.append(_linear(x, y + 1, x_dim))
    return neighbors * rounds
```

Generate the pipeline from topology dimensions:

```python
def pipeline_order(x_dim, y_dim):
    return [_linear(x, y, x_dim)
            for y in range(y_dim)
            for x in (range(x_dim) if y % 2 == 0 else range(x_dim - 1, -1, -1))]


def pipeline_dsts(src_node, x_dim, y_dim, rounds):
    order = pipeline_order(x_dim, y_dim)
    pos = order.index(src_node)
    return [] if pos + 1 == len(order) else [order[pos + 1]] * rounds
```

Implement Global and Local Gather with `ROOT_NODE` validation. Keep the approved local leader table explicit because it defines this 4x4 workload, not a burst-size constant. Add `ai_alltoall_dsts(src, nodes, rounds)` as a thin round-count wrapper over the existing `all_to_all_dsts` ordering.

Extend the existing multicast emitter without changing its defaults:

```python
def emit_multicast_pattern(out_root, nodes, x_dim, y_dim, bases, config_bases,
                           sizes, shape, n_txn, axi_size, axi_len, data_width,
                           base_local, region_bytes, n_slots, readback=True,
                           filler=True, config_probe=True):
```

The new `broadcast` branch calls it with all three flags false. This produces pure write-only collective traffic and intentionally empty non-source files, while legacy `multicast` retains its readback, neighbor filler, and config probe.

- [x] **Step 4: Add pure write-only emission tests**

For each new CLI pattern, generate two rounds at `--size 5 --len 7`, then assert:

```python
assert read_path.read_text() == ""
assert all(txn["size"] == 5 and txn["len"] == 7 and len(txn["beats"]) == 8
           for txn in writes)
```

Also assert the one-round network-write totals:

```text
Global Broadcast: 1 injected multicast AW
Global Gather: 15 unicast AWs
Local 2x2 Gather: 12 unicast AWs
AlltoAll: 240 unicast AWs
Neighbor Exchange: 48 unicast AWs
Pipeline P2P: 15 unicast AWs
Regional Exchange: 64 unicast AWs
```

Run the real CLI entry point for at least one-round AlltoAll and Neighbor Exchange fixtures. Assert generation succeeds, every address stays inside its destination window, and the emitted totals are 240 and 48 respectively. This test must exercise allocator sizing after destination-list expansion, not only the pure helper functions.

For every mapping fixture, parse `traffic_meta.json` and compare `active_sources` and `source_write_bursts` with the emitted node files. Check Broadcast `destination_deliveries` against the exact Row, Column, 2x2, and Global group memberships; this metadata is the report's fanout source of truth.

- [x] **Step 5: Wire new Make variables without hardcoded burst geometry**

Pass only pattern-specific mapping arguments:

```make
GATHER_SHAPE ?= global
ROOT_NODE    ?= 0
AI_ROUNDS    ?=

_AI_ARGS     := $(if $(AI_ROUNDS),--rounds $(AI_ROUNDS))
_GATHER_ARGS := $(if $(filter gather,$(PATTERN)),--gather-shape $(GATHER_SHAPE) --root-node $(ROOT_NODE))
_MCAST_ARGS  := $(if $(filter multicast broadcast,$(PATTERN)),--mcast-shape $(MCAST_SHAPE))
```

Add `global` to the multicast shape choices. Include mapping parameters in both stimulus and simulation tags:

```text
Broadcast: <pattern>_<mcast-shape>
Gather global: gather_global_root<root-node>
Gather submesh: gather_submesh
```

Add Make dry-run assertions covering Broadcast `row` versus `global`, Gather `global root0` versus `global root15`, and Gather `submesh`; each case must produce distinct tags and the expected generator arguments.

Add this CLI contract after argument parsing:

```python
ai_patterns = {"broadcast", "gather", "alltoall", "neighbor_exchange", "pipeline"}
if a.rounds is not None and a.pattern not in ai_patterns | {"many_to_many"}:
    ap.error("--rounds is valid only for AI traffic patterns")
if a.rounds is not None and a.rounds <= 0:
    ap.error("--rounds must be positive")
rounds = a.rounds if a.rounds is not None else a.transactions_per_node
```

For `many_to_many`, use `4 * a.rounds` only when `--rounds` is present; otherwise pass `a.transactions_per_node` unchanged to preserve the existing CLI.

Keep `STIM_SIZE`, `BURST_LEN`, and `INJECTION_COUNT` on the existing common generator command. Extend the write-only read-count selection to all AI patterns. Calculate `region_bytes` and `extent` only after the per-source destination lists are known, using the maximum expanded source count and any Broadcast multicast window.

- [x] **Step 6: Run the complete generator suite**

```text
python -m pytest sim/tools/test_gen_test_patterns_filemaster.py -q
```

Expected: PASS.

- [x] **Step 7: Commit AI mapping generation**

```text
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns_filemaster.py sim/verilator/Makefile README.md docs/verification-environment.md
git commit -m "feat(sim): add AI inference traffic mappings"
```

---

## Stage 3: Directed-round completion measurement

Goal: Measure one fully-ready AXI write round from common issue start through the final expected B response, without mixing in the testbench settle window.

Success Criteria: The log reports the common directed-round start, final expected B completion, active-source count, and exact elapsed cycles; CSV parsing rejects missing or inconsistent evidence.

Status: Not Started

### Task 3: Add endpoint completion evidence and CSV fields

**Files:**

- Modify: `sim/tb/test/user_node_endpoint.sv:80-90, 1065-1200`
- Modify: `sim/tb/noc_tb_top.sv:335-375, 420-455`
- Modify: `sim/tools/gen_tb_top.py:998-1050, 1110-1150`
- Modify: `sim/tools/emit_result_csv.py`
- Modify: `sim/verilator/Makefile:280-490`
- Test: `sim/tools/test_emit_result_csv.py`
- Test: `sim/tools/test_gen_tb_top.py`

**Interfaces:**

- Produces endpoint outputs `stimulus_start_cycle_o` and `stimulus_done_cycle_o`.
- Emits exactly one metadata line in every mode, derived from loaded `expected_write_cnt[]` rather than the pattern name:

```text
[TrafficMeta] active_sources=<n> write_bursts=<n>
```

- Emits one top-level line:

```text
[RoundPerf] start_cycle=<n> completion_cycle=<n> round_cycles=<n> active_sources=<n> write_bursts=<n>
```

- CSV columns: `round_completion_cycles`, `round_active_sources`, `round_write_bursts`, and `stim_size`.

- [ ] **Step 1: Add failing parser tests**

```python
ROUND_LOG = """[RoundPerf] start_cycle=10 completion_cycle=210 round_cycles=200 active_sources=15 write_bursts=15
PASS: all 16 nodes done, non-vacuous
"""


def test_round_perf_fields_are_preserved():
    row = e.parse_round_perf(ROUND_LOG)
    assert row == {
        "round_completion_cycles": "200",
        "round_active_sources": "15",
        "round_write_bursts": "15",
    }


def test_round_perf_rejects_inconsistent_elapsed_time():
    with pytest.raises(SystemExit):
        e.parse_round_perf(ROUND_LOG.replace("round_cycles=200", "round_cycles=199"))
```

Also reject a missing or duplicate `[RoundPerf]` line, `active_sources=0`, and `write_bursts=0`. Add equivalent strict tests for missing or duplicate `[TrafficMeta]` evidence. Verify that `STIM_SIZE` is passed into CSV as `stim_size`, because report comparisons must reject mixed transfer geometry.

- [ ] **Step 2: Run the parser tests and confirm RED**

```text
python -m pytest sim/tools/test_emit_result_csv.py -k round_perf -q
```

Expected: FAIL because `parse_round_perf` does not exist.

- [ ] **Step 3: Capture cycles at the correct boundaries**

In directed mode, set the common start immediately after reset release and set done immediately after `wait_b()`/`wait_r()` joins, before `run_done` and the 50-cycle response check:

```systemverilog
@(posedge rst_ni);
stimulus_start_cycle_o = cycle_cnt;
case (get_injection_mode())
    // existing mode bodies
endcase
stimulus_done_cycle_o = cycle_cnt;
run_done = 1'b1;
```

The top must aggregate only endpoints whose `expected_txn_cnt[i] > 0`:

```systemverilog
round_start = 0;
round_completion = 0;
active_sources = 0;
write_bursts = 0;
for (int i = 0; i < NUM_ENDPOINTS; i++) begin
    if (expected_txn_cnt[i] > 0) begin
        if (active_sources == 0)
            round_start = stimulus_start_cycle[i];
        else if (stimulus_start_cycle[i] != round_start)
            $fatal(1, "RoundPerf sources did not start together");
        round_completion = round_completion > stimulus_done_cycle[i]
                         ? round_completion : stimulus_done_cycle[i];
        active_sources++;
        write_bursts += expected_write_cnt[i];
    end
end
```

Export `expected_write_cnt_o` separately from total transactions so the log does not infer writes from a write-only policy. Sum `expected_write_cnt[]` once at top level and emit `[TrafficMeta]` for all runs; `RoundPerf` reuses the same counts instead of recomputing them from pattern semantics.

- [ ] **Step 4: Gate RoundPerf to valid directed write-only runs**

Add Make variable `ROUND_PERF ?= 0` and pass `+round_perf=$(ROUND_PERF)`. Fatal unless `injection_mode == 0` and every active endpoint has zero loaded reads. This prevents a synthetic write/readback run from being mislabeled as an AI communication round.

When `ROUND_PERF=1`, the directed Make recipe must invoke `emit_result_csv.py` after the exact PASS check so the RoundPerf evidence reaches `result.csv`. Do not invoke the CSV path for ordinary directed correctness runs.

- [ ] **Step 5: Parse and validate RoundPerf**

Implement a strict anchored regex. Require:

```text
round_cycles = completion_cycle - start_cycle
active_sources > 0
write_bursts > 0
```

Write the fields into `result.csv`; leave the round fields empty when `+round_perf` was not requested.

- [ ] **Step 6: Run focused tests**

```text
python -m pytest sim/tools/test_emit_result_csv.py sim/tools/test_gen_tb_top.py -q
```

Expected: PASS.

- [ ] **Step 7: Commit round measurement**

```text
git add sim/tb/test/user_node_endpoint.sv sim/tb/noc_tb_top.sv sim/tools/gen_tb_top.py sim/tools/emit_result_csv.py sim/tools/test_emit_result_csv.py sim/tools/test_gen_tb_top.py
git commit -m "perf(sim): measure directed AI communication rounds"
```

---

## Stage 4: Common performance flow and report

Goal: Run every approved AI communication type under one documented geometry and produce directly comparable report rows and figures.

Success Criteria: Each row states its participant scale and shape, all patterns use the same 4 KB transfer geometry and per-active-source load points, directed-round completion is separated from continuous-load metrics, and generated output contains only current results plus Markdown reports.

Status: Not Started

### Task 4: Extend result aggregation and report wording

**Files:**

- Modify: `sim/tools/emit_result_csv.py`
- Modify: `sim/tools/perf_report.py`
- Modify: `sim/tools/test_emit_result_csv.py`
- Modify: `sim/tools/test_perf_report.py`
- Generate locally: `sim/verilator/output/perf_report.md` (ignored simulation artifact, not committed)

**Interfaces:**

- AI write-only set: `broadcast`, `gather`, `alltoall`, `neighbor_exchange`, `pipeline`, and `many_to_many`.
- Display map: `many_to_many -> Regional Exchange`.
- `Useful delivered bandwidth` remains workload-adjusted; the GEMM reference uses the approved 50% useful-byte ratio.
- `SWEEP_OFFERED` means offered DAT load per active source, not a 16-node average.
- CSV records `active_sources`, `offered_load_per_active_source`, `offered_load_mesh_avg`, and `accepted_injection_load_mesh_avg`.
- Make passes `--traffic-meta $(STIM_ROOT)/traffic_meta.json` to result aggregation. The parser must require the run's `[TrafficMeta] active_sources/write_bursts` to match the generated metadata before using `destination_deliveries`.
- Definitions:

```text
offered_load_mesh_avg = offered_load_per_active_source * active_sources / NUM_ENDPOINTS
accepted_injection_load_mesh_avg = completed source write bursts * (1 + beats) / (measured cycles * NUM_ENDPOINTS)
```

`accepted_injection_load_mesh_avg` is source-side DAT injection: one AW header plus its W beats. It does not count link traversals or multicast replicas. `Useful delivered bandwidth` is destination-side payload bytes per cycle: unicast delivery counts match completed source writes, while Broadcast uses the generated fanout metadata validated by merged B completion. Keep these two boundaries separate in tables and formulas.

The source monitor cannot observe multicast replicas. Calculate delivered payload from the validated metadata only after the run passes all expected B responses:

```text
useful_delivered_bandwidth = destination_deliveries * beats * BEAT_BYTES * useful_byte_ratio / measured_cycles
```

For unicast, `destination_deliveries` equals completed source writes. For Broadcast, the generated member count supplies fanout and the merged B completion proves that all expected replicas completed; never infer fanout from the single source-side monitor sample.

- [ ] **Step 1: Add failing aggregation tests**

Test that every AI pattern omits read offered-load terms and that `many_to_many` renders as `Regional Exchange`. Parse `active_sources` only from `[TrafficMeta]`, require it to match `traffic_meta.json`, then test 1/16, 4/16, 15/16, and 16/16 normalization. Reject missing metadata, count mismatches, and Broadcast fanout mismatches. Test that the report rejects comparison rows with different `STIM_SIZE`, `BURST_LEN`, per-active-source load points, or seeds.

- [ ] **Step 2: Run aggregation tests and confirm RED**

```text
python -m pytest sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py -q
```

Expected: FAIL on the new AI pattern set and label mapping.

- [ ] **Step 3: Centralize the two small policy tables**

```python
AI_WRITE_ONLY = frozenset({
    "broadcast", "gather", "alltoall", "neighbor_exchange", "pipeline",
    "many_to_many",
})

DISPLAY_NAME = {
    "broadcast": "Broadcast / Multicast",
    "gather": "Gather",
    "alltoall": "AlltoAll",
    "neighbor_exchange": "Neighbor Exchange",
    "pipeline": "Pipeline P2P",
    "many_to_many": "Regional Exchange",
}
```

Do not add a class or registry; these two immutable tables are the complete current policy.

- [ ] **Step 4: Separate the two measurement conditions in the report**

Use these labels:

```text
Continuous sweep:
  Low-load completion latency (cycles/transaction)
  Useful delivered bandwidth (B/cycle)
  DAT-link utilization (%)

Synchronized directed round:
  AXI write round completion time (cycles/round)
```

State the measurement boundary beside the metric: common issue start through the final expected B response. Do not claim it is a NoC-only time or that it was measured under background load.

- [ ] **Step 5: Run report tests**

```text
python -m pytest sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py -q
```

Expected: PASS.

- [ ] **Step 6: Commit aggregation and report structure**

```text
git add sim/tools/emit_result_csv.py sim/tools/perf_report.py sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py
git commit -m "perf(sim): report AI traffic on common metrics"
```

### Task 5: Run clean co-sim evidence

**Files:**

- Generate: `sim/verilator/output/<current-run>/`
- Preserve: `sim/verilator/output/*.md`

**Interfaces:**

- Directed runs use `INJECTION_MODE=0 ROUND_PERF=1 AI_ROUNDS=1`.
- Continuous sweeps use identical per-active-source `SWEEP_OFFERED`, `STIM_SIZE=6`, `BURST_LEN=63`, and seed list for every AI pattern. The report also shows each pattern's active-source count and derived mesh-average load.

- [ ] **Step 1: Preserve Markdown and run the documented clean target**

Use a temporary directory for `output/*.md`, run `make -C sim/verilator clean`, then restore only the Markdown files before generating new evidence.

- [ ] **Step 2: Run one directed round per mapping**

Run Broadcast Row/Column/2x2/Global, Gather Global/Local, AlltoAll, Neighbor Exchange, Pipeline P2P, and Regional Exchange. Require exact non-vacuous PASS plus the expected `RoundPerf` counts from Stage 2.

- [ ] **Step 3: Run the common offered-load sweep**

Use the approved 4 KB geometry. The sweep must calculate Injection rate from each active source's write-only DAT flits rather than reuse the retired read/write-pair formula. Record both per-active-source and mesh-average offered load; do not compare them as if they used the same denominator.

- [ ] **Step 4: Generate the report and figures**

```text
python sim/tools/perf_report.py sim/verilator/output
```

Expected: every primary communication type appears, units are present, and the report contains no retired synthetic-pattern row.

- [ ] **Step 5: Run final gates**

```text
python -m pytest sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py sim/tools/test_gen_tb_top.py -q
python specgen/tools/codegen.py --check
make build
make test
make pytest
make -C sim/verilator hello
```

Then run the required clean 2x2 `neighbor` smoke and one 4x4 AI directed smoke under WSL. Expected: all checks PASS.

- [ ] **Step 6: Verify cleanup and cross-file consistency**

Confirm `sim/verilator/output/` contains only the current AI run directories and Markdown reports. List every `.md` hit for the changed traffic names, record whether each file needs an edit, and verify the same definitions appear in `README.md`, `docs/verification-environment.md`, and the AI report.

- [ ] **Step 7: Close campaign metadata**

The report and run directories under `sim/verilator/output/` remain local ignored artifacts. Stage only explicitly listed campaign files; never use directory-wide `git add` in the dirty worktree. Fold the result into local ignored `docs/backlog.md` without committing it, and remove the root `IMPLEMENTATION_PLAN.md` only after all stages are complete.

---

## Explicitly Deferred

- AllReduce and ReduceScatter: require arithmetic reduction semantics and a matching completion check.
- Compute overlap coverage `[TBD]`: requires a user-approved PE compute budget in cycles plus a tagged probe round or synchronized round scheduler. Until both exist, do not emit the metric or relabel directed-round completion as overlap coverage.
- Variable-size AlltoAllv for MoE imbalance: add only after the balanced AlltoAll baseline is measured.
- Dependent single-tensor pipeline latency: requires receive-to-forward dependencies between endpoints; the first implementation measures steady-state link traffic.
