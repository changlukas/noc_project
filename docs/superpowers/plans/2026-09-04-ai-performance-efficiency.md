# AI NoC Performance Efficiency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the AI NoC report with standard performance metrics, fixed maximum source concurrency, separate Burst Length characterization, fair multicast and RR/RRD comparisons, and measured-set Pareto candidates.

**Architecture:** Extend the existing Mode 4, Read prefill, CSV, and report paths. Add one common workload measurement window and attribution-quality Router counters before collecting new data. Reuse the AI mappings in `gen_test_patterns.py`; do not create another traffic model or aggregate performance score.

**Tech Stack:** C++17 reference model, SystemVerilog/Verilator co-simulation, DPI-C, Python 3 standard library, GNU Make, GoogleTest, pytest, specgen YAML parameters.

**Spec:** `docs/superpowers/specs/2026-09-04-ai-performance-efficiency-design.md`

## Global Constraints

- Main native-width runs use a 4x4 mesh, XY routing, 512-bit AXI data, 64 B/beat, 4096 B/flow/round, 16 rounds, and separate Read and Write runs.
- Characterize Burst Length over 1, 4, 16, and 64 beats on the baseline DUT configuration while keeping 4096 B/flow/round. The corresponding transaction counts per flow/round are 64, 16, 4, and 1.
- Fix source Outstanding Depth and Max Transactions/ID at 32 transactions/initiator for every performance run.
- Do not generate Broadcast Read. Read requests reverse the payload edge and use the existing memory/checker prefill path.
- Use only established metric names. Burst Length is a workload variable, Outstanding Depth is a fixed workload condition, and neither is a DUT configuration candidate. RR and RRD are the only project aliases.
- All throughput, flit, utilization, stall, occupancy, and Completion Time values use the same workload start and end cycles.
- Do not reuse result CSV files created before the common-window schema. Do not mix 64-bit RR/RRD results with native 512-bit throughput results.
- Do not select a final configuration without approved Area, Power, and Timing limits. Report measured-set Pareto candidates only.
- Do not add dependencies. Preserve all unrelated worktree changes and stage exact paths or hunks for commits.

---

### Task 1: Make the workload window authoritative

**Files:**

- Modify: `ref_model/c_model/include/wrap/perf_collector.hpp`
- Modify: `ref_model/c_model/tests/common/test_perf_collector.cpp`
- Modify: `ref_model/dpi/cmodel_dpi.cpp`
- Modify: `ref_model/dpi/cmodel_dpi.h`
- Modify: `ref_model/top/noc_fabric.sv`
- Modify: `sim/tb/link_perf_monitor.sv`
- Modify: `sim/tb/noc_tb_top.sv`
- Modify: `sim/tb/test/user_node_endpoint.sv`
- Modify: `sim/tools/emit_result_csv.py`
- Modify: `sim/tools/gen_tb_top.py`
- Modify: `sim/tools/test_emit_result_csv.py`
- Modify: `sim/tb/noc_tb_top.sv`
- Modify: `sim/tb/test/user_node_endpoint.sv`
- Modify: `sim/tools/test_channel_compare_fault.py`
- Modify: `sim/tools/test_outstanding_injection_mode.py`
- Modify: `sim/tools/test_gen_tb_top.py`
- Modify: `sim/tools/test_outstanding_injection_mode.py`

**Interfaces:**

- Add `cmodel_perf_begin(long long start_cyc)` and `cmodel_perf_end(long long end_cyc)`.
- `PerfCollector::begin(uint64_t)` clears run counters/HWM and enables sampling.
- `PerfCollector::end(uint64_t)` disables sampling and closes the window.
- `link_perf_monitor` receives `measure_en`; it resets counters before the window and increments only while enabled.
- Mode 4 sources wait at one common barrier after file loading and Read prefill. The top opens the window before releasing the barrier and closes it when every active source reports its final B or RLAST completion.

- [ ] **Step 1: Write failing collector lifecycle tests**

  Add GoogleTest cases proving that samples before `begin()` and after `end()` are ignored, `begin()` clears an earlier run, and JSON emits the exact non-zero start/end cycles.

- [ ] **Step 2: Run the collector test and verify RED**

  ```bash
  cmake --build /home/lucas/noc_build/cmodel --target test_perf_collector
  ctest --test-dir /home/lucas/noc_build/cmodel -R test_perf_collector --output-on-failure
  ```

  Expected: compile failure for the missing lifecycle methods.

- [ ] **Step 3: Implement the minimum collector lifecycle**

  Use this public shape:

  ```cpp
  void begin(uint64_t start_cyc);
  void end(uint64_t end_cyc);
  bool active() const;
  ```

  `sample_router()` and `set_link()` return immediately when inactive. Reject `end <= start` through the existing DPI error path.

- [ ] **Step 4: Add the Mode 4 start barrier and counter gating**

  Generalize the existing comparison barrier rather than adding a second synchronization framework. A Mode 4 endpoint asserts ready only after stimulus and Read prefill are complete. On one clock edge the top calls `cmodel_perf_begin()`, asserts start to all active endpoints, and enables every link monitor. On the final active-source completion edge it disables monitors and calls `cmodel_perf_end()` before the 100-cycle settle period.

- [ ] **Step 5: Gate CSV acceptance on a common window**

  Require `perf.json.window.start_cyc` to equal the common source-start marker and `end_cyc` to equal the last active-source completion marker. Reject cycle-zero windows for Mode 4. Derive Accepted Throughput and Link Utilization only from counters inside this window.

- [ ] **Step 6: Run focused tests**

  ```bash
  python -m pytest sim/tools/test_emit_result_csv.py sim/tools/test_outstanding_injection_mode.py -q
  cmake --build /home/lucas/noc_build/cmodel --target test_perf_collector
  ctest --test-dir /home/lucas/noc_build/cmodel -R test_perf_collector --output-on-failure
  ```

  Expected: all focused tests pass; a fixture whose counter window includes settle cycles is rejected.

- [ ] **Step 7: Commit**

  ```bash
  git add ref_model/c_model/include/wrap/perf_collector.hpp ref_model/c_model/tests/common/test_perf_collector.cpp ref_model/dpi/cmodel_dpi.cpp ref_model/dpi/cmodel_dpi.h ref_model/top/noc_fabric.sv sim/tb/link_perf_monitor.sv sim/tb/noc_tb_top.sv sim/tb/test/user_node_endpoint.sv sim/tools/emit_result_csv.py sim/tools/gen_tb_top.py sim/tools/test_emit_result_csv.py sim/tools/test_gen_tb_top.py sim/tools/test_outstanding_injection_mode.py
  git commit -m "perf(sim): align counters to workload window"
  ```

---

### Task 2: Add attribution-quality Router diagnostics

**Files:**

- Modify: `ref_model/c_model/include/router/router.hpp`
- Modify: `ref_model/c_model/tests/router/test_router.cpp`
- Modify: `ref_model/c_model/include/wrap/perf_collector.hpp`
- Modify: `ref_model/c_model/tests/common/test_perf_collector.cpp`
- Modify: `ref_model/dpi/cmodel_dpi.cpp`
- Modify: `sim/tools/emit_result_csv.py`
- Modify: `sim/tools/test_emit_result_csv.py`

**Interfaces:**

- Replace aggregate DAT input occupancy with `noc.router_dat_input_vcs[]` entries `{router, port, vc, hwm_flits, capacity_flits}`. `port` uses `LOCAL`, `NORTH`, `EAST`, `SOUTH`, or `WEST`.
- Add `noc.router_dat_output_vcs[]` entries `{router, port, vc, credit_block_cycles}` with the same port names. A credit-block event is true only when an eligible head/body flit requests that output VC and its credit is zero.
- Expose that current-cycle event as `bool output_vc_credit_blocked(std::size_t out_port, uint8_t vc) const`.
- Keep REQ/RSP `valid && !ready` stall accounting unchanged.
- Stop using DAT `!valid && any_vc_zero` as evidence for buffer sizing.

- [ ] **Step 1: Write failing Router event tests**

  Cover three cases: an unrelated VC at zero credit does not count, an idle output does not count, and a requesting worm blocked on its selected zero-credit VC counts exactly once for that cycle.

- [ ] **Step 2: Run Router tests and verify RED**

  ```bash
  cmake --build /home/lucas/noc_build/cmodel --target test_router
  ctest --test-dir /home/lucas/noc_build/cmodel -R test_router --output-on-failure
  ```

  Expected: compile failure for the missing diagnostic accessor.

- [ ] **Step 3: Implement read-only Router diagnostic accessors**

  Add no new queue or arbitration state. Derive the event from the existing input front, wormhole owner, route, and `credit_[out][vc]` state after allocation eligibility is known. Expose the event and `input_fifo_size(port, vc)` through the existing wrap/DPI sampling path.

- [ ] **Step 4: Emit structured per-queue JSON and CSV summaries**

  `perf.json` keeps input occupancy and output credit blocking in the two arrays above. `result.csv` records `router_dat_input_vc_hwm_max_flits`, `router_dat_input_vc_capacity_flits`, and `router_dat_output_vc_credit_block_cycles_sum` while retaining the detailed JSON for localization.

- [ ] **Step 5: Run focused tests**

  ```bash
  python -m pytest sim/tools/test_emit_result_csv.py -q
  cmake --build /home/lucas/noc_build/cmodel --target test_router test_perf_collector
  ctest --test-dir /home/lucas/noc_build/cmodel -R 'test_router|test_perf_collector' --output-on-failure
  ```

  Expected: all tests pass and no test treats an idle zero-credit VC as a stall.

- [ ] **Step 6: Commit**

  ```bash
  git add ref_model/c_model/include/router/router.hpp ref_model/c_model/tests/router/test_router.cpp ref_model/c_model/include/wrap/perf_collector.hpp ref_model/c_model/tests/common/test_perf_collector.cpp ref_model/dpi/cmodel_dpi.cpp sim/tools/emit_result_csv.py sim/tools/test_emit_result_csv.py
  git commit -m "perf(router): expose per-vc occupancy and credit blocking"
  ```

---

### Task 3: Calculate the standard throughput bound

**Files:**

- Modify: `ref_model/top/noc_fabric.sv`
- Modify: `sim/tools/gen_test_patterns.py`
- Modify: `sim/tools/pattern_metrics.py`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py`
- Modify: `sim/tools/test_outstanding_injection_mode.py`
- Modify: `sim/tools/test_pattern_metrics.py`
- Modify: `sim/tools/perf_report.py`
- Modify: `sim/tools/test_perf_report.py`

**Interfaces:**

- Reuse `mcast_groups()`, `reverse_payload_edges()`, and the AI destination helpers from `sim/tools/gen_test_patterns.py`.
- Add `ai_resource_flits(mapping, direction, burst_beats, rounds, multicast_mode)` returning `{resource_name: flit_count}`.
- Resource names distinguish `req|rsp|dat`, source injection, directed mesh link, and destination ejection.
- Add `ideal_throughput_bound(...)` returning logical delivered bytes divided by the maximum resource serialization cycles.
- Native mapping is AW/W on DAT, AR on REQ, B on RSP, and R on DAT. Hardware multicast enumerates its fork tree and CollectB join tree separately.

- [ ] **Step 1: Write failing analytic tests**

  Assert exact resource counts for one Pipeline Write, one Pipeline Read reverse path, one Global Gather, and Row/Column/Local 2x2/Global Broadcast. Assert hardware multicast injects one AW/W stream per producer while repeated unicast injects one per remote destination.

- [ ] **Step 2: Run analytic tests and verify RED**

  ```bash
  python -m pytest sim/tools/test_pattern_metrics.py -q
  ```

  Expected: failure for the missing AI resource-load functions.

- [ ] **Step 3: Implement the minimum resource enumerator**

  Extend the existing XY link walker. Keep one dictionary of resource counts; do not create a graph framework. For multicast, port the fixed 4x4 fork/join edge semantics used by `route_mask_fork()` and `route_mask_join()` and cover every approved shape with golden tests.

- [ ] **Step 4: Cross-check the analytic model against emitted counters**

  Add NI-to-Router injection and Router-to-NI ejection monitors in `noc_fabric.sv`, named exactly like the analytic resources. Add the already-approved repeated-unicast Broadcast generator needed by this check. Capture one deterministic no-stall directed Write, Read, hardware-multicast, and repeated-unicast fixture from emitted counters; keep fixture values literal and independent of `ai_resource_flits()`. Require exact per-plane source, link, destination, and logical-delivery counts before accepting `% of Ideal Throughput`.

- [ ] **Step 5: Render the standard metric columns**

  The main table contains only:

  ```text
  Communication Type
  Direction
  Ideal Throughput Bound (B/cycle)
  Accepted Throughput at Outstanding Depth 32 (B/cycle)
  % of Ideal Throughput
  ```

  Do not restore an Outstanding sweep, 95%-Outstanding, saturation-depth, or aggregate efficiency column.

- [ ] **Step 6: Run focused tests and commit**

  ```bash
  python -m pytest sim/tools/test_pattern_metrics.py sim/tools/test_perf_report.py -q
  git add ref_model/top/noc_fabric.sv sim/tools/gen_test_patterns.py sim/tools/pattern_metrics.py sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_outstanding_injection_mode.py sim/tools/test_pattern_metrics.py sim/tools/perf_report.py sim/tools/test_perf_report.py
  git commit -m "feat(perf): compare accepted throughput with ideal bound"
  ```

---

### Task 4: Add workload characterization and DUT configuration sweeps

**Files:**

- Modify: `sim/tools/gen_test_patterns.py`
- Modify: `sim/tools/emit_result_csv.py`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py`
- Modify: `sim/tools/test_emit_result_csv.py`
- Modify: `sim/verilator/Makefile`
- Modify: `sim/tools/run_vc_buffer_tradeoff.py`
- Modify: `sim/tools/test_run_vc_buffer_tradeoff.py`
- Modify: `sim/tools/perf_report.py`
- Modify: `sim/tools/test_perf_report.py`

**Interfaces:**

- Burst Length characterization: baseline DUT configuration with Broadcast Global Write, Gather Global Read/Write, All-to-All Read/Write, and Pipeline P2P Read/Write; Burst Length `{1,4,16,64}` beats and Outstanding Depth 32.
- Multicast comparison: Row, Column, Local 2x2, and Global Broadcast Write; hardware multicast versus repeated unicast with identical producers, destinations, issue order, AXI-ID policy, Outstanding Depth 32, and 4096 B/flow/round.
- Record `multicast_mode` as `hardware` or `repeated_unicast`. Require `payload_deliveries > source_requests` only for hardware multicast; repeated unicast requires one source request per remote payload delivery.
- RR/RRD comparison: Pipeline P2P only, matched Read/Write directions, Outstanding Depth 32, one AXI ID and one destination per initiator, 16 rounds, and 64 Control probes.
- RR/RRD uses 8 B/beat and two 256-beat transactions per flow/round. Main throughput sweeps remain native 64 B/beat.
- Mode 3 derives expected background burst and beat counts from the loaded stimulus instead of fixed node/count constants. Runtime markers must prove every background interval contains the Control probe interval. Traffic metadata names the shared directed geometric edge; RR requires Control and background activity on its REQ/RSP resource, while RRD requires Control on REQ/RSP and background DAT activity on that same edge.

- [ ] **Step 1: Write failing fixed-payload Burst tests**

  For each Burst Length assert `beats * transactions_per_flow == 64`, exact 4096 B/flow/round, and unchanged payload edges. Reject any run row whose generated traffic metadata disagrees.

- [ ] **Step 2: Expose the completed repeated-unicast Broadcast generation**

  Reuse Task 3's tested repeated-unicast generator. Expose it through the campaign CLI/Make target without changing its producer/member sets, local-member policy, or deterministic member order.

- [ ] **Step 3: Replace directed channel comparison traffic with Pipeline P2P**

  Keep the existing runtime `2-channel` and `3-channel` mappings. Generate Pipeline P2P background traffic, matched direction, two `AxLEN=255`, `AxSIZE=3` transactions per flow/round, one ID per initiator, and 64 node-0 Control probes. Reject a run unless RR proves shared directed REQ/RSP use and RRD proves the same geometric DAT edge plus full temporal overlap.

- [ ] **Step 4: Add isolated Make targets and result roots**

  Add targets `sim-ai-burst-sweep`, `sim-ai-multicast-compare`, and `sim-ai-rr-rrd`. Store results only under `output/burst`, `output/multicast_compare`, and `output/rr_vs_rrd`. Tags include traffic, direction, Burst Length, fixed Outstanding Depth 32, implementation/mapping, and recorded seed.

- [ ] **Step 5: Keep VC and NI depth coupling explicit**

  In `run_vc_buffer_tradeoff.py`, configure `NI_DAT_RX_VC_DEPTH` with `ROUTER_VC_DEPTH` and run every candidate at Outstanding Depth 32. Record Router DAT, NI RX DAT, fixed NI TX, and Read RoB storage separately. Keep NI TX depth fixed; do not open a sweep without measured TX-full admission stalls.

- [ ] **Step 6: Run focused tests and commit**

  ```bash
  python -m pytest sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_run_vc_buffer_tradeoff.py sim/tools/test_perf_report.py -q
  git add sim/tools/gen_test_patterns.py sim/tools/emit_result_csv.py sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_emit_result_csv.py sim/tb/noc_tb_top.sv sim/tb/test/user_node_endpoint.sv sim/tools/test_channel_compare_fault.py sim/tools/test_outstanding_injection_mode.py sim/verilator/Makefile sim/tools/run_vc_buffer_tradeoff.py sim/tools/test_run_vc_buffer_tradeoff.py sim/tools/perf_report.py sim/tools/test_perf_report.py
  git commit -m "feat(perf): add AI traffic comparison sweeps"
  ```

---

### Task 5: Run the clean campaign and publish Pareto results

**Files:**

- Modify: `docs/noc-performance-parameters.md`
- Modify: `docs/noc-workload-benchmark.md`
- Modify: `docs/superpowers/specs/2026-08-28-channel-count-latency-design.md`
- Modify: `docs/backlog.md`
- Generate: `sim/verilator/output/perf_report.md`
- Generate: `sim/verilator/output/*.svg`
- Generate: `sim/verilator/output/{baseline,burst,multicast_compare,rr_vs_rrd,tradeoff}/`

**Interfaces:**

- A result manifest records git revision, config hash, generated-parameter hash, simulator version,
  seed, exact command, and the path and SHA-256 of the campaign `source.patch`.
- Pareto comparison treats Accepted Throughput and Completion Time per workload as separate objectives. Storage categories remain separate: Router DAT, NI RX DAT, NI TX, and Read RoB.
- The report states which workload limits each candidate. It leaves final recommendation unset until approved synthesis limits exist.

- [ ] **Step 1: Verify and clean exact output targets**

  Confirm the resolved repository and Verilator build roots. Run the documented pre-clean for generated filelists/tops and `/home/lucas/noc_build/verilator/obj_dir_*`, then run:

  ```bash
  make -C sim/verilator clean
  python specgen/tools/codegen.py --check
  ```

  Expected: the old output campaign is removed, generated parameters match sources, and no unrelated workspace path is deleted.

- [ ] **Step 2: Run functional gates before the matrix**

  ```bash
  python -m pytest sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_outstanding_injection_mode.py sim/tools/test_emit_result_csv.py sim/tools/test_emit_result_manifest.py sim/tools/test_pattern_metrics.py sim/tools/test_perf_report.py sim/tools/test_run_vc_buffer_tradeoff.py -q
  make -C sim/verilator hello
  ```

  Expected: all tests pass and the toolchain prints `TOOLCHAIN OK`.

- [ ] **Step 3: Run baseline, Burst, multicast, and RR/RRD campaigns**

  Run each target with `CONFIG=mesh_4x4 SEED=1`. Every accepted row must show checker PASS, exact logical bytes, complete responses, a valid common window, and configuration metadata. Stop on the first invalid row instead of continuing with partial data.

- [ ] **Step 4: Run conditional hardware trade-offs**

  Reuse the existing fixed-depth and equal-total-entry VC candidates. Run Read RoB variants only
  when both its HWM reaches the configured limit and a resource-specific non-zero admission-stall
  counter is observed. Keep Max Transactions/ID fixed at 32. Rebuild once per build-time DUT
  configuration and restore shipped defaults in a `finally` path followed by `codegen.py --check`.

- [ ] **Step 5: Confirm Pareto boundary points**

  If the stimulus or arbitration consumes PRNG state, repeat only boundary candidates with paired seeds 1, 2, and 3 and report median plus range. If deterministic, repeat the exact command once and require identical counters.

- [ ] **Step 6: Generate and inspect the report**

  ```bash
  python sim/tools/perf_report.py sim/verilator/output
  git diff --check
  ```

  Verify the report has one standard main performance table at Outstanding Depth 32, Burst Length characterization on the baseline DUT, multicast raw source flits plus Speedup, Pipeline P2P RR/RRD mean Control Completion Time, separate DUT storage costs, Pareto candidates, and no Outstanding sweep, Injection Rate, Offered Load, 95%-Outstanding, or unsupported PPA claim.

- [ ] **Step 7: Apply cross-file consistency updates**

  Update the three listed performance documents together. Use `rg` to list every Markdown reference to the renamed metrics, RR/RRD geometry, `DAT_NUM_VC`, `ROUTER_VC_DEPTH`, and `NI_DAT_RX_VC_DEPTH`; resolve active-document mismatches and leave historical result records unchanged.

- [ ] **Step 8: Run final acceptance and commit**

  ```bash
  python -m pytest sim/tools -q
  python specgen/tools/codegen.py --check
  make -C sim/verilator sim CONFIG=mesh_2x2 PATTERN=neighbor
  git diff --check
  ```

  Expected: pytest and codegen pass, the clean 2x2 smoke reaches non-vacuous scoreboard PASS, and the report is reproducible from only the new result roots.

  ```bash
  git add docs/noc-performance-parameters.md docs/noc-workload-benchmark.md docs/superpowers/specs/2026-08-28-channel-count-latency-design.md docs/backlog.md sim/verilator/output/perf_report.md sim/verilator/output/*.svg
  git commit -m "docs(perf): publish AI NoC efficiency trade-off"
  ```

---

## Stage Mapping for Active Execution

When execution begins, copy these five task goals and success criteria into the repository-root
`IMPLEMENTATION_PLAN.md` without overwriting an unrelated active campaign:

1. **Measurement window:** every performance counter uses the exact workload interval.
2. **Router diagnostics:** per-VC occupancy and selected-credit blocking are attributable.
3. **Throughput bound:** analytic resource counts match emitted counters.
4. **Experiment sweeps:** Burst, multicast, and Pipeline P2P RR/RRD results pass focused gates.
5. **Campaign and report:** clean results produce standard metrics and measured-set Pareto candidates.
