# Injection mode and injection-rate sweep — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make injection mode, injection rate and injection count parameters of `make sim`, plumb the NSU's `max_unique_ids` and `max_outstanding` through as plusargs, and produce a latency-throughput curve per VC count.

**Architecture:** `INJECTION_MODE=1` selects the interleaved, rate-paced send loop that already exists in `user_node_endpoint.sv`; mode 0 keeps today's two-phase run. Mode 1 disarms the `axi_scoreboard`, whose write-before-read precondition interleaving destroys. Each run parses its own `run.log` into a one-row `result.csv` carrying the six parameters that bound the number. A sweep target loops `make sim` and plots.

**Tech Stack:** GNU make, SystemVerilog (Verilator 5.048), C++17 header-only c_model, Python 3.

Spec: `docs/superpowers/specs/2026-07-09-injection-mode-and-rate-sweep-design.md`

## Global Constraints

- **Every build, test and simulation runs inside WSL.** The agent's shell is Git Bash on Windows and cannot build this project. Wrap each command:

  ```bash
  wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test > /tmp/out.log 2>&1; echo "rc=$?"; tail -20 /tmp/out.log'
  ```

  Redirect to a file and `tail` it. Piping `make` output straight through `wsl -e` swallows it. Edit files with the Read/Edit/Write tools using Windows paths (`E:\05_NoC\noc_project\...`).
- **`clang-format` (v20.1.8) exists on win32 only.** Run it from the normal Bash tool, never inside WSL.
- **ctest baseline: `397 tests passed, 0 failed`.** This plan adds no tests. Every task must end at exactly 397.
- Co-sim builds are slow (Verilator). Allow up to 600000 ms per command. A long-running command has not hung.
- If a build dies on `libgtest.a: archive has no index`, the build tree is corrupt, not the code. Recover with `rm -rf ~/noc_build/cmodel/_deps/googletest-build/googletest/CMakeFiles/gtest.dir ~/noc_build/cmodel/lib/libgtest.a && cmake -S src/c_model -B ~/noc_build/cmodel`, then rebuild. Never edit source to work around it.
- If the Verilator build fails on a stale `.d` dependency naming a renamed header, run `make -C sim/verilator clean` first.
- Naming: `snake_case` for variables/methods/fields, `PascalCase` for types, full words, no abbreviations, no camelCase.
- Commit message format `type(scope): description`, English. Never `--no-verify`. Do not push.
- **No new unit tests.** The Verilator co-sim is the gate. `sim/tools/gen_test_patterns.py` is not touched by this plan.

---

## Branch

This work depends on `max_unique_ids` and `max_outstanding` existing in the c_model, which landed on `feat/nsu-meta-buffer-remap`. Branch from there:

```bash
git checkout feat/nsu-meta-buffer-remap && git checkout -b feat/injection-mode-sweep
```

## File Structure

| file | responsibility after this change |
|---|---|
| `sim/dv/floonoc-test/axi_bw_monitor.sv` | prints the latency sample count so the aggregate can be weighted |
| `src/dpi/cmodel_dpi.{h,cpp}` | `cmodel_nsu_create` carries `max_unique_ids` and `max_outstanding` |
| `src/c_model/include/wrap/nsu_wrap.hpp` | `init` takes both, no longer hardcodes them |
| `sim/tools/gen_tb_top.py` | reads and echoes both plusargs, emits the widened DPI call |
| `sim/tb/user_node_endpoint.sv` | `+injection_mode` selects the send loop and arms or disarms the scoreboard |
| `sim/verilator/Makefile` | one `run-directed` recipe, gate branches on mode; emits `result.csv` |
| `Makefile` | parameter pass-through; `sim-injection-sweep` |
| `sim/tools/emit_result_csv.py` | new: parses one `run.log` into one `result.csv` |
| `sim/tools/plot_injection_sweep.py` | new: globs every `result.csv`, merges, prints table, renders PNG |

Deleted: `sim/tools/collect_saturation.py`, `sim/tools/plot_saturation.py`.

---

## Task 1: Print the latency sample count

The aggregate `mean_latency` must be weighted by each monitor's transaction count. The count exists inside the monitor but is not printed.

**Files:**
- Modify: `sim/dv/floonoc-test/axi_bw_monitor.sv:146-149`

**Interfaces:**
- Consumes: nothing.
- Produces: monitor lines of the form `[Monitor <name>][Read] Latency: <mean> +- <sd>, N: <count>, BW: <bw> Bits/cycle, Util: <util>%`.

- [ ] **Step 1: Add the count to both `$display` calls**

Replace lines 146-149 of `sim/dv/floonoc-test/axi_bw_monitor.sv`:

```systemverilog
    $display("[Monitor %s][Read] Latency: %0.2f +- %0.2f, N: %0d, BW: %0.2f Bits/cycle, Util: %0.2f%%",
             Name, read_latency_mean, read_latency_stddev, read_latency.size(), read_bw, read_util);
    $display("[Monitor %s][Write] Latency: %0.2f +- %0.2f, N: %0d, BW: %0.2f Bits/cycle, Util: %0.2f%%",
             Name, write_latency_mean, write_latency_stddev, write_latency.size(), write_bw, write_util);
```

- [ ] **Step 2: Record the local modification**

`axi_bw_monitor.sv` is vendored from FlooNoC under `sim/dv/floonoc-test/LICENSE`. Add a line at the top of the file, immediately after the existing header comment:

```systemverilog
// Locally modified: the two $display calls print the latency sample count (N),
// which the aggregate weighting in sim/tools/emit_result_csv.py requires.
```

- [ ] **Step 3: Run one co-sim and confirm the field appears**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor SEED=12345 > /tmp/t1.log 2>&1; echo "rc=$?"; grep -m2 "\[Monitor" /tmp/t1.log'
```

Expected: `DIRECTED PASS` and two monitor lines containing `N: 4` (four transactions per node in the current directed default).

If `N:` is absent, the build did not pick up the SV change. Run `make -C sim/verilator clean` and retry.

- [ ] **Step 4: Commit**

```bash
git add sim/dv/floonoc-test/axi_bw_monitor.sv
git commit -m "feat(sim): print latency sample count in axi_bw_monitor

The aggregate mean latency across 32 node-direction monitors must be weighted
by each monitor's transaction count. The count existed but was not printed.
Vendored file, marked locally modified per its licence."
```

---

## Task 2: Widen `cmodel_nsu_create` on the C++ side

Two new parameters reach `NsuConfig`. Nothing calls them with non-default values yet; Task 3 wires the generator.

**Files:**
- Modify: `src/dpi/cmodel_dpi.h:130`, `src/dpi/cmodel_dpi.cpp:509-526`
- Modify: `src/c_model/include/wrap/nsu_wrap.hpp:52,69-70`
- Modify: `src/c_model/tests/wrap/test_cmodel_dpi.cpp:65`

**Interfaces:**
- Consumes: `ni::cmodel::wrap::kMetaBufferMaxUniqueIds` (1), `kMetaBufferMaxOutstanding` (32) from `wrap/wrap_defaults.hpp`.
- Produces:
  - `unsigned long long cmodel_nsu_create(const char* name, int src_id, int num_vc, int max_unique_ids, int max_outstanding)`
  - `NsuWrap::init(uint8_t src_id, uint8_t num_vc, std::size_t queue_depth, std::size_t max_unique_ids, std::size_t max_outstanding)`

- [ ] **Step 1: Widen `NsuWrap::init`**

In `src/c_model/include/wrap/nsu_wrap.hpp`, change the signature at line 52:

```cpp
    void init(uint8_t src_id = 0, uint8_t num_vc = 1, std::size_t queue_depth = kAxiQueueDepth,
              std::size_t max_unique_ids = kMetaBufferMaxUniqueIds,
              std::size_t max_outstanding = kMetaBufferMaxOutstanding) {
```

and replace the two hardcoded assignments at lines 69-70:

```cpp
        cfg.port_params.meta_buffer_max_outstanding = max_outstanding;
        cfg.port_params.meta_buffer_max_unique_ids = max_unique_ids;
```

- [ ] **Step 2: Widen the DPI declaration**

In `src/dpi/cmodel_dpi.h`, replace line 130:

```c
// max_unique_ids: 1 collapses every manager onto the all-ones downstream AXI id
// (FlooNoC's ChimneyDefaultCfg); 256 passes the manager's id through. No other
// value is legal, and the Depacketize constructor asserts it.
// max_outstanding: shared MetaBuffer pool size per direction (FlooNoC MaxTxns).
unsigned long long cmodel_nsu_create(const char* name, int src_id, int num_vc, int max_unique_ids,
                                     int max_outstanding);
```

- [ ] **Step 3: Widen the DPI definition**

In `src/dpi/cmodel_dpi.cpp`, replace the signature at line 509 and the `init` call at line 515:

```cpp
extern "C" unsigned long long cmodel_nsu_create(const char* name, int src_id, int num_vc,
                                                int max_unique_ids, int max_outstanding) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nsu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_nsu_create, 0ull) {
        auto adapter = std::make_unique<NsuWrap>();
        adapter->init(static_cast<uint8_t>(src_id), static_cast<uint8_t>(num_vc), kAxiQueueDepth,
                      static_cast<std::size_t>(max_unique_ids),
                      static_cast<std::size_t>(max_outstanding));
```

Leave the rest of the function body untouched.

- [ ] **Step 4: Fix the one hand-written caller**

In `src/c_model/tests/wrap/test_cmodel_dpi.cpp`, line 65:

```cpp
    unsigned long long nsu_handle = cmodel_nsu_create("nsu_test", 0, /*num_vc=*/1,
                                                      /*max_unique_ids=*/1,
                                                      /*max_outstanding=*/32);
```

- [ ] **Step 5: Confirm no other caller exists**

Run: `grep -rn "cmodel_nsu_create" src/ sim/ | grep -v "tb_top_"`

Expected: exactly the declaration, the definition, the one test call, and the two `gen_tb_top.py` emitter lines. The `sim/tb/tb_top_*.sv` files still carry the old three-argument import. They are generator output and Task 3 regenerates them; the co-sim will not build until then. That is expected.

- [ ] **Step 6: Build and test**

```bash
clang-format -i src/dpi/cmodel_dpi.cpp src/dpi/cmodel_dpi.h src/c_model/include/wrap/nsu_wrap.hpp src/c_model/tests/wrap/test_cmodel_dpi.cpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test > /tmp/t2.log 2>&1; echo "rc=$?"; grep -E "tests passed|tests failed" /tmp/t2.log'
```

Expected: `397 tests passed, 0 tests failed`.

- [ ] **Step 7: Commit**

```bash
git add src/dpi/ src/c_model/include/wrap/nsu_wrap.hpp src/c_model/tests/wrap/test_cmodel_dpi.cpp
git commit -m "feat(dpi): cmodel_nsu_create carries max_unique_ids and max_outstanding

Widen the existing symbol rather than add cmodel_nsu_create_ex. The only
hand-written caller is one ctest line; the eleven tb_top files are generator
output. Defaults still come from wrap_defaults.hpp when unset."
```

---

## Task 3: Read the plusargs, emit the widened call, regenerate the testbenches

**Files:**
- Modify: `sim/tools/gen_tb_top.py:533-534` (import), `:545-551` (plusarg block), `:562` (call)
- Regenerate: `sim/tb/tb_top_*.sv` (11 files)

**Interfaces:**
- Consumes: `cmodel_nsu_create(name, src_id, num_vc, max_unique_ids, max_outstanding)` (Task 2).
- Produces: `+max_unique_ids=<n>` and `+max_outstanding=<n>` plusargs, both echoed into `run.log`.

- [ ] **Step 1: Emit the widened import**

In `sim/tools/gen_tb_top.py`, replace lines 533-534:

```python
    w('    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name,')
    w('                                                              input int src_id, input int num_vc,')
    w('                                                              input int max_unique_ids,')
    w('                                                              input int max_outstanding);')
```

- [ ] **Step 2: Declare, read and echo the two plusargs**

In the same file, immediately after the `string sam_config_path = "";` line, add:

```python
    w("")
    w("    // NSU knobs. max_unique_ids=1 collapses every manager onto one downstream")
    w("    // AXI id (FlooNoC default); 256 passes the manager's id through.")
    w("    // max_outstanding is the shared MetaBuffer pool per direction.")
    w("    int unsigned max_unique_ids  = 1;")
    w("    int unsigned max_outstanding = 32;")
```

and inside the `initial begin` block, immediately after the existing `sam_config` read:

```python
    w('        void\'($value$plusargs("max_unique_ids=%d", max_unique_ids));')
    w('        void\'($value$plusargs("max_outstanding=%d", max_outstanding));')
    w('        $display("[Config] max_unique_ids=%0d max_outstanding=%0d", max_unique_ids, max_outstanding);')
```

The `$display` is not decoration. It is the only record of which configuration produced a number, and its absence is why the previous saturation series was thrown away.

- [ ] **Step 3: Emit the widened call**

Replace line 562:

```python
        w(f'        nsu_ctx[{i}] = cmodel_nsu_create("nsu_{i}", {c}, NUM_VC, max_unique_ids, max_outstanding);')
```

- [ ] **Step 4: Regenerate every testbench**

The build regenerates `sim/tb/tb_top_<topo>.sv` from the topology YAML. Force all eleven:

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && for t in mesh_1x1_vc1 mesh_2x2_nonuniform_vc1 mesh_2x4_vc1 mesh_4x4_vc1 mesh_4x4_vc2 mesh_4x4_vc4 mesh_4x4_vc8; do for suffix in "" "_rob"; do python3 sim/tools/gen_tb_top.py --topology $t$suffix --out sim/tb/tb_top_$t$suffix.sv; done; done 2>&1 | tail -3; git status --short sim/tb/'
```

Expected: the `git status` lists the regenerated `tb_top_*.sv` files as modified. If a topology has no `_rob` variant checked in, do not create one — check `git status` against `ls sim/tb/tb_top_*.sv` and regenerate only files that already exist.

- [ ] **Step 5: Confirm the `[Config]` line appears**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make -C sim/verilator clean > /dev/null 2>&1; make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor SEED=12345 > /tmp/t3.log 2>&1; echo "rc=$?"; grep -m1 "\[Config\]" sim/verilator/output/directed_mesh_4x4_vc1_neighbor_s12345/run.log; tail -1 /tmp/t3.log'
```

Expected: `[Config] max_unique_ids=1 max_outstanding=32` and `DIRECTED PASS`.

The **defaults** are correct here. `MAX_UNIQUE_IDS` and `MAX_OUTSTANDING` are not yet Makefile variables, so nothing overrides the plusarg defaults declared in Step 2. Task 5 adds the Makefile pass-through and Task 5 Step 5 checks the override.

The absence of `[Config]` means the testbench was not regenerated. Re-run Step 4.

- [ ] **Step 6: Full ctest**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test > /tmp/t3b.log 2>&1; echo "rc=$?"; grep -E "tests passed|tests failed" /tmp/t3b.log'
```

Expected: `397 tests passed`.

- [ ] **Step 7: Commit**

```bash
git add sim/tools/gen_tb_top.py sim/tb/
git commit -m "feat(sim): read and echo the NSU knobs as plusargs

+max_unique_ids and +max_outstanding reach every NSU, and a [Config] line
records them in run.log. A number whose configuration is not recorded is a
number that will be thrown away."
```

---

## Task 4: `+injection_mode` selects the send loop and arms the scoreboard

**Files:**
- Modify: `sim/tb/user_node_endpoint.sv:251-256` (scoreboard), `:264-265` (declarations), `:294-306` (send loop)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `+injection_mode=0|1`, `+injection_rate=<real>`. Mode 1 leaves the `axi_scoreboard` disarmed.

- [ ] **Step 1: Add a plusarg reader function**

Two `initial` blocks need the mode. Reading a shared module-scope variable from both is a race: nothing orders the blocks. Read it independently in each, through a pure function.

In `sim/tb/user_node_endpoint.sv`, immediately above the scoreboard `initial` block (line 251):

```systemverilog
    // Read once per caller, into a local. A shared module-scope variable written
    // by one initial block and read by another has no defined ordering.
    function automatic int unsigned get_injection_mode();
        int unsigned m = 0;
        void'($value$plusargs("injection_mode=%d", m));
        return m;
    endfunction
```

- [ ] **Step 2: Arm the scoreboard only in mode 0**

Replace the scoreboard `initial` block (lines 251-256):

```systemverilog
    initial begin
        scoreboard = new(master_dv);
        scoreboard.reset();
        @(posedge rst_ni);
        // Mode 1 interleaves reads and writes, so a read may precede the write to
        // its address and the scoreboard's write-before-read precondition fails.
        // Skip BOTH enable_all_checks() and monitor(): construction hooks nothing,
        // but monitor() forks the sampling tasks and mutates the memory model even
        // with checks off.
        if (get_injection_mode() == 0) begin
            scoreboard.enable_all_checks();
            scoreboard.monitor();
        end
    end
```

- [ ] **Step 3: Select the send loop on the mode, not on the rate's presence**

Replace the declarations at lines 264-265:

```systemverilog
    real injection_rate;
```

and replace the `if ($value$plusargs("traffic_inj_ratio=%f", traffic_inj_ratio))` branch (lines 294-306) with:

```systemverilog
        if (get_injection_mode() == 1) begin
            injection_rate = 1.0;
            void'($value$plusargs("injection_rate=%f", injection_rate));
            inj_gate_pct = int'(injection_rate * 100.0);
            // Continuous injection: one phase, reads and writes interleaved, each
            // send gated per cycle on injection_rate. join (not join_none) so B/R
            // are consumed and the pass terminates cleanly.
            fork
                gated_run_aw();
                file_master.run_w();
                gated_run_ar();
                file_master.wait_b();
                file_master.wait_r();
            join
            run_done = 1'b1;
        end else begin
```

Leave the `else` body (the two-phase forks) exactly as it is.

Update the comment block above `gated_run_aw` (line 258 onward) to say `+injection_mode` and `+injection_rate` instead of `+traffic_inj_ratio`.

- [ ] **Step 4: Confirm mode 0 is unchanged**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor SEED=12345 > /tmp/t4a.log 2>&1; echo "rc=$?"; tail -2 /tmp/t4a.log'
```

Expected: `DIRECTED PASS`. Mode 0 is the default and nothing passes `+injection_mode`, so `get_injection_mode()` returns 0 and the scoreboard arms.

- [ ] **Step 5: Confirm mode 1 disarms the scoreboard**

The Makefile does not yet pass the plusargs. Invoke the binary directly:

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project/sim/verilator && mkdir -p output/t4 && $HOME/noc_build/verilator/obj_dir_mesh_4x4_vc1_directed/Vtb_top "+stim_dir=$PWD/../test_patterns/directed_mesh_4x4_vc1_neighbor" "+num_reads=4" "+num_writes=4" "+injection_mode=1" "+injection_rate=0.4" "+verilator+seed+12345" "+sam_config=$PWD/../topologies/mesh_4x4_vc1.yaml" > output/t4/run.log 2>&1; echo "rc=$?"; grep -c "Unexpected RData" output/t4/run.log; grep -c "\[Monitor" output/t4/run.log'
```

Expected: `rc=0`, **zero** `Unexpected RData` (the scoreboard is disarmed), and 32 monitor lines.

If `Unexpected RData` appears, the scoreboard was armed. That is the whole point of this task. Do not proceed.

- [ ] **Step 6: Commit**

```bash
git add sim/tb/user_node_endpoint.sv
git commit -m "feat(sim): +injection_mode selects the send loop and arms the scoreboard

Mode 1 interleaves reads and writes, which breaks the scoreboard's
write-before-read precondition. Disarm it explicitly instead of ignoring its
warnings: a live checker whose output is ignored trains the reader to ignore it.
The mode is read through a pure function, because two initial blocks need it and
nothing orders them."
```

---

## Task 5: One run recipe, mode-dependent gate, parameter pass-through

**Files:**
- Modify: `sim/verilator/Makefile:194-234` (run-directed), delete `:236-292` (run-traffic and sim-saturation)
- Modify: `Makefile:171-192`
- Delete: `sim/tools/collect_saturation.py`, `sim/tools/plot_saturation.py`

**Interfaces:**
- Consumes: `+injection_mode`, `+injection_rate` (Task 4); `+max_unique_ids`, `+max_outstanding` (Task 3).
- Produces: `make sim TB=<tb> PATTERN=<p> [INJECTION_MODE=1] [INJECTION_RATE=<r>] [INJECTION_COUNT=<n>] [MAX_UNIQUE_IDS=<n>] [MAX_OUTSTANDING=<n>] [SEED=<n>]`.

- [ ] **Step 1: Replace the parameter block in `sim/verilator/Makefile`**

Replace lines 194-202 (`PATTERN` through `_HOTSPOT_ARGS`) with:

```makefile
# One directed run recipe, two injection modes.
#   INJECTION_MODE=0 (default): two phases, writes drain before reads, scoreboard gates.
#   INJECTION_MODE=1: one phase, reads and writes interleave, paced per cycle by
#     INJECTION_RATE. The scoreboard's write-before-read precondition fails, so the
#     testbench disarms it and the axi_bw_monitor gates instead.
# INJECTION_COUNT bounds the run. Past saturation the queues never empty, and
# wait_b/wait_r would not terminate without it (booksim solves the same problem
# with sim_type=throughput, which skips its drain).
PATTERN         ?= neighbor
INJECTION_MODE  ?= 0
INJECTION_RATE  ?= 1.0
INJECTION_COUNT ?= $(if $(filter 1,$(INJECTION_MODE)),200,4)
MAX_UNIQUE_IDS  ?= 1
MAX_OUTSTANDING ?= 32
HOTSPOT         ?= 5

# _CONTINUOUS is non-empty exactly when INJECTION_MODE is 1. Kept on one line:
# a backslash continuation inside a `?=` value folds the leading whitespace of
# the next line into the value.
_CONTINUOUS    := $(filter 1,$(INJECTION_MODE))
STIM_ROOT      ?= $(COSIM_ROOT)/test_patterns/stim_$(TOPOLOGY)_$(PATTERN)_n$(INJECTION_COUNT)
SIM_TAG        ?= $(if $(_CONTINUOUS),continuous_$(TOPOLOGY)_$(PATTERN)_r$(INJECTION_RATE)_s$(SEED),directed_$(TOPOLOGY)_$(PATTERN)_s$(SEED))
GEN_PATTERNS   := $(COSIM_ROOT)/tools/gen_test_patterns.py
EMIT_CSV       := $(COSIM_ROOT)/tools/emit_result_csv.py
_HOTSPOT_ARGS  := $(if $(filter hotspot,$(PATTERN)),--hotspot $(HOTSPOT),)
```

`DIRECTED_TAG` is renamed to `SIM_TAG`: it names both modes now. `RUN_TAG` is taken by `run-tb-top`.

- [ ] **Step 2: Replace the `run-directed` recipe body**

Replace lines 205-234 with:

```makefile
.PHONY: run-directed
run-directed: $(TBTOP_EXE)
	# Guard runs first in the recipe (before emit/run), so a wrong RUN_CLASS exits
	# before a vacuous no-scoreboard run. Deliberately a recipe check, not a
	# parse-time $(error): OBJ_DIR/TBTOP_EXE are := fixed at parse, so a target-
	# specific override cannot re-key them; the cost is a wasted wrong-flavor build.
	@[ "$(RUN_CLASS)" = "directed" ] || { echo "ERROR: run-directed requires RUN_CLASS=directed (got '$(RUN_CLASS)')"; exit 1; }
	@mkdir -p output/$(SIM_TAG)
	$(PYTHON3) $(GEN_PATTERNS) --topology $(TOPOLOGY) \
	    --out $(STIM_ROOT) --pattern $(PATTERN) \
	    --transactions-per-node $(INJECTION_COUNT) --size 5 --len 0 \
	    --memory-size 0x40000 --seed $(SEED) $(_HOTSPOT_ARGS)
	@echo "running $(SIM_TAG) (mode=$(INJECTION_MODE) rate=$(INJECTION_RATE) count=$(INJECTION_COUNT) pattern=$(PATTERN))"
	$(TBTOP_EXE) \
	    "+stim_dir=$(STIM_ROOT)" \
	    "+num_reads=$(INJECTION_COUNT)" "+num_writes=$(INJECTION_COUNT)" \
	    "+injection_mode=$(INJECTION_MODE)" "+injection_rate=$(INJECTION_RATE)" \
	    "+max_unique_ids=$(MAX_UNIQUE_IDS)" "+max_outstanding=$(MAX_OUTSTANDING)" \
	    "+verilator+seed+$(SEED)" \
	    "+sam_config=$(TB_TOP_TOPO)" \
	    "+perf_out=output/$(SIM_TAG)/perf.json" \
	    "+perf_scenario=$(SIM_TAG)" \
	    > output/$(SIM_TAG)/run.log 2>&1; \
	rc=$$?; \
	echo "--- run.log: per-node monitors + tail ---"; grep -F '[Monitor' output/$(SIM_TAG)/run.log; tail -4 output/$(SIM_TAG)/run.log; \
	if grep -qE 'Unexpected RData|Unexpected W last|Not supported (AW|AR) burst|Atomic transfers not supported|%Error' \
	        output/$(SIM_TAG)/run.log; then \
	    echo "RUN FAIL: scoreboard mismatch/assertion/malformed stimulus"; exit 1; \
	fi; \
	if [ "$(INJECTION_MODE)" = "1" ]; then \
	    if [ $$rc -ne 0 ]; then echo "CONTINUOUS FAIL (rc=$$rc)"; exit 1; fi; \
	    if ! grep -qiE '\[Monitor .*\]\[(Read|Write)\].*BW:\s*[1-9]' output/$(SIM_TAG)/run.log; then \
	        echo "CONTINUOUS FAIL: no non-zero bandwidth reported"; exit 1; \
	    fi; \
	    $(PYTHON3) $(EMIT_CSV) --log output/$(SIM_TAG)/run.log \
	        --out output/$(SIM_TAG)/result.csv \
	        --topology $(TOPOLOGY) --pattern $(PATTERN) \
	        --injection-mode $(INJECTION_MODE) --injection-rate $(INJECTION_RATE) \
	        --injection-count $(INJECTION_COUNT) --seed $(SEED) \
	        --max-unique-ids $(MAX_UNIQUE_IDS) --max-outstanding $(MAX_OUTSTANDING); \
	    echo "CONTINUOUS PASS: $(SIM_TAG)"; \
	elif [ $$rc -ne 0 ] || ! grep -q 'PASS: all' output/$(SIM_TAG)/run.log; then \
	    echo "DIRECTED FAIL: run did not reach non-vacuous PASS (rc=$$rc)"; exit 1; \
	else \
	    echo "DIRECTED PASS: $(SIM_TAG) scoreboard clean, non-vacuous"; \
	fi
```

Note the `Unexpected RData` grep now runs in **both** modes. In mode 1 the scoreboard is disarmed, so a hit means Task 4 regressed.

- [ ] **Step 3: Delete `run-traffic`, `sim-saturation` and their scripts**

Delete lines 236-292 of `sim/verilator/Makefile` in their entirety: the `INJ_RATIO` / `TRAFFIC_TXNS` / `IDS_PER_TILE` / `TRAFFIC_TAG` / `TRAFFIC_STIM` block, the `run-traffic` recipe, and the `sim-saturation` target.

```bash
git rm sim/tools/collect_saturation.py sim/tools/plot_saturation.py
grep -rn "run-traffic\|sim-saturation\|collect_saturation\|plot_saturation\|TXNS_PER_NODE\|INJ_RATIO\|TRAFFIC_TXNS\|IDS_PER_TILE" --include=Makefile --include=*.mk --include=*.py sim/ Makefile
```

The `grep` must return nothing outside `docs/`. Anything it finds is a caller the deletion just broke. `docs/` is reconciled in Task 8.

- [ ] **Step 4: Pass the parameters through the root `Makefile`**

In `Makefile`, replace lines 177-192:

```makefile
TB      ?= mesh_4x4_vc1
PATTERN ?= neighbor
_TOPO   := $(TB:tb_%=%)
_CLASS  := $(if $(filter constrained_random,$(PATTERN)),constrained_random,directed)
_SEED   := $(if $(SEED),$(SEED),$(shell bash -c 'echo $$RANDOM$$RANDOM'))

# Forwarded only when set, so sim/verilator/Makefile's own defaults apply otherwise.
# INJECTION_COUNT's default depends on INJECTION_MODE and is computed there.
_INJ_ARGS := \
    $(if $(INJECTION_MODE),INJECTION_MODE=$(INJECTION_MODE)) \
    $(if $(INJECTION_RATE),INJECTION_RATE=$(INJECTION_RATE)) \
    $(if $(INJECTION_COUNT),INJECTION_COUNT=$(INJECTION_COUNT)) \
    $(if $(MAX_UNIQUE_IDS),MAX_UNIQUE_IDS=$(MAX_UNIQUE_IDS)) \
    $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING))

.PHONY: sim
sim:
	@echo ">>> sim TB=$(_TOPO) PATTERN=$(PATTERN) class=$(_CLASS) SEED=$(_SEED)"
ifeq ($(_CLASS),constrained_random)
	$(MAKE) -C sim/verilator run-constrained-random TOPOLOGY=$(_TOPO) RUN_CLASS=constrained_random \
	    SEED=$(_SEED) $(if $(NUM_READS),NUM_READS=$(NUM_READS)) $(if $(NUM_WRITES),NUM_WRITES=$(NUM_WRITES))
else
	$(MAKE) -C sim/verilator run-directed TOPOLOGY=$(_TOPO) RUN_CLASS=directed \
	    PATTERN=$(PATTERN) SEED=$(_SEED) $(_INJ_ARGS) $(if $(HOTSPOT),HOTSPOT=$(HOTSPOT))
endif
```

The `TXN` variable is retired. `INJECTION_COUNT` replaces it.

- [ ] **Step 5: Verify mode 0 on all four patterns, unchanged**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && for p in neighbor transpose uniform_random hotspot; do make sim TB=tb_mesh_4x4_vc1 PATTERN=$p SEED=12345 > /tmp/m0_$p.log 2>&1 || { echo "FAIL $p"; break; }; tail -1 /tmp/m0_$p.log; done'
```

Expected: four `DIRECTED PASS` lines.

Then confirm the knobs actually reach the c_model:

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor SEED=12345 MAX_UNIQUE_IDS=256 MAX_OUTSTANDING=64 > /tmp/knob.log 2>&1; grep -m1 "\[Config\]" sim/verilator/output/directed_mesh_4x4_vc1_neighbor_s12345/run.log'
```

Expected: `[Config] max_unique_ids=256 max_outstanding=64`.

- [ ] **Step 6: Commit**

Task 6 writes `emit_result_csv.py`, which this recipe calls. Commit them together at the end of Task 6 rather than leaving a broken mode-1 path. Stage the work now:

```bash
git add sim/verilator/Makefile Makefile
```

Do **not** commit yet. Proceed to Task 6.

---

## Task 6: `emit_result_csv.py`

One run, one row. The recipe knows its own tag, so nothing guesses a path.

**Files:**
- Create: `sim/tools/emit_result_csv.py`

**Interfaces:**
- Consumes: the monitor lines from Task 1, the recipe invocation from Task 5.
- Produces: `output/<tag>/result.csv`, one header line and one row, columns:
  `topology,vc,pattern,injection_mode,injection_rate,injection_count,seed,max_unique_ids,max_outstanding,accepted_bits_per_cycle,mean_latency`

- [ ] **Step 1: Write the script**

Create `sim/tools/emit_result_csv.py`:

```python
"""Emit one CSV row for a completed continuous-injection run.

Parses the axi_bw_monitor lines out of one run.log and writes result.csv beside
it. The row carries every parameter that bounds the measurement, so a number is
never separated from the configuration that produced it.

accepted_bits_per_cycle sums BW across all monitors, which is correct because
every monitor shares one cycle_cnt window.

mean_latency is weighted by each monitor's sample count. A plain average of the
printed means is wrong: each is already a mean over that monitor's own
transaction count, and those counts differ.
"""
import argparse
import csv
import pathlib
import re
import sys

# [Monitor node0.manager][Read] Latency: 98.30 +- 4.10, N: 200, BW: 107.02 Bits/cycle, Util: 41.80%
_MON = re.compile(
    r"\[Monitor[^\]]*\]\[(?:Read|Write)\]\s+Latency:\s*([\d.]+)\s*\+-\s*[\d.]+,\s*"
    r"N:\s*(\d+),\s*BW:\s*([\d.]+)\s*Bits/cycle",
    re.I,
)
_VC = re.compile(r"_vc(\d+)")


def parse_monitors(log_text):
    """Return (summed BW, sample-weighted mean latency)."""
    total_bw = 0.0
    weighted_latency = 0.0
    total_samples = 0
    for mean, count, bw in _MON.findall(log_text):
        count = int(count)
        total_bw += float(bw)
        weighted_latency += float(mean) * count
        total_samples += count
    if total_samples == 0:
        sys.exit("emit_result_csv: no monitor line reported a sample; the run injected nothing")
    return total_bw, weighted_latency / total_samples


def vc_count(topology):
    m = _VC.search(topology)
    if not m:
        sys.exit(f"emit_result_csv: no _vc<N> in topology name {topology!r}")
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--topology", required=True)
    ap.add_argument("--pattern", required=True)
    ap.add_argument("--injection-mode", required=True)
    ap.add_argument("--injection-rate", required=True)
    ap.add_argument("--injection-count", required=True)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--max-unique-ids", required=True)
    ap.add_argument("--max-outstanding", required=True)
    a = ap.parse_args()

    bw, latency = parse_monitors(pathlib.Path(a.log).read_text())
    row = {
        "topology": a.topology,
        "vc": vc_count(a.topology),
        "pattern": a.pattern,
        "injection_mode": a.injection_mode,
        "injection_rate": a.injection_rate,
        "injection_count": a.injection_count,
        "seed": a.seed,
        "max_unique_ids": a.max_unique_ids,
        "max_outstanding": a.max_outstanding,
        "accepted_bits_per_cycle": f"{bw:.1f}",
        "mean_latency": f"{latency:.1f}",
    }
    with open(a.out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
    print(f"wrote {a.out}: {bw:.1f} bits/cyc, latency {latency:.1f}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify the weighting on a hand-built log**

The weighting is the one piece of logic here that can be silently wrong. Check it before trusting a real run.

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && printf "%s\n" \
  "[Monitor n0][Read] Latency: 100.00 +- 1.00, N: 1, BW: 10.00 Bits/cycle, Util: 1.00%%" \
  "[Monitor n1][Read] Latency: 200.00 +- 1.00, N: 3, BW: 20.00 Bits/cycle, Util: 1.00%%" \
  > /tmp/fake.log && python3 sim/tools/emit_result_csv.py --log /tmp/fake.log --out /tmp/fake.csv \
    --topology mesh_4x4_vc4_rob --pattern uniform_random --injection-mode 1 --injection-rate 0.4 \
    --injection-count 200 --seed 1 --max-unique-ids 256 --max-outstanding 32 && cat /tmp/fake.csv'
```

Expected: `30.0 bits/cyc, latency 175.0`. The weighted mean is `(100*1 + 200*3) / 4 = 175`. An unweighted mean would print `150.0`, and the column `vc` must read `4`.

- [ ] **Step 3: Run one real continuous-injection point**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1 PATTERN=uniform_random SEED=12345 INJECTION_MODE=1 INJECTION_RATE=0.4 MAX_UNIQUE_IDS=256 > /tmp/t6.log 2>&1; echo "rc=$?"; tail -2 /tmp/t6.log; cat sim/verilator/output/continuous_mesh_4x4_vc1_uniform_random_r0.4_s12345/result.csv'
```

Expected: `CONTINUOUS PASS`, and a two-line CSV whose row shows `injection_mode=1`, `injection_rate=0.4`, `max_unique_ids=256`.

- [ ] **Step 4: Confirm mode 0 emits no CSV**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && ls sim/verilator/output/directed_mesh_4x4_vc1_neighbor_s12345/'
```

Expected: `run.log` and `perf.json`, no `result.csv`. Mode 0 measures nothing.

- [ ] **Step 5: Full ctest**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test > /tmp/t6b.log 2>&1; grep -E "tests passed|tests failed" /tmp/t6b.log'
```

Expected: `397 tests passed`.

- [ ] **Step 6: Commit Tasks 5 and 6 together**

The `git rm` from Task 5 Step 3 is already staged.

```bash
git add sim/tools/emit_result_csv.py sim/verilator/Makefile Makefile
git status --short
git commit -m "feat(sim): one run recipe, two injection modes, per-run result.csv

run-traffic folds into run-directed; the gate branches on INJECTION_MODE. Each
continuous run parses its own run.log into a one-row result.csv carrying the six
parameters that bound the number.

collect_saturation.py deleted: it guessed the log path from a tag it did not
build, which is why adding a second pattern would have broken it silently.
mean_latency is sample-weighted; the unweighted average it used was wrong."
```

---

## Task 7: The sweep and the plot

**Files:**
- Create: `sim/tools/plot_injection_sweep.py`
- Modify: `Makefile` (add `sim-injection-sweep`)

**Interfaces:**
- Consumes: `output/*/result.csv` (Task 6).
- Produces: `sim/tools/injection_sweep.csv`, a printed table, and `injection_sweep.png` when matplotlib is present.

- [ ] **Step 1: Write the plot script**

Create `sim/tools/plot_injection_sweep.py`:

```python
"""Merge every continuous-injection result.csv into one table and plot it.

Globs rather than concatenating: `cat output/*/result.csv` would repeat the
header on every file.

Two curves per VC count, sharing the offered injection rate on x: accepted
throughput, whose knee is saturation, and mean latency, whose divergence is how
the literature defines the same point.
"""
import csv
import pathlib
import sys
from collections import defaultdict

_OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "verilator" / "output"
_MERGED = pathlib.Path(__file__).resolve().parent / "injection_sweep.csv"


def load(pattern):
    rows = []
    for csv_path in sorted(_OUTPUT.glob("continuous_*/result.csv")):
        for row in csv.DictReader(csv_path.open()):
            if row["pattern"] == pattern:
                rows.append(row)
    if not rows:
        sys.exit(f"no continuous_*/result.csv rows for pattern {pattern!r}; run make sim-injection-sweep")
    return rows


def main():
    pattern = sys.argv[1] if len(sys.argv) > 1 else "uniform_random"
    rows = load(pattern)

    with _MERGED.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {_MERGED} ({len(rows)} points, pattern={pattern})")

    by_vc = defaultdict(list)
    for row in rows:
        by_vc[int(row["vc"])].append(
            (float(row["injection_rate"]),
             float(row["accepted_bits_per_cycle"]),
             float(row["mean_latency"])))
    for vc in by_vc:
        by_vc[vc].sort()

    knobs = {(r["max_unique_ids"], r["max_outstanding"]) for r in rows}
    if len(knobs) != 1:
        print(f"WARNING: rows mix NSU settings {knobs}; the curves are not comparable")
    print(f"max_unique_ids={rows[0]['max_unique_ids']} max_outstanding={rows[0]['max_outstanding']}")

    print(f"\n{'rate':>6} " + " ".join(f"{'vc'+str(v):>18}" for v in sorted(by_vc)))
    rates = sorted({p[0] for pts in by_vc.values() for p in pts})
    for rate in rates:
        cells = []
        for vc in sorted(by_vc):
            hit = [p for p in by_vc[vc] if p[0] == rate]
            cells.append(f"{hit[0][1]:8.0f}/{hit[0][2]:<9.0f}" if hit else f"{'-':>18}")
        print(f"{rate:>6} " + " ".join(cells))
    print("\n(cells are accepted_bits_per_cycle / mean_latency)")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("(matplotlib absent; the table above is the deliverable)")
        return

    fig, (ax_bw, ax_lat) = plt.subplots(1, 2, figsize=(11, 4))
    for vc in sorted(by_vc):
        xs = [p[0] for p in by_vc[vc]]
        ax_bw.plot(xs, [p[1] for p in by_vc[vc]], marker="o", label=f"vc{vc}")
        ax_lat.plot(xs, [p[2] for p in by_vc[vc]], marker="o", label=f"vc{vc}")
    caption = (f"{pattern}, max_unique_ids={rows[0]['max_unique_ids']}, "
               f"max_outstanding={rows[0]['max_outstanding']}")
    ax_bw.set(xlabel="offered injection rate", ylabel="accepted throughput (bits/cycle)")
    ax_lat.set(xlabel="offered injection rate", ylabel="mean latency (cycles)")
    for ax in (ax_bw, ax_lat):
        ax.legend()
        ax.grid(alpha=0.3)
    fig.suptitle(caption)
    fig.tight_layout()
    out = pathlib.Path(__file__).resolve().parent / "injection_sweep.png"
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
```

The caption carries `max_unique_ids` and `max_outstanding` because both bound the result. The mixed-settings warning exists because `output/` accumulates across runs and a stale row would silently corrupt the curve.

- [ ] **Step 2: Add the sweep target to the root `Makefile`**

Append after the `sim` target:

```makefile
# Injection-rate sweep: four VC configs x nine rates, one point per make sim.
# MAX_UNIQUE_IDS defaults to 256 here, not 1: the shipped default collapses every
# manager onto one downstream AXI id at each subordinate, which serialises them
# and flattens the curve. MAX_OUTSTANDING is inherited so the bring-up step can
# sweep it without editing this target.
# Heavy: rebuilds Verilator once per VC config. Run on WSL.
SWEEP_RATES ?= 0.05 0.1 0.2 0.3 0.4 0.5 0.7 0.85 1.0
SWEEP_VCS   ?= 1 2 4 8

.PHONY: sim-injection-sweep
sim-injection-sweep:
	@for vc in $(SWEEP_VCS); do \
	    for r in $(SWEEP_RATES); do \
	        echo ">>> sweep vc$$vc rate $$r"; \
	        $(MAKE) sim TB=tb_mesh_4x4_vc$${vc}_rob PATTERN=$(PATTERN) SEED=$(_SEED) \
	            INJECTION_MODE=1 INJECTION_RATE=$$r \
	            MAX_UNIQUE_IDS=$(if $(MAX_UNIQUE_IDS),$(MAX_UNIQUE_IDS),256) \
	            $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING)) || exit 1; \
	    done; \
	done
	$(PYTHON3) sim/tools/plot_injection_sweep.py $(PATTERN)
```

- [ ] **Step 3: Smoke the plot on two points**

Do not run the full sweep yet. Prove the glob-merge and the table first.

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1_rob PATTERN=uniform_random SEED=1 INJECTION_MODE=1 INJECTION_RATE=0.1 MAX_UNIQUE_IDS=256 > /tmp/s1.log 2>&1 && make sim TB=tb_mesh_4x4_vc1_rob PATTERN=uniform_random SEED=1 INJECTION_MODE=1 INJECTION_RATE=0.5 MAX_UNIQUE_IDS=256 > /tmp/s2.log 2>&1 && python3 sim/tools/plot_injection_sweep.py uniform_random'
```

Expected: `wrote .../injection_sweep.csv (2 points, pattern=uniform_random)`, a two-row table, and no mixed-settings warning.

- [ ] **Step 4: Commit**

```bash
git add sim/tools/plot_injection_sweep.py Makefile
git commit -m "feat(sim): sim-injection-sweep and the latency-throughput plot

The sweep knows nothing about any run. It knows how to call make sim. The plot
globs result.csv rather than concatenating, because cat would repeat the header,
and it warns when rows mix NSU settings, because output/ accumulates."
```

---

## Task 8: Fault injection, bring-up, then the figures

The co-sim is the gate. Fault injection runs first: a knob that changes nothing when set to an absurd value was never wired.

**Files:**
- Modify then revert: nothing. All fault injection is by command-line parameter.
- Modify: `docs/backlog.md`, `docs/development.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the answer to whether the fabric is the bottleneck, and two figures.

Both fault injections read field 10 of the row, `accepted_bits_per_cycle`. The tag is identical across
the two runs of each pair, so each overwrites the previous `result.csv`. Read it between runs.

- [ ] **Step 1: Fault-inject `MAX_OUTSTANDING`**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && TAG=sim/verilator/output/continuous_mesh_4x4_vc1_rob_uniform_random_r1.0_s1; for n in 1 32; do make sim TB=tb_mesh_4x4_vc1_rob PATTERN=uniform_random SEED=1 INJECTION_MODE=1 INJECTION_RATE=1.0 MAX_UNIQUE_IDS=256 MAX_OUTSTANDING=$n > /tmp/fo_$n.log 2>&1 || { echo "run failed at n=$n"; break; }; echo "MAX_OUTSTANDING=$n -> $(tail -1 $TAG/result.csv | cut -d, -f10) bits/cyc"; done'
```

Expected: `MAX_OUTSTANDING=1` yields a far lower number than `=32`.

If the two numbers are equal, **the plusarg is not reaching the c_model**. Check the `[Config]` line in the run log first. Stop and report. Do not proceed to any figure: a knob that changes nothing when set to an absurd value was never wired.

- [ ] **Step 2: Fault-inject `MAX_UNIQUE_IDS`**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && TAG=sim/verilator/output/continuous_mesh_4x4_vc1_rob_hotspot_r1.0_s1; for n in 1 256; do make sim TB=tb_mesh_4x4_vc1_rob PATTERN=hotspot SEED=1 INJECTION_MODE=1 INJECTION_RATE=1.0 MAX_UNIQUE_IDS=$n > /tmp/fu_$n.log 2>&1 || { echo "run failed at n=$n"; break; }; echo "MAX_UNIQUE_IDS=$n -> $(tail -1 $TAG/result.csv | cut -d, -f10) bits/cyc"; done'
```

Expected: `MAX_UNIQUE_IDS=1` yields lower throughput than `=256` on `hotspot`, where all sixteen managers converge on one subordinate and the collapse serialises them.

Equal numbers mean the plusarg is not reaching the NSU. Stop and report.

- [ ] **Step 3: Mode 1 on all four patterns, scoreboard silent**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && for p in neighbor transpose uniform_random hotspot; do make sim TB=tb_mesh_4x4_vc1 PATTERN=$p SEED=12345 INJECTION_MODE=1 INJECTION_RATE=0.4 MAX_UNIQUE_IDS=256 > /tmp/m1_$p.log 2>&1 || { echo "FAIL $p"; break; }; tail -1 /tmp/m1_$p.log; done; grep -c "Unexpected RData" sim/verilator/output/continuous_*/run.log | grep -v ":0" || echo "zero Unexpected RData everywhere"'
```

Expected: four `CONTINUOUS PASS` lines and `zero Unexpected RData everywhere`. A non-zero count means the scoreboard was armed in mode 1.

- [ ] **Step 4: Mode 0 regression and `constrained_random`**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && for p in neighbor transpose uniform_random hotspot; do make sim TB=tb_mesh_4x4_vc1 PATTERN=$p SEED=12345 > /tmp/r_$p.log 2>&1 || { echo "FAIL $p"; break; }; tail -1 /tmp/r_$p.log; done; make sim TB=tb_mesh_4x4_vc1 PATTERN=constrained_random SEED=12345 2>&1 | tail -1'
```

Expected: four `DIRECTED PASS` and one `CR PASS`.

- [ ] **Step 5: Bring-up — how much throughput does the NI's buffer sizing cost?**

`max_outstanding` is the depth of a physical buffer in the subordinate-side NI: one metadata entry per in-flight transaction. It is area. FlooNoC names the same parameter `MaxTxns` and ships 32. **The headline figure runs at 32, the design point.** Raising it until the curve looks good would plot a machine nobody will build.

512 is the reachable ceiling under `hotspot`: RoB Enabled admits `ROB_CAPACITY = 1 << ROB_IDX_WIDTH = 32` per NMU per direction (`rob.hpp:80`, `ni_flit_constants.h:234`), single-beat stimulus, sixteen NMUs. A 512-deep pool cannot bind, so it models an ideal sink. It is a **reference line, not a candidate headline setting.**

`rob_idx` indexes the requesting NMU's reorder buffer, not anything in the NSU, which carries it as opaque payload (`meta_buffer.hpp:16`, restamped at `packetize.hpp:93,108`). Two NMUs may both hold `rob_idx=7`. So the ceiling is pattern-dependent: 32 under the permutations (`neighbor`, `transpose`, one source per destination), 512 under `hotspot`, and 32-expected-512-peak under `uniform_random`.

Run both arms on the two extreme VC counts:

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && for mo in 32 512; do for vc in 1 8; do make sim TB=tb_mesh_4x4_vc$${vc}_rob PATTERN=uniform_random SEED=1 INJECTION_MODE=1 INJECTION_RATE=1.0 MAX_UNIQUE_IDS=256 MAX_OUTSTANDING=$mo > /dev/null 2>&1; echo -n "mo=$mo vc=$vc: "; tail -1 sim/verilator/output/continuous_mesh_4x4_vc$${vc}_rob_uniform_random_r1.0_s1/result.csv | cut -d, -f10; done; done'
```

Four numbers. Compute and record two things:

1. **VC value at the design point**: `(vc8 - vc1) / vc1` at `mo=32`. This is what the headline figure shows.
2. **What the NI buffer costs**: `(mo512 - mo32) / mo32`, at each VC count. This is the throughput the 32-entry pool gives up against an ideal sink.

If `vc1` and `vc8` coincide at **both** arms, neither the fabric nor the NI pool binds. Suspect the `AxiMasterPort` per-channel queue (16, `wrap_defaults.hpp:12`), the router per-VC input depth (4), or the router inject credit. **Stop and report** — a VC comparison is meaningless when VC count changes nothing.

**Record all four numbers.** The next reader will not rerun this.

- [ ] **Step 6: Run the headline sweep at the design point**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim-injection-sweep PATTERN=uniform_random MAX_OUTSTANDING=32 > /tmp/sweep.log 2>&1; echo "rc=$?"; tail -20 /tmp/sweep.log'
```

Expected: 36 runs, then the merged table and the PNG. `MAX_OUTSTANDING=32` is the shipped NI buffer depth and the figure's subject.

The throughput curve must show a knee. A flat line at every VC count means VC count changes nothing, which Step 5 should already have caught. Do not publish a flat curve as a VC comparison.

- [ ] **Step 7: Run the diagnostic reference line**

Only the two extreme VC counts, only to bound the headline figure from above.

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim-injection-sweep PATTERN=uniform_random MAX_OUTSTANDING=512 SWEEP_VCS="1 8" > /tmp/sweep_ideal.log 2>&1; echo "rc=$?"; tail -12 /tmp/sweep_ideal.log'
```

`plot_injection_sweep.py` will warn that the rows mix NSU settings, because `output/` now holds both arms. That warning is doing its job. Move the 512 rows aside before replotting the headline:

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && mkdir -p sim/verilator/output_ideal && for d in sim/verilator/output/continuous_*; do grep -q ",512," $d/result.csv 2>/dev/null && mv $d sim/verilator/output_ideal/; done; python3 sim/tools/plot_injection_sweep.py uniform_random'
```

Expected: the headline table replots with no warning, and `output_ideal/` holds the reference rows.

- [ ] **Step 8: Update the docs**

In `docs/backlog.md`, replace the "Next round: injection rate in `make sim`, VC comparison figures" section with a `## Done` entry recording: the interface (`make sim TB= PATTERN= [INJECTION_MODE=1 INJECTION_RATE= INJECTION_COUNT=] [MAX_UNIQUE_IDS= MAX_OUTSTANDING=]`), the sweep target, the four bring-up numbers from Step 5, the VC delta at the design point, and what the 32-entry NI buffer costs against the ideal sink. Strike the two remaining "Open" bullets.

Add one new backlog item: **`max_outstanding` as its own sweep axis.** It is an NI buffer-depth parameter, hence area. Sweeping it against VC count would say how the two trade off. Not this round.

In `docs/development.md`, replace every reference to `run-traffic`, `sim-saturation`, `collect_saturation.py` and `plot_saturation.py`. Add the `INJECTION_COUNT` mode-dependent default, because a default that changes with another variable will surprise someone. State that `MAX_OUTSTANDING` is an architectural parameter, not a knob to raise until a curve looks good.

- [ ] **Step 9: Full ctest**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
```

Expected: `397 tests passed, 0 tests failed`.

- [ ] **Step 10: Commit**

```bash
git add docs/
git commit -m "docs: injection-mode interface, sweep results, retire the traffic vocabulary

Records the four bring-up numbers beside the figures, so the next reader does
not have to rerun them. max_outstanding is documented as an NI buffer depth,
not a knob to raise until the curve looks good."
```

- [ ] **Step 11: Report, do not push**

Summarize: both fault injections, the four mode-1 patterns with zero `Unexpected RData`, the mode-0 regression, the four bring-up numbers, the VC delta at the design point, what the 32-entry buffer costs against the ideal sink, and the final ctest count.

---

## Self-Review

**Spec coverage**

| spec element | task |
|---|---|
| monitor prints the latency sample count | 1 |
| `cmodel_nsu_create` widened, not duplicated | 2 |
| one hand-written caller updated | 2 |
| plusargs read and echoed into `run.log` | 3 |
| eleven `tb_top_*.sv` regenerated | 3 |
| `+injection_mode` selects the send loop | 4 |
| scoreboard disarmed in mode 1, both calls skipped | 4 |
| `INJECTION_COUNT` mode-dependent default | 5 |
| `run-traffic` folded into `run-directed` | 5 |
| `INJ_RATIO` / `TRAFFIC_TXNS` / `IDS_PER_TILE` / `TRAFFIC_TAG` retired | 5 |
| `collect_saturation.py` deleted | 5 |
| per-run `result.csv`, six parameters plus two measurements | 6 |
| `mean_latency` sample-weighted | 6 |
| `sim-injection-sweep`, `MAX_UNIQUE_IDS=256` | 7 |
| two figures from one CSV, caption carries both knobs | 7 |
| `plot_saturation.py` replaced | 7 |
| fault injection first | 8 |
| headline at the design point (`MAX_OUTSTANDING=32`) | 8 |
| diagnostic reference line (`=512`, ideal sink) | 8 |
| `max_outstanding` documented as an NI buffer depth | 8 |
| mode 0 unchanged on all four patterns | 5, 8 |
| mode 1 on all four patterns, zero `Unexpected RData` | 8 |
| `gen_test_patterns.py` untouched | all; enforced by Global Constraints |
| no new unit tests | all; enforced by Global Constraints |

**Placeholder scan.** Clean. The headline sweep runs at `MAX_OUTSTANDING=32` unconditionally: it is the shipped NI buffer depth, so it is a design fact, not a measurement outcome. The bring-up produces numbers to report, not a setting to choose.

**Type consistency.** `emit_result_csv.py`'s CLI flags (`--injection-mode`, `--max-unique-ids`, ...) match the recipe invocation in Task 5 Step 2 exactly. The column order in Task 6 Step 1 matches the header in the Interfaces block. `plot_injection_sweep.py` reads `vc`, `injection_rate`, `accepted_bits_per_cycle`, `mean_latency`, `pattern`, `max_unique_ids`, `max_outstanding`, all of which `emit_result_csv.py` writes. Task 8 Step 5 cuts field 10 of the row, which is `accepted_bits_per_cycle`.

**Four things the spec did not say, resolved here.**

1. `cat output/*/result.csv` would repeat the header on every file. `plot_injection_sweep.py` globs and merges with `csv.DictReader` instead. No `cat`.
2. Two `initial` blocks in `user_node_endpoint.sv` need `injection_mode`, and SystemVerilog does not order them. Each reads it independently through `get_injection_mode()` rather than sharing a module-scope variable.
3. `DIRECTED_TAG` names both modes now, so it becomes `SIM_TAG`. `RUN_TAG` was already taken by `run-tb-top`. A backslash continuation inside its `?=` value would fold the next line's leading whitespace into the tag, so `_CONTINUOUS := $(filter 1,$(INJECTION_MODE))` hoists the condition and the assignment stays on one line.
4. `STIM_ROOT` gains `_n$(INJECTION_COUNT)`. Without it, switching between mode 0 (4 transactions) and mode 1 (200) would silently reuse the other mode's stimulus directory.

**A note on the two commits that span tasks.** Task 5 stages but does not commit, because the recipe it writes calls `emit_result_csv.py`, which Task 6 creates. Committing Task 5 alone would leave mode 1 broken. The pair lands together at Task 6 Step 6. Every other task commits on its own.
