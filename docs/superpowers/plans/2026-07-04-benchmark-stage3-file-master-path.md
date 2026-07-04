# Benchmark Stage 3 — file_master path (directed data axis) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the pulp `axi_file_master` + `axi_scoreboard` + two-phase (write → read) directed data-integrity axis into the co-sim endpoint and tb generator, and prove it scoreboard-clean on a single-node bring-up mesh then the full 4x4 across all 4 traffic patterns.

**Architecture:** Add a compile-time `TB_DIRECTED` flavor to `user_node_endpoint.sv` (mirrors the existing `TB_TRANSPORT_RUN` switch): under it the endpoint drives `master_dv` with `axi_file_master` (per-node `write.txt`/`read.txt` from the Stage 2 emitter, path via `+stim_dir` plusarg), keeps the `axi_rand_slave(MAPPED=1)` as the tile memory, and instantiates an `axi_scoreboard` **inside the endpoint** on `master_dv` (golden from its own W, check on its own R — end-to-end through the NoC). Because the scoreboard is endpoint-internal, `gen_tb_top.py` only needs to `ifdef`-guard the tb-level `axi_reorder_compare` + its `cmp_eos` exit contribution so the directed build does not require the (mutually-exclusive) transport checker. A new `RUN_CLASS=directed` in the Verilator Makefile passes `+define+TB_DIRECTED`, invokes the emitter, and greps stdout for scoreboard mismatch `$warning`s (the checker fails via `$warning`, not exit code).

**Tech Stack:** SystemVerilog (pulp `axi-0.39.7` VIP), Verilator 5.048 `--binary --timing` on WSL (+ z3 for the MAPPED rand_slave), Python 3 (emitter + gen_tb_top), GNU Make.

## Global Constraints

- **Environment: WSL/Linux only** for build + co-sim (Verilator 5.048 + z3). Windows keeps only the Python emitter/gen unit tests and lint. (`feedback_run_on_wsl_linux`.) **Every build/lint/run gate MUST use the native-Linux build root** (the `/mnt/e` tree holds Windows-COFF artifacts that WSL `ld` rejects): run from Git Bash as `wsl.exe -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make -C sim/verilator <target> TOPOLOGY=<t> RUN_CLASS=<c> BUILD_ROOT=$HOME/noc_build FILELIST_F=$HOME/noc_build/filelist_<t>.f PYTHON3=python3 VERILATOR=verilator [NUM_READS=.. NUM_WRITES=..]'`. The `BUILD_ROOT`/`FILELIST_F` overrides are mandatory — any bare `make` command in a task step below is shorthand for this full form.
- **Reuse pulp IP as-is:** `axi_file_master`, `axi_scoreboard`, `axi_rand_slave(MAPPED=1)` are used unmodified. The only custom SV is the endpoint wiring; no new dependency, no new file (spec File-tree: "新增檔案 0", except the bring-up topology YAML, see Open Decision B).
- **Directed stimulus is INCR-only, `atop=0`, full strobe, full readback** (read set ⊇ write set by construction) — the scoreboard's supported subset (spec D5). An INCR-only run must not raise a scoreboard unsupported-burst `$warning`; if it does, the emitter produced illegal stimulus → fail, do not exempt (spec Harness).
- **Scoreboard failure is `$warning` "Unexpected RData" (`axi_test.sv:2141-2148`), not `$error`/exit code** — the gate greps stdout (spec Checker model / D6).
- **Add-only to the endpoint's existing flavors:** the `default` (data_integrity rand) and `TB_TRANSPORT_RUN` (transport rand) branches stay behaviorally untouched; the directed path is a third `ifdef` arm. Do not delete the Stage-2 emitter YAML path or `run_benchmark.py` — those are Stage 5 / D7.
- **Widths from the DUT SSoT:** endpoint params come from `ni_params_pkg` (generated from `specgen/source/constants.yaml`); the emitter reads the same file. `ID_WIDTH=8, ADDR_WIDTH=64, DATA_WIDTH=256`.

---

## Open Decisions

> **Both resolved by the 2026-07-04 cross-review (Codex GPT-5.5 + fresh-context Claude), verified against source — see `cross-review/REVIEW_AGGREGATE.md`.**
> **A → per-node** (cross-node barrier confirmed dead for scoreboard correctness: MAPPED slave commits W before B at `axi_test.sv:1494/1508/1517`, and D3 makes every master read only its own writes). **B → keep 1x1** (`route_compute` ejects LOCAL on `dst==src`, `router.hpp:72`, so self-route is legal). The barrier add-back notes below are retained only as a record of the rejected alternative. A separate review finding (G1: "the two-phase read-check is vacuous") was **REFUTED** at `axi_test.sv:2050` (the `xx`-init push means `delete(0)` on B leaves `[data]`, not empty) — but its remedy, a fault-injection negative control, is adopted as a mandatory Task 3 gate (Step 4a).

**Decision A — two-phase barrier granularity (spec D4 says cross-node; this plan builds per-node).**
Spec D4 mandates a *cross-node* barrier: every node finishes its write phase before *any* node starts reading. This plan instead implements a **per-node** two-phase (`fork run_aw; run_w; wait_b; join` then `fork run_ar; wait_r; join`, all inside each endpoint), which needs **zero new ports** and no tb-top coordination.

Rationale for per-node: the endpoint's scoreboard builds golden from *its own* W beats and checks *its own* R beats, and invariant D3 partitions write addresses by source so **a master only ever reads addresses it wrote itself**. That master's own `wait_b` guarantees its write reached the MAPPED slave before it issues any AR. No node's readback depends on another node's write, so the cross-node barrier adds ports + tb wiring (endpoint `write_done_o`, tb-top AND-reduce, broadcast `reads_go_i`) for no correctness gain — textbook dead flexibility.

If the reviewers or the user want the spec's cross-node barrier kept (e.g. defense-in-depth, or a concern I've missed about MAPPED-slave write-visibility ordering), Task 1 + Task 2 gain the barrier wiring; the rest of the plan is unchanged. **Recommendation: per-node.** This plan is written for per-node; the barrier add-back is called out inline in Tasks 1-2.

**Decision B — single-node bring-up topology.** Spec Stage 3 success = "單 node → 4x4". No 1x1 topology exists (smallest is `mesh_2x4_vc1`, 8 nodes; `transpose` needs a square mesh so 2x4 can't run all 4 patterns). This plan **adds `sim/topologies/mesh_1x1_vc1.yaml`** (one node writing/reading its own tile via the LOCAL port) as the bring-up step: it isolates file_master + scoreboard + two-phase + the NI (NMU/NSU) loopback from inter-router routing. **Recommendation: add 1x1.** Alternative: bring up on `mesh_2x4_vc1` with `neighbor` only. Task 3 assumes 1x1; if rejected, retarget Task 3 to `mesh_2x4_vc1 --pattern neighbor`.

---

## File Structure

- Modify `sim/tb/user_node_endpoint.sv` — add the `TB_DIRECTED` flavor (file_master + in-endpoint scoreboard + two-phase); fix the stale "scoreboard VCS-only" header comment.
- Modify `sim/tools/gen_tb_top.py` — `ifdef TB_DIRECTED` guards around the tb-level `axi_reorder_compare` block and its `cmp_eos` exit-gate term; update the two header comment blocks that assert scoreboard is VCS-only.
- Modify `sim/verilator/Makefile` — add `RUN_CLASS=directed` (→ `+define+TB_DIRECTED`), `STIM_DIR` plusarg plumbing, and a `run-directed` recipe (emit → run → grep scoreboard warnings).
- Create `sim/topologies/mesh_1x1_vc1.yaml` — bring-up topology (Open Decision B).
- (No change to `gen_test_patterns.py` — the Stage 2 emitter is complete and consumed as-is.)

---

### Task 1: Endpoint `TB_DIRECTED` flavor — file_master + in-endpoint scoreboard + two-phase

**Files:**
- Modify: `sim/tb/user_node_endpoint.sv`

**Interfaces:**
- Consumes: `axi_test::axi_file_master`, `axi_test::axi_scoreboard`, `axi_test::axi_rand_slave` (`sim/dv/axi-0.39.7/src/axi_test.sv`); `AXI_BUS_DV`; existing endpoint ports (unchanged — no new ports under the per-node decision).
- Produces: under `+define+TB_DIRECTED`, an endpoint that reads `<stim_dir>/node<NODE_ID>/{write,read}.txt`, drives `master_dv` two-phase, checks R against golden on `master_dv`, and raises `run_done`/`end_of_sim_o` after the read phase. `+stim_dir=<dir>` plusarg selects the stimulus root (default `sim/test_patterns/directed`).

- [ ] **Step 1: Read the file and locate the two edit regions**

Read `sim/tb/user_node_endpoint.sv`. The two regions to change:
- The `ifdef TB_TRANSPORT_RUN` / `else` typedef block (`~176-204`) — add a leading `ifdef TB_DIRECTED` arm for the file_master + MAPPED slave typedefs.
- The stimulus `initial` blocks (`~206-247`) — add a `TB_DIRECTED` two-phase path and the scoreboard instance.

- [ ] **Step 2: Fix the stale header comment (scoreboard is usable on Verilator for directed)**

Replace the header paragraph that begins `// axi_scoreboard is withdrawn from the Verilator flow:` … through `// (VCS-only, backlog).` (lines ~11-13) with:

```systemverilog
// pulp axi_scoreboard is usable on the Verilator directed axis (2026-07-04 spike +
// review): the 8'hxx->8'h00 2-state collapse only bites reads of never-written
// addresses, which a full-readback directed run never issues. Wired in-endpoint on
// master_dv under +define+TB_DIRECTED. (Rationale: spec D6 / cross-review aggregate.)
```

Also extend the "Run flavors" comment block to add the third flavor:

```systemverilog
//   +define+TB_DIRECTED : data integrity — axi_file_master two-phase (write ->
//                         barrier -> read) + in-endpoint axi_scoreboard on
//                         master_dv, MAPPED rand_slave as tile memory. Stimulus
//                         from <stim_dir>/node<ID>/{write,read}.txt (+stim_dir=).
```

- [ ] **Step 3: Add the `TB_DIRECTED` typedef arm**

Immediately before the existing `` `ifdef TB_TRANSPORT_RUN `` (line ~176, after the `MAX_BURST_LEN` localparam), open a new arm and make the existing switch its `elsif`/`else`. Concretely, change `` `ifdef TB_TRANSPORT_RUN `` to `` `ifdef TB_DIRECTED `` + a new block, then `` `elsif TB_TRANSPORT_RUN ``:

```systemverilog
`ifdef TB_DIRECTED
    typedef axi_test::axi_file_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime)
    ) file_master_t;
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b1)
    ) rand_slave_t;
    typedef axi_test::axi_scoreboard #(
        .IW(ID_WIDTH), .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .UW(1), .TT(TestTime)
    ) scoreboard_t;
`elsif TB_TRANSPORT_RUN
```

Leave the existing `TB_TRANSPORT_RUN` and final `` `else `` (default data_integrity) blocks intact — the trailing `` `endif `` now closes the 3-way chain.

- [ ] **Step 4: Add the directed stimulus + scoreboard, guard the rand path**

The existing `rand_master`/`rand_slave` declarations + the two stimulus `initial` blocks (`~206-247`) must run only in the rand flavors. Wrap them and add the directed path. Replace the block from `rand_master_t rand_master;` through the closing of the second `initial` (`rand_slave.run();` … `end`) with:

```systemverilog
    // run_done drives end_of_sim_o for ALL flavors: declare it exactly ONCE here,
    // above the ifdef, and delete the per-arm copies (the existing declaration at
    // user_node_endpoint.sv:215 moves up to this point).
    logic run_done = 1'b0;

`ifdef TB_DIRECTED
    file_master_t file_master;
    rand_slave_t  rand_slave;
    scoreboard_t  scoreboard;

    // Stimulus root: <stim_dir>/node<NODE_ID>/{write,read}.txt (emitter output).
    string stim_dir = "sim/test_patterns/directed";
    string write_path;
    string read_path;

    // MAPPED memory slave = this node's tile memory (persists across both phases).
    initial begin
        rand_slave = new(slave_dv);
        rand_slave.reset();
        @(posedge rst_ni);
        rand_slave.run();
    end

    // In-endpoint scoreboard on master_dv: golden from this node's W, check on
    // its R (end-to-end round trip through the NoC). enable_all_checks turns on
    // read-data + B-resp + R-resp checks; monitor() forks the sampling.
    initial begin
        scoreboard = new(master_dv);
        scoreboard.reset();
        @(posedge rst_ni);
        scoreboard.enable_all_checks();
        scoreboard.monitor();
    end

    // Directed driver: per-node two-phase. load_files() fills the queues (do NOT
    // call run(): it re-forks all five and double-consumes the queues, spec
    // Two-phase). Phase 1 drains all writes (wait_b => committed at the slave);
    // phase 2 issues reads, checked by the scoreboard against golden.
    initial begin
        void'($value$plusargs("stim_dir=%s", stim_dir));
        write_path = $sformatf("%s/node%0d/write.txt", stim_dir, NODE_ID);
        read_path  = $sformatf("%s/node%0d/read.txt",  stim_dir, NODE_ID);
        file_master = new(master_dv);
        file_master.load_files(read_path, write_path);
        @(posedge rst_ni);
        fork file_master.run_aw(); file_master.run_w(); file_master.wait_b(); join
        fork file_master.run_ar(); file_master.wait_r(); join
        run_done = 1'b1;
    end
`else
    // ---- existing rand flavors (data_integrity default + TB_TRANSPORT_RUN) ----
    rand_master_t rand_master;
    rand_slave_t  rand_slave;

    int unsigned num_reads;
    int unsigned num_writes;
    // ... (existing bodies unchanged; run_done declaration REMOVED from here — it
    // now lives once above the ifdef) ...
`endif
```

Note: `run_done` is declared once above the ifdef (both flavors assign it); neither arm re-declares it. The `always_ff` that registers `end_of_sim_o <= run_done` stays OUTSIDE the ifdef. Keep `num_reads`/`num_writes` inside the `else` arm only (file_master ignores them). The `bw_monitor` + `txn_cnt_o` block stays unchanged and applies to all flavors (AW/AR handshake count is non-vacuous for file_master too).

> **If Open Decision A flips to cross-node barrier:** add `output logic write_done_o` (set after the phase-1 join) and `input logic reads_go_i` (wait before phase 2) to the port list, and gate phase 2 on `wait(reads_go_i)`. Task 2 then AND-reduces `write_done_o` across nodes and broadcasts `reads_go_i`.

- [ ] **Step 5: Compile-gate the untouched rand path (non-regression)**

The directed edits are all under `ifdef TB_DIRECTED`; the default build must be byte-behavior-identical. On WSL, rebuild + run the default flavor:

```bash
make -C sim/verilator run-tb-top TOPOLOGY=mesh_2x4_vc1 RUN_CLASS=data_integrity \
    PYTHON3=python3 VERILATOR=verilator NUM_READS=4 NUM_WRITES=4 SEED=1
```

Expected: build succeeds, run ends `PASS: all 8 nodes done, non-vacuous`. (Confirms the ifdef restructure didn't disturb the rand path.)

- [ ] **Step 6: Commit**

```bash
git add sim/tb/user_node_endpoint.sv
git commit -m "feat(endpoint): TB_DIRECTED flavor - file_master + in-endpoint scoreboard + two-phase"
```

---

### Task 2: `gen_tb_top` — ifdef-guard the transport checker for the directed build

**Files:**
- Modify: `sim/tools/gen_tb_top.py`

**Interfaces:**
- Consumes: nothing new.
- Produces: the emitted `tb_top_<topo>.sv` wraps the `axi_reorder_compare` generate block and its `cmp_eos` exit-gate term in `` `ifndef TB_DIRECTED `` / `` `endif ``, so a `+define+TB_DIRECTED` build compiles without the transport checker and exits on `end_of_sim` + non-vacuous alone. The default (no define) output is unchanged except for the added guard lines.

- [ ] **Step 1: Guard the reorder_compare block — but keep `cmp_eos` declared unconditionally**

In `emit_tb_top` (`gen_tb_top.py`), the compare section (`w("...")` block ~584-634) declares `cmp_mst_req/rsp`, `cmp_slv_req/rsp` (`:589-592`), `logic cmp_eos [NUM_NODES];` (`:593`), then the `g_compare` generate (`:594-633`).

**Critical (cross-review C1):** the watchdog `$display` at `:471-472` references `cmp_eos[i]` in a *separate* `initial` block. If `cmp_eos`'s declaration is compiled out under `TB_DIRECTED`, that reference is an undeclared identifier → hard compile error. So keep `cmp_eos` declared **unconditionally** and guard only the taps + the generate:

- Move the `w("    logic cmp_eos [NUM_NODES];")` line so it is emitted **first**, right after the `// Checking ...` comment, OUTSIDE the guard.
- Then emit `w("\`ifndef TB_DIRECTED")`.
- Then the four `cmp_mst_req/rsp` + `cmp_slv_req/rsp` tap declarations (`:589-592`) and the whole `g_compare` generate (`:594-633`).
- Then emit `w("\`endif")` (after `w("    end : g_compare")` + its trailing `w("")`).

The watchdog `$display` at `:471-472` stays untouched: under `TB_DIRECTED`, `cmp_eos` is declared but undriven (reads `x`), which is harmless in a diagnostic print. The exit gate (Step 2) still needs the split so the directed build does not *wait* on the never-driven `cmp_eos`.

- [ ] **Step 2: Guard the `cmp_eos` term in the exit gate**

In the exit-logic block, the loop currently emits:

```python
    w("            for (int i = 0; i < NUM_NODES; i++)")
    w("                all_done &= end_of_sim[i] & cmp_eos[i];  // compares drained too")
```

Replace those two lines with a directed/non-directed split so the directed build waits on `end_of_sim` only:

```python
    w("            for (int i = 0; i < NUM_NODES; i++)")
    w("`ifdef TB_DIRECTED")
    w("                all_done &= end_of_sim[i];  // scoreboard is in-endpoint; no cmp_eos")
    w("`else")
    w("                all_done &= end_of_sim[i] & cmp_eos[i];  // compares drained too")
    w("`endif")
```

- [ ] **Step 3: Update the two stale "scoreboard VCS-only" comment blocks**

In `emit_tb_top`'s header comment, the lines emitting `// (pulp axi_scoreboard is VCS-only: its 'x wildcard collapses` … `// under Verilator 2-state.)` — replace with a directed-aware note:

```python
    w("// (pulp axi_scoreboard is not wired at tb level: in the +define+TB_DIRECTED")
    w("// directed build it lives inside each endpoint on master_dv; this reorder")
    w("// compare block is the transport-axis checker and is compiled out under")
    w("// TB_DIRECTED.)")
```

- [ ] **Step 4: Regenerate + confirm the default (non-directed) tb still compiles**

```bash
python3 sim/tools/gen_tb_top.py --topology mesh_4x4_vc1
grep -n 'ifndef TB_DIRECTED\|ifdef TB_DIRECTED' sim/tb/tb_top_mesh_4x4_vc1.sv
```

Expected: the guards appear around the compare block and in the exit gate. Then rebuild the default flavor on WSL to prove the guarded output still compiles the transport path:

```bash
make -C sim/verilator run-tb-top TOPOLOGY=mesh_2x4_vc1 RUN_CLASS=data_integrity \
    PYTHON3=python3 VERILATOR=verilator NUM_READS=4 NUM_WRITES=4 SEED=1
```

Expected: `PASS: all 8 nodes done, non-vacuous` (default build unaffected by the guards — `TB_DIRECTED` undefined).

- [ ] **Step 5: Commit**

```bash
git add sim/tools/gen_tb_top.py
git commit -m "feat(gen_tb_top): ifdef-guard reorder_compare so TB_DIRECTED build drops the transport checker"
```

---

### Task 3: Makefile `RUN_CLASS=directed` + single-node bring-up (scoreboard clean)

**Files:**
- Modify: `sim/verilator/Makefile`
- Create: `sim/topologies/mesh_1x1_vc1.yaml`

**Interfaces:**
- Consumes: Task 1 endpoint, Task 2 gen_tb_top, the Stage 2 emitter (`gen_test_patterns.py --format file_master`).
- Produces: `make -C sim/verilator run-directed TOPOLOGY=mesh_1x1_vc1 PATTERN=neighbor RUN_CLASS=directed` emits stimulus, builds the `directed` flavor, runs, and reports `DIRECTED PASS` iff zero scoreboard mismatch `$warning`s and a non-vacuous `PASS`. The recipe guards on `RUN_CLASS=directed` (fail-loud) so a bare invocation cannot silently run the no-scoreboard binary.

- [ ] **Step 1: Create the bring-up topology**

Model it on `sim/topologies/mesh_2x4_vc1.yaml` (read it first for the exact schema). Create `sim/topologies/mesh_1x1_vc1.yaml`:

```yaml
topology:
  name: mesh_1x1_vc1
  x_dim: 1
  y_dim: 1
  num_vc: 1
```

(Copy any additional required keys verbatim from `mesh_2x4_vc1.yaml`; keep only structural values changed.)

- [ ] **Step 2: Add the `directed` run-class to the Makefile**

In `sim/verilator/Makefile`, extend the `RUN_CLASS` validation (line ~15) and add the define. Change the filter to include `directed`:

```make
ifeq ($(filter $(RUN_CLASS),data_integrity transport directed),)
$(error RUN_CLASS must be data_integrity, transport or directed (got '$(RUN_CLASS)'))
endif
```

After the existing `ifeq ($(RUN_CLASS),transport)` block (line ~75-77), add:

```make
ifeq ($(RUN_CLASS),directed)
VERILATOR_FLAGS += +define+TB_DIRECTED
endif
```

- [ ] **Step 2b: Lint the directed config first (localizes the compile-time findings)**

This is the FIRST time `TB_DIRECTED` is actually defined — the endpoint 3-way ifdef, the `run_done` hoist, and the gen_tb_top guards (C1) all compile for the first time here. Lint on an existing topology (no 1x1 yet) so a compile error is localized to the SV, not the topology/emitter:

```bash
make -C sim/verilator TOPOLOGY=mesh_2x4_vc1 RUN_CLASS=directed \
    PYTHON3=python3 VERILATOR=verilator VERILATOR_EXTRA_FLAGS='--lint-only'
```

Expected: lint passes (no `%Error`). If it fails on an undeclared `cmp_eos`, Step 1/Task-2 C1 was not applied correctly (the declaration must stay unconditional). Fix before proceeding — do NOT let the first directed compile be the functional run.

- [ ] **Step 3: Add the `run-directed` recipe**

Add near `run-tb-top` (after line ~190). It emits per-node stimulus for one pattern, runs the directed binary with `+stim_dir`, and greps for scoreboard warnings. `num_reads`/`num_writes` are passed only to size the tb watchdog (the file_master count comes from the files), so set them to `TXNS_PER_NODE`.

```make
# Directed data-integrity run: emit file_master stimulus for one pattern, run the
# TB_DIRECTED binary, gate on zero axi_scoreboard mismatch $warning + non-vacuous PASS.
PATTERN        ?= neighbor
TXNS_PER_NODE  ?= 4
HOTSPOT        ?= 5
STIM_ROOT      ?= $(COSIM_ROOT)/test_patterns/directed_$(TOPOLOGY)_$(PATTERN)
DIRECTED_TAG   ?= directed_$(TOPOLOGY)_$(PATTERN)_s$(SEED)
GEN_PATTERNS   := $(COSIM_ROOT)/tools/gen_test_patterns.py
_HOTSPOT_ARGS  := $(if $(filter hotspot,$(PATTERN)),--hotspot $(HOTSPOT),)

.PHONY: run-directed
run-directed: $(TBTOP_EXE)
	@mkdir -p output/$(DIRECTED_TAG)
	$(PYTHON3) $(GEN_PATTERNS) --topology $(TOPOLOGY) --format file_master \
	    --out $(STIM_ROOT) --pattern $(PATTERN) \
	    --transactions-per-node $(TXNS_PER_NODE) --size 5 --len 0 \
	    --memory-size 0x40000 --seed $(SEED) $(_HOTSPOT_ARGS)
	@echo "running $(DIRECTED_TAG) (txns/node=$(TXNS_PER_NODE) pattern=$(PATTERN))"
	$(TBTOP_EXE) \
	    "+stim_dir=$(STIM_ROOT)" \
	    "+num_reads=$(TXNS_PER_NODE)" "+num_writes=$(TXNS_PER_NODE)" \
	    "+verilator+seed+$(SEED)" \
	    "+perf_out=output/$(DIRECTED_TAG)/perf.json" \
	    "+perf_scenario=$(DIRECTED_TAG)" \
	    > output/$(DIRECTED_TAG)/run.log 2>&1; \
	rc=$$?; \
	echo "--- run.log (tail) ---"; tail -12 output/$(DIRECTED_TAG)/run.log; \
	if grep -qE 'Unexpected RData|Unexpected W last|Not supported (AW|AR) burst|Atomic transfers not supported' \
	        output/$(DIRECTED_TAG)/run.log; then \
	    echo "DIRECTED FAIL: scoreboard mismatch/malformed stimulus"; exit 1; \
	elif [ $$rc -ne 0 ] || ! grep -q 'PASS: all' output/$(DIRECTED_TAG)/run.log; then \
	    echo "DIRECTED FAIL: run did not reach non-vacuous PASS (rc=$$rc)"; exit 1; \
	else \
	    echo "DIRECTED PASS: $(DIRECTED_TAG) scoreboard clean, non-vacuous"; \
	fi
```

- [ ] **Step 3b: Fault-injection negative control — prove the scoreboard fires on a written-address corruption (MANDATORY, before any clean run)**

Project discipline (`feedback_verification_ip_fault_injection`) + cross-review U1: a *clean* pass is meaningless until the checker is shown to fire. The spike only faulted an *unwritten* address (the 2-state-collapse path); this control faults a **written-and-read-back** value — the path the co-sim actually relies on. Source analysis says the golden survives as `[data]` after B (`axi_test.sv:2050` init push means `delete(0)` leaves the value, not empty), so a corrupted readback MUST fire `Unexpected RData`; this step proves it empirically.

Inject via a throwaway define — corrupt the R data returned to `master_dv` (endpoint bridge, the `assign master_dv.r_data = master_axi_rsp_i.rdata;` at `user_node_endpoint.sv:113`). Temporarily wrap it:

```systemverilog
`ifdef TB_DIRECTED_FAULT
    assign master_dv.r_data = master_axi_rsp_i.rdata ^ 64'h1;  // flip 1 bit: golden!=R
`else
    assign master_dv.r_data = master_axi_rsp_i.rdata;
`endif
```

Build + run the 1x1 neighbor with the fault define (via the existing `VERILATOR_EXTRA_FLAGS` hook — no Makefile edit):

```bash
make -C sim/verilator run-directed TOPOLOGY=mesh_1x1_vc1 PATTERN=neighbor RUN_CLASS=directed \
    PYTHON3=python3 VERILATOR=verilator TXNS_PER_NODE=4 SEED=1 \
    VERILATOR_EXTRA_FLAGS='+define+TB_DIRECTED_FAULT'
```

Expected: `DIRECTED FAIL: scoreboard mismatch/malformed stimulus` (the gate greps `Unexpected RData`, which MUST fire). If it does NOT fire, the read-data check is vacuous — STOP, do not proceed; this would contradict the `:2050` source trace, so investigate the golden lifetime before trusting any clean run. Then **remove the `TB_DIRECTED_FAULT` wrapper** (revert to the plain `assign`) and rebuild clean for Step 4.

- [ ] **Step 4: Run the single-node bring-up (neighbor = self-loop on 1x1)**

On WSL:

```bash
make -C sim/verilator run-directed TOPOLOGY=mesh_1x1_vc1 PATTERN=neighbor RUN_CLASS=directed \
    PYTHON3=python3 VERILATOR=verilator TXNS_PER_NODE=4 SEED=1
```
(`RUN_CLASS=directed` is mandatory — the recipe guard rejects any other class to prevent a vacuous false PASS.)

Expected: `DIRECTED PASS: directed_mesh_1x1_vc1_neighbor_s1 scoreboard clean, non-vacuous`.

OBSERVE (record verbatim before interpreting):
- No `Unexpected RData` and no `Not supported ... burst` / `Atomic transfers` warning in the log.
- The `PASS: all 1 nodes done, non-vacuous` line.
- `node0 txn_cnt > 0`.

If a scoreboard warning fires here, STOP — this is the isolated endpoint+NI loopback; debug the two-phase / scoreboard wiring before scaling (do not proceed to Task 4). If the 1x1 self-route abort-fatals in `route_compute` (dst==src via LOCAL unsupported), that is a bring-up finding: report it (per `feedback_subagent_report_bugs_before_fixing`) rather than working around — fall back to Open Decision B's `mesh_2x4_vc1 --pattern neighbor`.

- [ ] **Step 5: Commit**

```bash
git add sim/verilator/Makefile sim/topologies/mesh_1x1_vc1.yaml
git commit -m "feat(sim): RUN_CLASS=directed + run-directed recipe + 1x1 bring-up topology"
```

---

### Task 4: 4x4 co-sim — all 4 patterns scoreboard clean (Stage 3 gate)

**Files:** none (run + observe only).

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: the Stage 3 success evidence — `mesh_4x4_vc1` directed, `DIRECTED PASS` on `neighbor`, `transpose`, `uniform_random`, `hotspot`.

- [ ] **Step 1: Run all 4 patterns on 4x4**

On WSL, run each (the first builds the directed 4x4 binary; the rest reuse it):

```bash
for p in neighbor transpose uniform_random hotspot; do
  make -C sim/verilator run-directed TOPOLOGY=mesh_4x4_vc1 PATTERN=$p RUN_CLASS=directed \
      BUILD_ROOT=$HOME/noc_build FILELIST_F=$HOME/noc_build/filelist_mesh_4x4_vc1.f \
      PYTHON3=python3 VERILATOR=verilator TXNS_PER_NODE=4 SEED=1 || break
done
```

Expected: four `DIRECTED PASS: directed_mesh_4x4_vc1_<pattern>_s1 scoreboard clean, non-vacuous` lines.

OBSERVE per pattern: zero `Unexpected RData`, `PASS: all 16 nodes done, non-vacuous`, every `txn_cnt > 0`.

**Scale watch (cross-review C3):** `axi_scoreboard.monitor()` forks `6 + 2·2**IW` = 518 coroutines per endpoint (`ID_WIDTH=8`), ≈ 8300 under `--timing` at 4x4, on top of file_master/rand_slave/bw_monitor. Most park idle on `wait(size>0)`, but the build + run cost is untested at this fan-out — the 1x1 bring-up (518 coroutines) does NOT bound it. Record the actual 4x4 directed build time and per-pattern wall-clock in the Step 3 result note. If the build or run is impractically slow, that is a real finding to report (not silently absorb) — a smaller `MAX_READ/WRITE`-style mitigation or a scoreboard-IW cap would need its own round; do not scale a slow checker blindly.

- [ ] **Step 2: If a pattern fails, triage — do not mask**

A scoreboard mismatch on `hotspot`/`uniform_random` (many-to-one tiles) but clean on `neighbor`/`transpose` (bijection) points at the per-src offset partition (`alloc_unique_offset`) — the same class as the 2026-07-01 slot-overlap bug. Capture the mismatch (`actual vs expected` addresses from the log), check whether it is a whole-slot stride shift (generator) vs scattered corruption (fabric), and report before fixing (`feedback_dont_silence_the_checker`, `feedback_subagent_report_bugs_before_fixing`). Record any real fabric finding in `docs/backlog.md`.

- [ ] **Step 3: Record the Stage 3 result**

Update the spec Stages table (row 3) and `docs/backlog.md` "In progress" table: mark Stage 3 DONE with the passing evidence (topologies + patterns + `pass/warn` counts), and set Stage 4 (`rand conformance`) as NEXT.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md docs/backlog.md
git commit -m "docs: Stage 3 file_master path done - 4x4 directed scoreboard clean, 4 patterns"
```

---

## Self-Review

- **Spec coverage (Stage 3 row + §元件處置 "改寫"):** endpoint file_master + two-phase + scoreboard (Task 1); gen_tb_top checker wiring / exit change (Task 2); single-node → 4x4, 4-pattern scoreboard clean (Tasks 3-4). `gen_test_patterns` is already Stage 2 (consumed, not re-touched). `matrix.yaml`/`run_regress.py` rewrite is Stage 5 — deliberately out of scope; Task 3's `run-directed` recipe is the minimal Stage-3 run path, not the harness.
- **Spec deviations flagged, not silent:** two-phase barrier granularity (Open Decision A — per-node vs spec's cross-node D4) and bring-up topology (Open Decision B — new 1x1). Both carry a recommendation + the exact add-back if a reviewer/user disagrees.
- **Checker discipline:** gate greps `Unexpected RData` + unsupported-burst/atomic `$warning`s (not exit code, per D6); fault path (Task 3 Step 4, Task 4 Step 2) reports before fixing. Bring-up isolates the mechanism (1x1) before scale (4x4).
- **Add-only:** all endpoint changes under `ifdef TB_DIRECTED`; Task 1 Step 5 + Task 2 Step 4 prove the rand path is non-regressed. No IP modified, no new dependency; the only new file is the bring-up YAML.
- **Placeholder scan:** none — every code step carries the actual SV/Make/YAML/commands.
- **Type/name consistency:** `TB_DIRECTED` define, `RUN_CLASS=directed`, `+stim_dir=`, `file_master_t`/`rand_slave_t`/`scoreboard_t` typedefs, `run-directed` recipe, and the `<stim_dir>/node<ID>/{write,read}.txt` layout match across Tasks 1-4 and the Stage 2 emitter output.
