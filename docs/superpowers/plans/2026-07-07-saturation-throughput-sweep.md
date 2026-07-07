# Saturation-Throughput Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure the saturation throughput of each VC config (vc1/2/4/8) on the 4x4 mesh and render a bar chart, so the value of adding VCs becomes visible.

**Architecture:** Reuse the directed co-sim build (`RUN_CLASS=directed`, `axi_file_master`) and the already-instantiated per-node `axi_bw_monitor` (`u_bw_mst`). Add a runtime "traffic mode" to `user_node_endpoint.sv` that, when `+traffic_inj_ratio` is present, replaces the two-phase directed run with continuous interleaved injection paced by a per-cycle gate. Sweep a few high injection ratios per VC, read accepted throughput from the monitor, take the plateau as saturation throughput.

**Tech Stack:** SystemVerilog testbench (pulp axi VIP, Verilator 5.048), GNU Make, Python 3 for the collector + bar chart.

## Global Constraints

- Directed path only, no `randomize()` in the injection path, so Verilator never invokes z3. Use `$urandom_range` for the gate.
- Zero edits to upstream sources: pulp `sim/dv/axi-0.39.7/*` and `sim/dv/floonoc-test/axi_bw_monitor.sv` stay untouched. Reuse public `file_master.aw_queue`/`ar_queue`/`drv` only.
- `link_perf_monitor` (8 instances in `src/sv/noc_fabric_mesh_4x4_vc1.sv`) stays untouched.
- Run on WSL/Linux with the gitignored repo-root `local.mk` (`BUILD_ROOT := $(HOME)/noc_build`). The `/mnt/e/.../build` tree is Windows-COFF and WSL `ld` rejects it.
- Naming: `traffic_inj_ratio` (FlooNoC `TRAFFIC_INJ_RATIO`), offered load, accepted / saturation throughput (Dally), `traffic_type` uniform (FlooNoC).
- Phase 1 is `uniform_random` on 4x4 only. Do not wire other patterns or topologies.
- One-shot study. The collector and bar-chart scripts are throwaway, no plotting framework.

---

### Task 1: Traffic mode (continuous gated injection) in the endpoint

**Files:**
- Modify: `sim/tb/user_node_endpoint.sv` (the `ifdef TB_DIRECTED` stimulus `initial`, lines 255-265, plus a small task + signals above it)

**Interfaces:**
- Consumes: existing `file_master` (`axi_test::axi_file_master`), its public `aw_queue`/`ar_queue` (`ax_beat_t[$]`), `drv.send_aw(ax_beat_t)` / `drv.send_ar(ax_beat_t)`, `run_w()`, `clk_i`, `rst_ni`, `run_done`.
- Produces: when `+traffic_inj_ratio=<f>` (0.0..1.0) is passed, the endpoint injects continuously at that rate for `TRAFFIC_CYCLES` then sets `run_done`. When absent, behavior is unchanged (two-phase). New output signal is only the existing `end_of_sim_o` timing (no new ports).

- [ ] **Step 1: Add traffic-mode signals + a gated replay task above the directed stimulus `initial` (after line 254)**

```systemverilog
    // Traffic mode (perf sweep): continuous interleaved injection paced by a
    // per-cycle gate. Selected at runtime by +traffic_inj_ratio; absent => the
    // two-phase directed run below is unchanged. Gate uses $urandom_range (PRNG,
    // no constraint solver => no z3). Only AW/AR launches are gated; run_w feeds
    // W freely so each W still follows its own AW.
    localparam int unsigned TRAFFIC_CYCLES = 40000;  // long run; warmup+drain negligible
    real traffic_inj_ratio;
    int  unsigned inj_gate_pct;
    // cap-block-ish instrumentation: cycles where the gate fired but the bus was
    // not ready (fabric backpressure). Reported for the steady-state sanity check.
    longint unsigned ax_launch_attempt = 0;
    longint unsigned ax_launch_done    = 0;

    // Gated, replaying AX launcher. `is_write` picks aw_queue+send_aw vs
    // ar_queue+send_ar. `start_off` rotates the replay start so nodes are not in
    // lockstep. Loops until run_done (set by the fixed-window timer).
    task automatic gated_run_ax(input bit is_write, input int unsigned start_off);
        int unsigned idx;
        idx = start_off;
        forever begin
            if (run_done) return;
            // gate: idle one cycle unless the coin is under the ratio
            if (($urandom_range(0, 99)) >= inj_gate_pct) begin
                @(posedge clk_i);
            end else begin
                ax_launch_attempt++;
                if (is_write) begin
                    if (file_master.aw_queue.size() == 0) @(posedge clk_i);
                    else file_master.drv.send_aw(file_master.aw_queue[idx % file_master.aw_queue.size()]);
                end else begin
                    if (file_master.ar_queue.size() == 0) @(posedge clk_i);
                    else file_master.drv.send_ar(file_master.ar_queue[idx % file_master.ar_queue.size()]);
                end
                ax_launch_done++;
                idx++;
            end
        end
    endtask
```

- [ ] **Step 2: Branch the directed stimulus `initial` on the plusarg (replace the two-phase fork at lines 261-264)**

Replace:

```systemverilog
        @(posedge rst_ni);
        fork file_master.run_aw(); file_master.run_w(); file_master.wait_b(); join
        fork file_master.run_ar(); file_master.wait_r(); join
        run_done = 1'b1;
```

with:

```systemverilog
        @(posedge rst_ni);
        if ($value$plusargs("traffic_inj_ratio=%f", traffic_inj_ratio)) begin
            inj_gate_pct = int'(traffic_inj_ratio * 100.0);
            fork
                gated_run_ax(1'b1, NODE_ID);              // writes, rotated start
                gated_run_ax(1'b0, NODE_ID + 1);          // reads, rotated start
                file_master.run_w();                      // W follows AW freely
            join_none
            repeat (TRAFFIC_CYCLES) @(posedge clk_i);     // fixed steady window
            run_done = 1'b1;
        end else begin
            fork file_master.run_aw(); file_master.run_w(); file_master.wait_b(); join
            fork file_master.run_ar(); file_master.wait_r(); join
            run_done = 1'b1;
        end
```

- [ ] **Step 3: Build the directed flavor**

Run (WSL): `cd sim/verilator && make TBTOP_EXE RUN_CLASS=directed TOPOLOGY=mesh_4x4_vc1`
Expected: Verilator build succeeds (no compile error on the new task / plusarg).

- [ ] **Step 4: Smoke-run traffic mode at ratio=1.0 and confirm sustained non-vacuous traffic**

Run (WSL): `cd sim/verilator && make run-directed RUN_CLASS=directed TOPOLOGY=mesh_4x4_vc1 VERILATOR_RUN_EXTRA="+traffic_inj_ratio=1.0"`
Expected: the run reaches `end_of_sim` after ~TRAFFIC_CYCLES, and each node's `axi_bw_monitor` `$display` prints non-zero `read_bw` / `write_bw`. (If `VERILATOR_RUN_EXTRA` is not already a passthrough, add it in Task 2 first.)

- [ ] **Step 5: Commit**

```bash
git add sim/tb/user_node_endpoint.sv
git commit -m "feat(sim): traffic mode - continuous gated injection at +traffic_inj_ratio"
```

---

### Task 2: Sweep run recipe (Makefile)

**Files:**
- Modify: `sim/verilator/Makefile` (add a `run-traffic` target near `run-directed`, ~line 202; add a `+traffic_inj_ratio` passthrough)

**Interfaces:**
- Consumes: the directed build (`RUN_CLASS=directed`), `+stim_dir`, `+perf_out`, the Task 1 `+traffic_inj_ratio`.
- Produces: `make run-traffic RUN_CLASS=directed TOPOLOGY=<t> INJ_RATIO=<f>` runs one sweep point, writing the run log (with the bw_monitor `$display` lines) to `output/traffic_<TOPOLOGY>_r<INJ_RATIO>/run.log` and `perf.json` to the same dir.

- [ ] **Step 1: Add the `run-traffic` target**

```makefile
# Saturation sweep: one operating point at INJ_RATIO on the directed build.
# Traffic mode (user_node_endpoint +traffic_inj_ratio) injects continuously.
INJ_RATIO   ?= 1.0
TRAFFIC_TAG ?= traffic_$(TOPOLOGY)_r$(INJ_RATIO)
.PHONY: run-traffic
run-traffic: $(TBTOP_EXE)
	@[ "$(RUN_CLASS)" = "directed" ] || { echo "ERROR: run-traffic requires RUN_CLASS=directed"; exit 1; }
	@mkdir -p output/$(TRAFFIC_TAG)
	$(PYTHON3) ../tools/gen_test_patterns.py --pattern uniform_random \
	    --topology $(TOPOLOGY) --out $(STIM_ROOT) --format file_master
	./$(TBTOP_EXE) \
	    "+stim_dir=$(STIM_ROOT)" \
	    "+traffic_inj_ratio=$(INJ_RATIO)" \
	    "+perf_out=output/$(TRAFFIC_TAG)/perf.json" \
	    2>&1 | tee output/$(TRAFFIC_TAG)/run.log
```

(Match the exact `gen_test_patterns.py` flags and `STIM_ROOT` / `TBTOP_EXE` variables already used by the `run-directed` recipe at lines 203-219. Copy that recipe's stimulus-gen line verbatim and only swap the pattern to `uniform_random` and add `+traffic_inj_ratio`.)

- [ ] **Step 2: Run one point end-to-end**

Run (WSL): `cd sim/verilator && make run-traffic RUN_CLASS=directed TOPOLOGY=mesh_4x4_vc1 INJ_RATIO=1.0`
Expected: `output/traffic_mesh_4x4_vc1_r1.0/run.log` exists and contains per-node `axi_bw_monitor` lines with non-zero bandwidth.

- [ ] **Step 3: Commit**

```bash
git add sim/verilator/Makefile
git commit -m "feat(sim): run-traffic recipe for saturation sweep (one point per INJ_RATIO)"
```

---

### Task 3: Collector — parse accepted throughput into a CSV

**Files:**
- Create: `sim/tools/collect_saturation.py`
- Test: `sim/tools/test_collect_saturation.py`

**Interfaces:**
- Consumes: run logs at `sim/verilator/output/traffic_<topo>_r<ratio>/run.log` containing `axi_bw_monitor` `$display` lines. (Inspect one real log from Task 2 to get the exact line format before writing the parser. The monitor prints per-`Name` bandwidth; `Name` is `node<N>.manager`.)
- Produces: `parse_bw(log_text) -> dict` returning `{"read_bw": float, "write_bw": float}` aggregated (summed) across nodes, and a `main()` that loops the vc configs x ratios, runs `make run-traffic`, and writes `saturation.csv` with columns `vc,pattern,inj_ratio,accepted_bits_per_cyc,read_bits_per_cyc,write_bits_per_cyc`.

- [ ] **Step 1: Capture the real monitor line format**

Run (WSL): `grep -iE "bw|bandwidth|node[0-9]+.manager" sim/verilator/output/traffic_mesh_4x4_vc1_r1.0/run.log | head`
Record the exact format. Use it in the parser regex below (adjust the pattern to match).

- [ ] **Step 2: Write the failing parser test**

```python
# sim/tools/test_collect_saturation.py
from collect_saturation import parse_bw

def test_parse_bw_sums_nodes():
    # Replace this sample with two real lines captured in Step 1.
    log = (
        "node0.manager: read_bw = 4.0 bits/cyc, write_bw = 2.0 bits/cyc\n"
        "node1.manager: read_bw = 3.0 bits/cyc, write_bw = 1.0 bits/cyc\n"
    )
    out = parse_bw(log)
    assert out["read_bw"] == 7.0
    assert out["write_bw"] == 3.0
```

- [ ] **Step 3: Run the test, expect failure**

Run: `cd sim/tools && python3 -m pytest test_collect_saturation.py -v`
Expected: FAIL (`collect_saturation` not found).

- [ ] **Step 4: Write the parser (regex adjusted to the real Step-1 format)**

```python
# sim/tools/collect_saturation.py
import re, sys

_BW = re.compile(r"read_bw\s*=\s*([\d.]+).*?write_bw\s*=\s*([\d.]+)", re.I)

def parse_bw(log_text: str) -> dict:
    read = write = 0.0
    for m in _BW.finditer(log_text):
        read += float(m.group(1))
        write += float(m.group(2))
    return {"read_bw": read, "write_bw": write}
```

- [ ] **Step 5: Run the test, expect pass**

Run: `cd sim/tools && python3 -m pytest test_collect_saturation.py -v`
Expected: PASS.

- [ ] **Step 6: Add the sweep `main()` (runs make per point, writes CSV)**

```python
VC_CONFIGS = ["mesh_4x4_vc1", "mesh_4x4_vc2", "mesh_4x4_vc4", "mesh_4x4_vc8"]
RATIOS = [0.7, 0.85, 1.0]

def main():
    import subprocess, pathlib
    rows = ["vc,pattern,inj_ratio,accepted_bits_per_cyc,read_bits_per_cyc,write_bits_per_cyc"]
    for topo in VC_CONFIGS:
        for r in RATIOS:
            subprocess.run(["make", "run-traffic", "RUN_CLASS=directed",
                            f"TOPOLOGY={topo}", f"INJ_RATIO={r}"],
                           cwd="../verilator", check=True)
            log = pathlib.Path(f"../verilator/output/traffic_{topo}_r{r}/run.log").read_text()
            bw = parse_bw(log)
            acc = bw["read_bw"] + bw["write_bw"]
            rows.append(f"{topo},uniform_random,{r},{acc},{bw['read_bw']},{bw['write_bw']}")
    pathlib.Path("saturation.csv").write_text("\n".join(rows) + "\n")
    print("wrote saturation.csv")

if __name__ == "__main__":
    main()
```

- [ ] **Step 7: Commit**

```bash
git add sim/tools/collect_saturation.py sim/tools/test_collect_saturation.py
git commit -m "feat(sim): saturation collector - parse bw_monitor, sweep vc x ratio to CSV"
```

---

### Task 4: Bar chart + verification

**Files:**
- Create: `sim/tools/plot_saturation.py` (throwaway)

**Interfaces:**
- Consumes: `sim/tools/saturation.csv` from Task 3.
- Produces: `saturation.png` (bar per VC config at the plateau ratio) and a printed table. No plotting framework beyond a single matplotlib call, guarded so a missing matplotlib still prints the table.

- [ ] **Step 1: Run the full sweep**

Run (WSL): `cd sim/tools && python3 collect_saturation.py`
Expected: `saturation.csv` with 12 rows (4 vc x 3 ratios).

- [ ] **Step 2: Plateau + steady-state check (verification, booksim2-style)**

For each vc, confirm accepted throughput is flat across ratios 0.85 and 1.0 (within a few percent). If accepted still rises from 0.85 to 1.0, the point is not yet saturated. Record the plateau value as the saturation throughput.

Run: `cd sim/tools && python3 -c "import csv;[print(r) for r in csv.DictReader(open('saturation.csv'))]"`
Expected: for each vc, `accepted_bits_per_cyc` at r=0.85 and r=1.0 differ by < ~5%.

- [ ] **Step 3: Cross-check the monitor against perf.json (validate the trusted-vs-AI tools)**

At one point (vc1, r=1.0), compare the bw_monitor accepted bytes against the `perf.json` write/read byte counts over the same window.

Run: `cd sim/verilator && python3 -c "import json;d=json.load(open('output/traffic_mesh_4x4_vc1_r1.0/perf.json'));print([ (s['name'], s.get('write_byte_count'), s.get('read_byte_count')) for s in d['axi_slots']])"`
Expected: the perf.json aggregate bytes / window cycles is within a few percent of the monitor's accepted throughput. Agreement validates both readouts; a gap flags which to trust (record the finding).

- [ ] **Step 4: Write the bar-chart script**

```python
# sim/tools/plot_saturation.py
import csv

def saturation_per_vc(csv_path="saturation.csv", plateau_ratio=1.0):
    out = {}
    for row in csv.DictReader(open(csv_path)):
        if abs(float(row["inj_ratio"]) - plateau_ratio) < 1e-6:
            out[row["vc"]] = float(row["accepted_bits_per_cyc"])
    return out

def main():
    sat = saturation_per_vc()
    for vc, v in sat.items():
        print(f"{vc}: saturation throughput = {v:.2f} bits/cyc")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.bar(list(sat.keys()), list(sat.values()))
        plt.ylabel("saturation throughput (bits/cyc)")
        plt.title("Saturation throughput per VC config (4x4, uniform_random)")
        plt.savefig("saturation.png")
        print("wrote saturation.png")
    except ImportError:
        print("(matplotlib absent; table above is the deliverable)")

if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Render + read the result**

Run: `cd sim/tools && python3 plot_saturation.py`
Expected: prints one saturation number per vc; vc8 > vc1 (VC value visible), or a recorded finding if not.

- [ ] **Step 6: Commit**

```bash
git add sim/tools/plot_saturation.py
git commit -m "feat(sim): saturation bar chart + verification (plateau, perf.json cross-check)"
```

---

## Self-review notes

- Spec coverage: injection gate (Task 1), reuse u_bw_mst / link_perf_monitor untouched (Task 1 constraints), sweep + CSV (Tasks 2-3), bar chart (Task 4), verification incl perf.json cross-check + plateau + VC separation (Task 4). Offered-load-as-payload-bits is folded into the accepted-throughput readout (bw_monitor reports bits/cyc directly), so the x-axis-quantity concern is handled by measuring accepted bits rather than launch counts.
- Known implementation risk (Task 1): the exact `send_aw`/`send_ar` blocking semantics and `w_queue` ordering under replay may need tuning against `make sim`. The verification step (non-zero sustained bw) is the gate. If W ordering desyncs the scoreboard, note the scoreboard is not the goal here and can be left disabled in traffic mode.
- Numbers marked to tune during bring-up: `TRAFFIC_CYCLES` (40000), `RATIOS` ({0.7,0.85,1.0}). Adjust once the first run shows the fill time and the plateau location.
