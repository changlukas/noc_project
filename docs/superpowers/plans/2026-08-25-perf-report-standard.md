# Performance report standard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the performance report and its tooling with the textbook method: open loop injection, `plat` and `nlat`, latency vs offered load per pattern, saturation by the 3x rule, per pattern ideal throughput and percent of ideal.

**Architecture:** Testbench gains a booksim style `qtime` source queue and prints per node source queue delay. `emit_result_csv.py` carries offered load, `mean_latency_network`, `mean_latency_open`. A new `pattern_metrics.py` computes avg hops and ideal throughput from the generator's destination functions. A new `perf_report.py` renders the whole report from the run directories. The old summarizer, plotter, scratch files and old report are deleted.

**Tech Stack:** SystemVerilog tb (Verilator), Python 3 (stdlib, matplotlib optional), pytest under `sim/tools`, WSL co-sim.

**Spec:** `docs/superpowers/specs/2026-08-25-perf-report-standard.md` (binding).

## Global Constraints

- Branch `feat/perf-report-standard` off `chore/ni-rx-depth-sweep`. Commits `type(scope): description`, body ends with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- `sim/dv/` is third party, never modified. No DUT change (`ref_model/` untouched).
- Clean cut: everything in the spec's Removed list is deleted in this campaign, no compatibility shims, no dead knobs left behind. `grep -rn 'summarize_results\|plot_injection_sweep\|perf_cli_summary\|sweep_summary' Makefile sim docs README.md` must return nothing when done.
- Writing rules for the report and docs: terse, tables over prose, no semicolons or dashes in running text, every number from a run or the analytic script.
- WSL only for builds and runs, foreground, explicit Bash timeout up to 600000 ms, `BUILD_ROOT=$HOME/noc_build`, `VERILATOR_EXTRA_FLAGS="-CFLAGS -g0"` / `CCACHE_DISABLE=1` on the known build flakes, retry once on a transient Python crash. After a sim: `git checkout -- sim/verilator/test_patterns/ specgen/generated/` if modified. If `ni_params.h` changes, wipe `$HOME/noc_build/verilator/obj_dir_*` before rebuilding (a stale make dependency silently kept the old model once).
- A subagent that hits a spec deviation or a failing gate reports BLOCKED with verbatim output. No workarounds.

---

## Stage 1: Open loop injection
Goal: the testbench records source queue delay per transaction and `result.csv` carries `plat`.
Success Criteria: a rate 0.005 run shows `mean_latency_open` within 1 cycle of `mean_latency_network`, a rate 1.0 uniform_random run shows `mean_latency_open` far above it and growing with `INJECTION_COUNT`.
Status: Not Started

### Task 1: `qtime` in `user_node_endpoint.sv` and the CSV columns

**Files:**
- Modify: `sim/tb/test/user_node_endpoint.sv` (`run_aw_paced` / `run_ar_paced` ~:715-727, end of sim reporting near the `[mst_bp]` prints)
- Modify: `sim/tools/emit_result_csv.py` (`_MON` regex, new `_SRCQ` regex, `parse_monitors`, row fields)
- Test: `sim/tools/test_emit_result_csv.py` (new, pytest)

**Interfaces:**
- Produces: run.log lines `[SrcQueue node<N>][Read] mean: <float>, N: <int>` and `[Write]`, one pair per node. `result.csv` columns `offered_flits_per_node_cycle`, `offered_bytes_per_node_cycle`, `accepted_bits_per_cycle` (unchanged), `mean_latency_network`, `mean_latency_open`. `mean_latency` is removed.

- [ ] **Step 1: Write the failing test** `sim/tools/test_emit_result_csv.py`:

```python
import csv, pathlib, sys
import emit_result_csv as e

LOG = """[Config] max_unique_ids=1 max_outstanding=32 dat_num_vc=2 router_vc_depth=8 mst_stall_random=0 ni_dat_rx_vc_depth=8
[Monitor node0.master][Read] Latency: 40.00 +- 1.00, N: 100, BW: 64.00 Bits/cycle, Util: 10.00%
[Monitor node0.master][Write] Latency: 60.00 +- 1.00, N: 100, BW: 64.00 Bits/cycle, Util: 10.00%
[SrcQueue node0][Read] mean: 10.00, N: 100
[SrcQueue node0][Write] mean: 30.00, N: 100
"""

def test_open_latency_adds_source_queue_delay(tmp_path, monkeypatch):
    log = tmp_path / "run.log"; log.write_text(LOG)
    out = tmp_path / "result.csv"
    monkeypatch.setattr(sys, "argv", ["emit_result_csv", "--log", str(log), "--out", str(out),
        "--topology", "mesh_4x4", "--pattern", "neighbor", "--injection-mode", "1",
        "--injection-rate", "0.5", "--injection-count", "200", "--seed", "1", "--burst-len", "32"])
    e.main()
    row = next(csv.DictReader(out.open()))
    assert row["mean_latency_network"] == "50.0"   # (40*100 + 60*100) / 200
    assert row["mean_latency_open"] == "70.0"      # + (10*100 + 30*100) / 200
    # offered on the DAT plane: p = 0.5 AX per cycle on each of AW and AR. BURST_LEN 32 is
    # AxLEN 32, 33 beats (gen_test_patterns.py emits axi_len + 1 W beats). A write is
    # 1 AW header + 33 W = 34 DAT flits, a read is 33 R = 33 DAT flits (AR rides REQ).
    # flits: 0.5 * 34 + 0.5 * 33 = 33.5. bytes (payload beats only): 0.5 * 33 * 64 * 2 = 2112.
    assert row["offered_flits_per_node_cycle"] == "33.5"
    assert row["offered_bytes_per_node_cycle"] == "2112.0"
    assert "mean_latency" not in row
```

Verify the beat count against `sim/tools/gen_test_patterns.py` (`axi_len + 1` W beats per write, same for R) before trusting the numbers above, state the formula in the module docstring. Offered load counts DAT plane network flits only (AW header plus beats), the spec's convention.

- [ ] **Step 2: Run** `cd sim/tools && python3 -m pytest -q test_emit_result_csv.py`. Expected FAIL (no `mean_latency_open`).

- [ ] **Step 3: Implement the tb.** In `user_node_endpoint.sv`, replace the pacing tasks. The Bernoulli process must keep running every cycle while a send is blocked on ready (booksim `_Inject`, `trafficmanager.cpp:930-941`), so it is its own process that produces slot times into a queue, and the send task consumes the queue:

```systemverilog
    // Open loop source queue (booksim2 _qtime, trafficmanager.cpp:922-945).
    // One Bernoulli trial per cycle per channel, independent of the network:
    // a hit appends the cycle as a slot. The send task takes the oldest slot
    // and issues when the channel is ready. handshake minus slot is the source
    // queue delay the packet latency includes and the monitor does not see.
    longint unsigned aw_slots[$], ar_slots[$];
    longint unsigned srcq_w_sum, srcq_r_sum;
    int unsigned     srcq_w_n,   srcq_r_n;

    always @(posedge clk_i) begin
        if (rst_ni && pacing_on) begin
            if ($urandom_range(0, 99) < injection_rate_pct) aw_slots.push_back(cycle_cnt);
            if ($urandom_range(0, 99) < injection_rate_pct) ar_slots.push_back(cycle_cnt);
        end
    end

    task automatic run_aw_paced();
        longint unsigned slot;
        while (file_master.aw_queue.size() > 0) begin
            while (aw_slots.size() == 0) @(posedge clk_i);
            slot = aw_slots.pop_front();
            file_master.drv.send_aw(file_master.aw_queue[0]);  // blocks until awready
            srcq_w_sum += cycle_cnt - slot;
            srcq_w_n++;
            void'(file_master.aw_queue.pop_front());
        end
    endtask
```

`pacing_on` is set when mode 1 or 2 starts and cleared when the AW and AR queues are both empty, so slots stop accumulating after the last transaction. `cycle_cnt` is the endpoint's free running cycle counter (grep the `[mst_bp]` accounting; add one if none). Same shape for AR. Mode 2 keeps using `run_aw_paced`. At end of sim, next to the `[mst_bp]` display, print `[SrcQueue node%0d][Read] mean: %0.2f, N: %0d` and `[Write]` (mean 0.00 with N 0 when nothing was issued). Do not touch mode 0 or mode 2 pacing beyond sharing the task if they already share it (mode 2 uses `run_aw_paced`, keep it working).

- [ ] **Step 4: Implement the CSV.** `emit_result_csv.py`: parse `[SrcQueue …]` lines, compute sample weighted means for the monitor latency (`mean_latency_network`) and for the source queue delay, `mean_latency_open = network + srcq`. Offered load from `--injection-rate` and `--burst-len` per the formula you derived. Drop `mean_latency`. Update the module docstring.

- [ ] **Step 5: Run** the pytest. Expected PASS. Then `make build-verilator` and two co-sim runs on WSL: `make sim-gen CONFIG=mesh_4x4 PATTERN=uniform_random INJECTION_MODE=1 INJECTION_RATE=0.005 BURST_LEN=32 SEED=1` + `make sim`, and the same at `INJECTION_RATE=1.0`. Expected: at 0.005 `mean_latency_open` within 1 cycle of `mean_latency_network`, at 1.0 open is at least 3 times network. Record both rows in the report.

- [ ] **Step 6: Commit** `feat(tb): open loop injection with source queue delay, plat and nlat in result.csv`

---

## Stage 2: Analytic pattern metrics
Goal: avg hops and ideal throughput per pattern from the generator's own destination functions.
Success Criteria: pytest pins uniform_random ideal = 1.0 flit per node per cycle on 4x4 (textbook 4 / k) and neighbor avg hops = 2.
Status: Not Started

### Task 2: `sim/tools/pattern_metrics.py`

**Files:**
- Create: `sim/tools/pattern_metrics.py`
- Test: `sim/tools/test_pattern_metrics.py`

**Interfaces:**
- Produces: `metrics(pattern: str, x_dim: int, y_dim: int, hotspots=None) -> dict(avg_hops=float, max_channel_load=float, ideal_flits_per_node_cycle=float)`; CLI `python3 pattern_metrics.py mesh_4x4` prints a markdown table for all ten patterns.

- [ ] **Step 1: Write the failing tests**

```python
import pattern_metrics as pm

def test_uniform_random_matches_textbook_4_over_k():
    m = pm.metrics("uniform_random", 4, 4)
    assert abs(m["ideal_flits_per_node_cycle"] - 1.0) < 1e-9   # 4 / k, k = 4
    assert abs(m["avg_hops"] - 8 / 3) < 0.01                  # (2/3) k

def test_neighbor_is_two_hops_one_flow_per_link():
    m = pm.metrics("neighbor", 4, 4)
    assert m["avg_hops"] == 2.0
    assert m["ideal_flits_per_node_cycle"] == 1.0

def test_bit_complement_bisection_bound():
    m = pm.metrics("bit_complement", 4, 4)
    assert m["ideal_flits_per_node_cycle"] == 0.5
```

Derive the uniform_random avg hops expectation from the textbook (`(2/3) k` for the average Manhattan distance with self traffic permitted, check and adjust the constant to the exact enumeration, and say which convention you used).

- [ ] **Step 2: Run** pytest. Expected FAIL (module missing).

- [ ] **Step 3: Implement.** Import `neighbor_dst`, `transpose_dst`, `bit_complement_dst`, `bit_reverse_dst`, `shuffle_dst`, `bit_rotation_dst`, `tornado_dst`, `_dst_for` from `gen_test_patterns.py` (do not duplicate the maps). For uniform_random and all_to_all every destination gets weight `1 / n_nodes` (self included, matching the generator's default); hotspot uses the generator's default hotspot set with equal weight. Walk XY (x first) from each source, add the source's weight to each directed link, avg hops = weighted mean of Manhattan distance, max channel load = max over links, ideal = 1 / max load. CLI prints the table. No numpy.

- [ ] **Step 4: Run** pytest. Expected PASS. Print the table for mesh_4x4 into the report file.

- [ ] **Step 5: Commit** `feat(tools): analytic avg hops and ideal throughput per traffic pattern`

---

## Stage 3: Report generator and clean cut
Goal: one script renders the report, the old tooling is gone.
Success Criteria: `python3 sim/tools/perf_report.py sim/verilator/output` writes `perf_report.md` with the six sections from the spec, pytest green, the grep in Global Constraints returns nothing.
Status: Not Started

### Task 3: `sim/tools/perf_report.py`, deletions

**Files:**
- Create: `sim/tools/perf_report.py`, `sim/tools/test_perf_report.py`
- Delete: `sim/tools/summarize_results.py`, `sim/tools/test_summarize_results.py`, `sim/tools/plot_injection_sweep.py`, `sim/verilator/perf_cli_summary.py`, `sim/verilator/output/sweep_summary.md`, `sim/verilator/output/sweep_summary_mesh_2x2.md`
- Modify: `sim/verilator/Makefile` (`sim-injection-sweep` :478-488 loses the plot call, gains `SWEEP_PATTERNS` and `SWEEP_SEEDS` loops, and takes the sweep in offered flits per node per cycle: `SWEEP_OFFERED ?= 0.067 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0 1.2`, converted per point to the Bernoulli rate `p = offered / (2 * (BURST_LEN + 1) + 1)` rounded to basis points, because with AxLEN 32 one AX per channel per cycle offers 67 DAT flits per node per cycle and the old rate grid 0.05 to 1.0 sits entirely above saturation), `README.md` and `docs/verification-environment.md` where they name the removed scripts or the old report

**Interfaces:**
- Consumes: `result.csv` columns from Task 1, `pattern_metrics.metrics`, `perf.json` link counts (reuse the `dat_link_util` logic from the old summarizer, moved).
- Produces: `perf_report.md` sections 1 to 4 and appendices A, B per the spec.

- [ ] **Step 1: Write the failing tests** (fixtures write `result.csv` and `run.log` under `tmp_path/continuous_mesh_4x4_<pattern>_r<rate>_s<seed>`):
  - `test_curve_marks_saturation`: 5 rates for uniform_random with `mean_latency_open` 40, 42, 50, 130, 400 and accepted 5, 10, 20, 21, 21: the saturation offered load is where open latency crosses 3 times the lowest rate's value, linearly interpolated between the two neighbouring rates (here between the third and fourth rate), and the accepted throughput reported for saturation is the interpolated accepted value at that load. No separate knee metric.
  - `test_pattern_summary_percent_of_ideal`: with `pattern_metrics` ideal 1.0 and a measured saturation 0.6, the row shows `60`.
  - `test_seed_spread_column`: two seeds at one rate give mean and max minus min.
  - `test_no_removed_names`: `grep` equivalent in Python over `Makefile`, `sim/`, `docs/`, `README.md` for the removed script names returns no hits (run last, after deletions).

- [ ] **Step 2: Run** pytest. Expected FAIL.

- [ ] **Step 3: Implement** `perf_report.py` (stdlib only, matplotlib optional for a PNG per curve, skipped without it). Group runs by parameter tuple as the old summarizer did (keep that `collect()` logic, moved and trimmed). Section 1 Method text comes from constants in the script and the `[Config]` line. Section 2 uses the zero-load rows (rate 0.005) of the narrow and data probe runs if present under the output dir (`s2zl*` tags) and the per stage table as a static block. Sections 3 and 4 from the curves. Saturation is the 3x rule only, no knee heuristic; section 3 also lists accepted throughput at the highest offered load, which is what booksim prints as accepted rate at saturation. Appendix A from `perf.json`. Appendix B from any run whose tuple differs from the default in vc, router depth or NI RX depth.

- [ ] **Step 4: Delete** the files in the list, edit the Makefile target and the docs that mention them, run the grep. Expected: nothing.

- [ ] **Step 5: Run** all `sim/tools` pytest. Expected green.

- [ ] **Step 6: Commit** `feat(tools): perf_report.py renders the standard report, old summarizer removed`

---

## Stage 4: Runs and report
Goal: the curves exist and the report is rendered.
Success Criteria: `perf_report.md` present with every section filled, saturation and knee per curve pattern, backlog updated.
Status: Not Started

### Task 4: sweeps

- [ ] Archive every existing `continuous_*`, `rxdepth*`, `rvcdepth*`, `sweep/` under `sim/verilator/output/archive/pre_open_loop/` (they carry `mean_latency`, not `plat`). Keep `s2zl*` and `s2_*` probe dirs, rerun the zero-load pair at rate 0.005 with the new tb so `plat` exists for section 2.
- [ ] `make build-verilator`. Sweep points are offered load in DAT flits per node per cycle (`SWEEP_OFFERED`, Task 3), the rate per point is `p = offered / 67` at AxLEN 32, so the lowest point 0.067 is `p = 0.001` and 1.2 is `p = 0.0179`. With 200 transactions per node the offered window is `200 / p` cycles, at least 11000, so `INJECTION_COUNT` stays 200. Zero-load is the lowest point. For each of uniform_random, tornado, shuffle, bit_complement, bit_reverse, transpose: `make sim-injection-sweep CONFIG=mesh_4x4 PATTERN=<p> SEED=1 BURST_LEN=32` (12 points). Then for each pattern find the 3x saturation load from a first `perf_report.py` pass and rerun the two rates around it with `SEED=2` and `SEED=3`. About 6 x 12 + 6 x 4 = 96 runs.
- [ ] The other four patterns: one run each at rate 0.9, seed 1 2 3 (12 runs), so section 4 has their rows with a note that they are single points.
- [ ] Appendix B single points: vc8 uniform_random and neighbor seed 1 at rate 0.9 (2 runs, yaml edit and restore, `codegen.py --check` clean).
- [ ] Render: `python3 sim/tools/perf_report.py sim/verilator/output`. Read it once as a reviewer would: every table populated, no `?`, the 3x saturation point falls where accepted throughput flattens for uniform_random or the discrepancy is stated.
- [ ] `docs/backlog.md` "Last round": the report rebuilt, where the numbers moved versus the old closed-loop report, the saturation table.
- [ ] Commit `docs(backlog): standard performance report rendered`
