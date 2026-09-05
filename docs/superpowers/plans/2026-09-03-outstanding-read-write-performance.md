# Outstanding-Driven AI Traffic Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Injection-rate-based performance measurement with separate Outstanding-driven Write and Read campaigns for the approved AI communication patterns.

**Architecture:** Extend the existing Mode 4 source window rather than adding another injection engine. Write occupies a slot from AW admission through B; Read occupies a slot from AR handshake through the matching RLAST. Read payloads preserve the approved AI dataflow by issuing reverse-direction unicast requests to prefilled producer memories; Broadcast remains Write-only because the architecture has no multicast Read protocol.

**Tech Stack:** SystemVerilog/Verilator co-simulation, Python 3 standard library, GNU Make, pytest, specgen-generated NoC configuration.

**Spec:** `docs/nmu-spec.md`, `docs/nsu-spec.md`, `docs/router-spec.md`, `docs/noc-performance-parameters.md`, and `docs/superpowers/plans/2026-09-02-ai-inference-traffic.md`.

## Global Constraints

- Use a 4x4 mesh, native AXI width 512 bits, `AxSIZE=6`, burst length 63, 64 beats, 4096 bytes per transaction, and 16 rounds.
- Sweep `SOURCE_OUTSTANDING_DEPTH={1,2,4,8,16,32}`; its unit is accepted transactions per AXI initiator.
- Measure Write and Read in separate runs and separate performance windows.
- Write patterns: Broadcast Row, Broadcast Column, Broadcast Submesh, Broadcast Global, Gather Global, Gather Submesh, All-to-All, Neighbor Exchange, Pipeline P2P, and Regional Exchange.
- Read patterns: Gather Global, Gather Submesh, All-to-All, Neighbor Exchange, Pipeline P2P, and Regional Exchange. Do not generate, measure, plot, or summarize Broadcast Read.
- Read uses unicast AR requests. The request travels consumer-to-producer; returned R payload follows the approved producer-to-consumer AI dataflow.
- Prefill physical memory and checker golden data before the performance window. Prefill traffic and cycles are excluded from every metric.
- Remove Injection rate and Offered load from the generated report, tables, plots, run labels, and conclusions for this campaign.
- Keep RR/RRD as the previously approved 64-bit common-payload latency comparison. Do not present it as native 512-bit bandwidth or include it in the main Outstanding campaign.
- Do not claim synthesis area, power, frequency, or PPA. Buffer entries are a storage proxy only.

---

### Task 1: Qualify Outstanding-driven Read

**Files:**

- Modify: `sim/tb/test/user_node_endpoint.sv`
- Modify: `sim/tools/test_outstanding_injection_mode.py`
- Test: existing focused Verilator harness invoked by `sim/tools/test_outstanding_injection_mode.py`

**Interfaces:**

- `SOURCE_OUTSTANDING_DEPTH` remains the single Mode 4 window-size parameter.
- Write occupancy remains `AW admitted - B completed`.
- Read occupancy is `AR transferred - matching RLAST transferred`.
- Read and Write counters are direction-specific; a Read run must not consume AW/W/B counters.

- [x] **Step 1: Add failing Read-window contract tests**

  Require Mode 4 to accept a Read-only stimulus, reject mixed Read/Write stimulus, and expose direction-specific accepted, completed, and high-water counters.

- [x] **Step 2: Verify the tests fail for the current Write-only fatal path**

  ```text
  python -m pytest sim/tools/test_outstanding_injection_mode.py -q
  ```

  Expected: FAIL because Mode 4 currently rejects `num_reads != 0`.

- [x] **Step 3: Implement the minimum Read slot controller**

  Add the Read branch beside the existing Write branch. Increment occupancy only on `ARVALID && ARREADY`; decrement only on the corresponding `RVALID && RREADY && RLAST`. Preserve modes 0-3 and the existing Write behavior.

- [x] **Step 4: Add protocol and non-vacuity checks**

  Check positive configured depth, observed high-water mark not exceeding it, exact request/completion counts, no unexpected R response, and zero Write transfers during a Read-only run.

- [x] **Step 5: Run the focused test**

  ```text
  python -m pytest sim/tools/test_outstanding_injection_mode.py -q
  ```

  Expected: PASS for Outstanding depths 1, 2, 4, 8, 16, and 32.

### Task 2: Generate and verify prefilled Read traffic

**Files:**

- Modify: `sim/tools/gen_test_patterns.py`
- Modify: `sim/verilator/Makefile`
- Modify: `sim/tb/test/user_node_endpoint.sv`
- Modify: focused generator tests adjacent to the existing AI-pattern tests

**Interfaces:**

- Add one direction selector with values `write` and `read`; default remains `write` for existing callers.
- The generator emits the same producer/consumer mapping for both directions. Read reverses only the AXI request edge.
- Each producer receives a generated `memory_init.txt` containing global AXI addresses and deterministic expected bytes.
- The static endpoint applies that file to its physical `axi_sim_mem.mem`; each Read initiator preloads the same address-derived bytes into its existing scoreboard before measurement.

- [x] **Step 1: Add failing mapping tests**

  For each Read pattern, assert that the R payload edge matches the approved Write dataflow. Assert that requesting Broadcast with `direction=read` is rejected with a clear diagnostic.

- [x] **Step 2: Add failing prefill/checker tests**

  Require every Read transaction to have one physical-memory preload and one matching checker preload. Require deterministic non-zero data derived from address and beat offset.

- [x] **Step 3: Extend the existing generators**

  Reuse the current destination-list helpers and generated-top `mem_poke` mechanism. Do not add a second traffic description format. Emit AR-only node stimulus for Read and AW/W-only stimulus for Write.

- [x] **Step 4: Strengthen Read completion checking**

  Require correct RID, RRESP, address-derived data, exactly 64 accepted R beats, and RLAST only on beat 63. Add one intentional-corruption negative case and require it to fail.

- [x] **Step 5: Run generator and focused co-sim tests**

  ```text
  python -m pytest sim/tools/test_outstanding_injection_mode.py -q
  ```

  Expected: all six Read mappings pass with non-zero checked bytes; the corruption case fails for the expected data mismatch.

### Task 3: Replace report load semantics and collect the clean baseline

**Files:**

- Modify: `sim/tools/emit_result_csv.py`
- Modify: `sim/tools/perf_report.py`
- Modify: `sim/tools/test_emit_result_csv.py`
- Modify: `sim/tools/test_perf_report.py`
- Modify: `sim/verilator/Makefile`
- Generate: `sim/verilator/output/perf_report.md`
- Generate: `sim/verilator/output/<run-label>/result.csv`

**Interfaces:**

- Human-facing run label: `m4_<pattern>_<r|w>_v<VC>_b<depth>_o<outstanding>`.
- Each CSV row records direction, producer count, consumer count, AXI initiator count, source requests, payload deliveries, configured Outstanding, observed source HWM, 64 beats, 4096 bytes, 16 rounds, completion cycles, completion latency, delivered payload bandwidth, DAT-link utilization distribution, and checker PASS/FAIL.
- `knee outstanding` is the smallest measured Outstanding whose delivered payload bandwidth reaches at least 95% of that configuration's measured maximum. It is not a confidence interval and uses no interpolation.
- `Completion latency @ O=1` replaces low-load/zero-load wording.

- [x] **Step 1: Add failing CSV/report contract tests**

  Reject rows containing Injection rate or Offered load in the Outstanding result set. Reject mixed geometry, missing direction, missing checker evidence, and Broadcast Read. Require all displayed units.

- [x] **Step 2: Implement direction-aware collection and report rendering**

  Render Write and Read as separate rows. Show Completion latency at O=1 `[cycles/transaction]`, peak delivered payload bandwidth `[B/cycle]`, knee Outstanding `[transactions/source]`, busiest DAT-link utilization `[%]`, high-load link counts, and relevant source/NMU/NSU HWM with explicit units.

- [x] **Step 3: Preserve Compute overlap coverage without inventing a PE latency**

  Compute it only when an explicit PE compute budget in cycles is supplied. Otherwise omit the value and state that the report has no selected PE compute budget; do not print an estimated percentage.

- [x] **Step 4: Perform the approved clean boundary**

  Resolve and verify the repository path, then run the repository's documented `make clean`. Do not reuse any historical CSV. Regenerate required sources and require `python specgen/tools/codegen.py --check` to pass before the campaign.

- [x] **Step 5: Run the 96-cell baseline**

  Run 10 Write patterns and 6 Read patterns at Outstanding 1, 2, 4, 8, 16, and 32. Every accepted row must report 4096 bytes per transaction, scoreboard PASS, no timeout or protocol fatal, and observed HWM not exceeding the configured Outstanding.

- [x] **Step 6: Generate and verify the report**

  ```text
  python -m pytest sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py -q
  python sim/tools/perf_report.py sim/verilator/output
  git diff --check
  ```

  Expected: tests pass and the report contains no Injection rate, Offered load, estimated saturation, or Broadcast Read.

### Task 4: Re-run the VC/buffer trade-off with capacity controls

**Files:**

- Temporarily modify and restore: `specgen/source/constants.yaml`
- Modify only if required for HWM visibility: NSU model/wrap/DPI/log/CSV path associated with `MetaBuffer` occupancy
- Modify: `docs/noc-performance-parameters.md`
- Modify: `docs/nsu-spec.md`
- Modify: `docs/backlog.md`
- Modify: `IMPLEMENTATION_PLAN.md`
- Generate: `sim/verilator/output/vc_buffer/`

**Interfaces:**

- Stress mappings are Broadcast Global Write, Gather Global Write/Read, and Regional Exchange Write/Read. There is no Broadcast Read cell.
- Fixed-depth screen: `(VC, Router depth)=(1,8),(2,8),(4,8),(8,8)`.
- Equal-total-entry slices: 32 entries/input uses `(1,32),(2,16),(4,8)`; 64 entries/input uses `(2,32),(4,16),(8,8)`.
- Storage proxy is `DAT_NUM_VC × NOC_ROUTER_VC_DEPTH` entries per Router input.
- NI capacity knobs use one-factor-at-a-time screening followed by one selected combined confirmation; never run a 3×3×3 full factorial.

- [x] **Step 1: Reconcile authoritative defaults before changing parameters**

  Make `docs/noc-performance-parameters.md` and `docs/nsu-spec.md` match `constants.yaml`: `DAT_NUM_VC=2` and `NSU_META_BUFFER_MAX_UNIQUE_IDS=8`. List every Markdown reference to either definition and identify historical plans as superseded rather than rewriting their historical results.

- [x] **Step 2: Gate conditional NI sizing on measured HWM**

  Treat `ROB_R_DEPTH` as beat slots: a 4KB Read reserves 64 slots when fallback allocation is used. Test depth 64 or 256 only when `ar_fallback_alloc_count > 0` and the 128-slot baseline is relevant. Test `MAX_TXNS_PER_ID` or NSU pool depths 16 and 8 only when their direction-specific HWM supports a cost reduction. Add NSU occupancy HWM first if it is not observable.

- [x] **Step 3: Run VC/router configurations**

  Run all six Outstanding points for the five approved stress rows at each fixed-depth and equal-capacity configuration. Rebuild once per hardware configuration. Restore `DAT_NUM_VC=2` and Router depth 8 after each batch and at campaign end.

- [x] **Step 4: Select measured-set Pareto candidates**

  Compare the same workload rows and direction weights. A selected configuration must not have both lower combined performance and larger storage proxy than another measured configuration. Report Read and Write raw results; do not hide a direction regression inside one opaque score.

- [x] **Step 5: Run final gates and close the auxiliary task**

  ```text
  python -m pytest sim/tools/test_outstanding_injection_mode.py sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py -q
  python specgen/tools/codegen.py --check
  git diff --check
  ```

  Run clean 2x2 and 4x4 co-sim smoke tests at restored defaults. Record exact commands and results in `docs/backlog.md`; mark this auxiliary task complete only after all generated rows and the final report pass.

---

## Planned Commits

1. `test(sim): define outstanding read performance contract`
2. `feat(sim): support outstanding driven read traffic`
3. `perf(sim): collect read and write outstanding sweeps`
4. `docs(perf): report outstanding based AI traffic results`

Each commit must compile and pass its focused tests. Stage exact paths only because the worktree contains unrelated changes.
