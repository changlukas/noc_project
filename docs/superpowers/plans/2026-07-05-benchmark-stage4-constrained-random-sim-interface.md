# Benchmark Stage 4 — constrained-random axis + unified `make sim` interface — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the constrained-random axis (rand_master + tb-level reorder_compare) as a gated, seed-reproducible run, align naming to DV-standard (`directed` / `constrained_random`, retire `data_integrity`/`transport`), and collapse per-run invocation into a single `make sim TB=tb_<topo> PATTERN=<pat> [SEED=]` where PATTERN selects the axis.

**Architecture:** Endpoint collapses from a 3-way `ifdef` (directed / transport / data_integrity) to a 2-way (`ifdef TB_DIRECTED` = file_master+scoreboard; `else` = constrained_random = old transport config, WRAP/EXC + RAND_RESP). The retired `data_integrity` (INCR+MAPPED) config is deleted; `constrained_random` is the single non-directed flavor, so it needs **no compile define** — only `TB_DIRECTED` is defined (ponytail: one define, not two). A new `run-constrained-random` recipe mirrors Stage 3's `run-directed` (build CR flavor → run → gate on reorder_compare `%Error` + `cmp_eos` drained + non-vacuous). The root `sim` target is rewritten from a `run_benchmark.py` caller into a DV run launcher: `TOPOLOGY=$(TB:tb_%=%)`, `RUN_CLASS` derived from PATTERN, recursive-make to the right backend, `SEED` random-and-recorded if unset. Host paths live in the existing root `local.mk`.

**Tech Stack:** SystemVerilog (pulp `axi-0.39.7` VIP + FlooNoC `axi_reorder_compare`), Verilator 5.048 `--binary --timing` on WSL (+ z3), GNU Make.

## Global Constraints

- **Environment: WSL/Linux only** for build + co-sim. Set host paths ONCE in the repo-root `local.mk` (gitignored `/local.mk`, already included via `sim/build_config.mk:13-16`): `BUILD_ROOT := $(HOME)/noc_build`, `PYTHON3 := python3`, `VERILATOR := verilator`. The `/mnt/e/.../build` tree holds Windows-COFF artifacts WSL `ld` rejects, so a native `BUILD_ROOT` is mandatory. With `local.mk` set, gates need no `BUILD_ROOT=`/`PYTHON3=` on the command line. yaml-cpp must already be built under `$HOME/noc_build` (Stage-3 env setup; if missing: `make build-yamlcpp BUILD_ROOT=$HOME/noc_build DEPS_SRC=/mnt/e/05_NoC/noc_project/build/cmodel/_deps` then `cp -r /mnt/e/05_NoC/noc_project/build/cmodel/_deps/yaml-cpp-src $HOME/noc_build/cmodel/_deps/`).
- **Redirect every WSL build/run to a log + capture EXIT + tail** (builds take minutes; do not let a turn end mid-build). No `pgrep` inside `wsl bash -c`.
- **Naming (DV-standard):** two axes `directed` / `constrained_random`; retire `data_integrity` and `transport` everywhere (RUN_CLASS values + endpoint define). Only `TB_DIRECTED` remains a define; `constrained_random` is the default `else` flavor with no define.
- **PATTERN drives class:** `neighbor|transpose|uniform_random|hotspot` → directed; `constrained_random` → CR. The emitter (`gen_test_patterns.py`) only accepts the 4 spatial patterns — **CR must NOT be passed to the emitter** (CR needs no stimulus files).
- **CR gate:** reorder_compare error-free (grep `%Error`) + `cmp_eos` all drained (tb exit already requires it) + non-vacuous. Scoreboard failures are `$warning`/`$error` leaving `rc==0`, so the gate greps stdout.
- **Seed:** `+verilator+seed+N`. `SEED` unset → `$$RANDOM$$RANDOM` (bash, 30-bit), printed at run start + baked into the run tag/log/output path. Given → replay. No double-run.
- **Fault-injection first:** prove reorder_compare fires on a deliberate beat corruption before trusting a clean CR run.
- **Add-only to directed:** Stage 3 directed behavior must stay byte-identical — the `ifdef TB_DIRECTED` arm and `run-directed` recipe are untouched except where explicitly folded into `sim`.
- **Class selection via recursive make**, never a target-specific override: `OBJ_DIR`/`TBTOP_EXE` are `:=` parse-time in `sim/verilator/Makefile:31,92`.

## File Structure

- Modify `sim/tb/user_node_endpoint.sv` — collapse 3-way ifdef → 2-way; `else` = constrained_random (old transport config); delete data_integrity arm; fix comments.
- Modify `sim/verilator/Makefile` — RUN_CLASS filter/default → `{directed constrained_random}`/`constrained_random`; drop the `transport`/`TB_TRANSPORT_RUN` define block; add `run-constrained-random` recipe.
- Modify `sim/vcs/Makefile` — RUN_CLASS filter/default renamed (VCS is Linux-workstation dry-run; keep in sync, do not run).
- Modify `Makefile` (repo root) — `RUN_CLASS ?= constrained_random`; rewrite the `sim:` target (PATTERN→class, `TB:tb_%=%`, seed random+record, recursive-make backend, drop `run_benchmark.py`); document the 3 `local.mk` lines in a comment.
- Modify `sim/tools/gen_tb_top.py` — comment/naming (`transport`→`constrained_random`); the `ifndef TB_DIRECTED` reorder_compare guard already covers CR (CR is not TB_DIRECTED), no logic change.

---

### Task 1: Retire `data_integrity` + rename to `constrained_random` (endpoint + 3 Makefiles + gen_tb_top)

**Files:**
- Modify: `sim/tb/user_node_endpoint.sv`
- Modify: `sim/verilator/Makefile`, `sim/vcs/Makefile`, `Makefile` (root)
- Modify: `sim/tools/gen_tb_top.py`

**Interfaces:**
- Produces: RUN_CLASS ∈ `{directed, constrained_random}`; the non-directed endpoint flavor = constrained_random (rand_master WRAP/EXC/INCR/FIXED + `axi_rand_slave` MAPPED=0 RAND_RESP=1); `build-verilator` default = constrained_random; only `+define+TB_DIRECTED` is emitted (CR = no define).

- [ ] **Step 1: Collapse the endpoint typedef ifdef 3-way → 2-way**

In `sim/tb/user_node_endpoint.sv`, replace the `` `elsif TB_TRANSPORT_RUN `` arm through the `` `else `` data_integrity arm (lines ~193-221) with a single `` `else `` = constrained_random (uses the old transport constraints):

```systemverilog
`else
    // constrained_random: full AXI conformance corner (INCR/FIXED/WRAP + exclusive),
    // random burst/size/addr within the per-master region; RAND_RESP slave.
    typedef axi_test::axi_rand_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime),
        .MAX_READ_TXNS(MAX_READ_TXNS_IN_FLIGHT), .MAX_WRITE_TXNS(MAX_WRITE_TXNS_IN_FLIGHT),
        .AXI_MAX_BURST_LEN(MAX_BURST_LEN),
        .AXI_EXCLS(1'b1), .AXI_ATOPS(1'b0), .UNIQUE_IDS(1'b0),
        .AXI_BURST_FIXED(1'b1), .AXI_BURST_INCR(1'b1), .AXI_BURST_WRAP(1'b1)
    ) rand_master_t;
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b0), .RAND_RESP(1'b1)
    ) rand_slave_t;
`endif
```

(The `ifdef TB_DIRECTED` arm at lines 181-192 is unchanged. Result: 2-way — directed vs constrained_random(else). No `TB_TRANSPORT_RUN`/`TB_CONSTRAINED_RANDOM` define.)

- [ ] **Step 2: Fix the endpoint comments that name the retired flavors**

- Line ~280: `// ---- existing rand flavors (data_integrity default + TB_TRANSPORT_RUN) ----` → `// ---- constrained_random flavor (rand_master + tb-level reorder_compare) ----`
- Lines ~294-296 (`// Permutation pairing (both flavors): ...`) → drop "both flavors": `// Permutation pairing: this master targets ONLY node (NUM_NODES-1-NODE_ID) so the`
- Update the module header "Run flavors" block: replace the `default`/`+define+TB_TRANSPORT_RUN` lines with the two current flavors — `default : constrained_random — rand_master (WRAP/EXC), RAND_RESP slave, tb-level reorder_compare.` and keep the `+define+TB_DIRECTED` line.

- [ ] **Step 3: Rename RUN_CLASS in `sim/verilator/Makefile`**

- Line ~14 comment: replace the `data_integrity`/`transport` descriptions with `constrained_random (default) : rand_master WRAP/EXC, RAND_RESP slave, reorder_compare.` and `directed : file_master + scoreboard, INCR two-phase.`
- Line 15: `RUN_CLASS ?= data_integrity` → `RUN_CLASS ?= constrained_random`
- Lines 16-17: `ifeq ($(filter $(RUN_CLASS),data_integrity transport directed),)` → `ifeq ($(filter $(RUN_CLASS),directed constrained_random),)` and the `$(error ...)` message → `RUN_CLASS must be directed or constrained_random (got '$(RUN_CLASS)')`
- Lines 75-77: delete the `ifeq ($(RUN_CLASS),transport)` / `+define+TB_TRANSPORT_RUN` / `endif` block entirely. Keep the `ifeq ($(RUN_CLASS),directed)` / `+define+TB_DIRECTED` block. (constrained_random adds no define.)

- [ ] **Step 4: Rename RUN_CLASS in `sim/vcs/Makefile` (dry-run, keep in sync)**

- Line ~5 comment, line 6 default, lines 7-8 filter+error: mirror Step 3 (`data_integrity | transport` → `directed | constrained_random`; default `constrained_random`). Also update the transport define reference near `sim/vcs/Makefile:143-144` if it names `TB_TRANSPORT_RUN` (rename to drop it — VCS directed/CR mirror verilator: only `+define+TB_DIRECTED` for directed). Do NOT run VCS (Linux-workstation only).

- [ ] **Step 5: Repoint the root default + gen_tb_top comments**

- `Makefile` (root) line ~124: `RUN_CLASS ?= data_integrity` → `RUN_CLASS ?= constrained_random`
- `sim/tools/gen_tb_top.py`: replace `transport`/`data_integrity` wording in the emitted header comments with `constrained_random`; the reorder_compare `ifndef TB_DIRECTED` guard is unchanged (CR is not TB_DIRECTED). Regenerating is not needed here (Task 3 rebuilds).

- [ ] **Step 6: Build-gate both flavors on WSL (compile + CR smoke + directed regression)**

Ensure root `local.mk` exists (create if missing):
```bash
printf 'BUILD_ROOT := $(HOME)/noc_build\nPYTHON3 := python3\nVERILATOR := verilator\n' > local.mk
```
Then (redirect + tail):
```bash
wsl.exe -e bash -lc 'cd /mnt/e/05_NoC/noc_project && FL=$HOME/noc_build/filelist_mesh_2x4_vc1.f; \
  make -C sim/verilator run-tb-top TOPOLOGY=mesh_2x4_vc1 RUN_CLASS=constrained_random FILELIST_F=$FL NUM_READS=4 NUM_WRITES=4 SEED=1 > $HOME/t1_cr.log 2>&1; echo "CR EXIT=$?"; tail -6 $HOME/t1_cr.log; grep -c "PASS: all 8 nodes done, non-vacuous" $HOME/t1_cr.log; \
  make -C sim/verilator run-directed TOPOLOGY=mesh_2x4_vc1 PATTERN=neighbor RUN_CLASS=directed FILELIST_F=$FL TXNS_PER_NODE=4 SEED=1 > $HOME/t1_dir.log 2>&1; echo "DIR EXIT=$?"; tail -4 $HOME/t1_dir.log'
```
Expected: CR build compiles, `PASS: all 8 nodes done, non-vacuous` count = 1 (rand + reorder_compare runs, exits 0); directed still `DIRECTED PASS` (naming change did not disturb directed). A bare `RUN_CLASS` (no value) now defaults to `constrained_random` and must still compile.

- [ ] **Step 7: Commit**

```bash
git add sim/tb/user_node_endpoint.sv sim/verilator/Makefile sim/vcs/Makefile Makefile sim/tools/gen_tb_top.py
git commit -m "refactor(sim): retire data_integrity, rename transport->constrained_random (2-flavor endpoint)"
```

---

### Task 2: `run-constrained-random` recipe + fault-injection proof

**Files:**
- Modify: `sim/verilator/Makefile` (add the recipe)
- (Task 2 fault step temporarily edits `sim/tools/gen_tb_top.py`, then reverts it.)

**Interfaces:**
- Consumes: Task 1 (constrained_random flavor + the tb-level reorder_compare under `ifndef TB_DIRECTED`).
- Produces: `make -C sim/verilator run-constrained-random RUN_CLASS=constrained_random TOPOLOGY=<t> SEED=<n> NUM_READS=<n> NUM_WRITES=<n>` → builds the CR flavor, runs with `+verilator+seed+<SEED>`, and reports `CR PASS`/`CR FAIL` gating on reorder_compare `%Error` + `cmp_eos` drained + non-vacuous.

- [ ] **Step 1: Add the `run-constrained-random` recipe**

In `sim/verilator/Makefile`, after `run-directed` (~line 231), add (CR has no stimulus files — no emitter, no `+stim_dir`):

```make
# Constrained-random run: rand_master (WRAP/EXC) + tb-level axi_reorder_compare.
# Gate on zero reorder_compare $error (%Error) + non-vacuous PASS (cmp_eos drain is
# in the tb exit). Seed is echoed into the tag so a FAIL is replayable.
CR_TAG ?= constrained_random_$(TOPOLOGY)_s$(SEED)
.PHONY: run-constrained-random
run-constrained-random: $(TBTOP_EXE)
	@[ "$(RUN_CLASS)" = "constrained_random" ] || { echo "ERROR: run-constrained-random requires RUN_CLASS=constrained_random (got '$(RUN_CLASS)')"; exit 1; }
	@mkdir -p output/$(CR_TAG)
	@echo "running $(CR_TAG) (seed=$(SEED) reads=$(NUM_READS) writes=$(NUM_WRITES))"
	$(TBTOP_EXE) \
	    "+num_reads=$(NUM_READS)" "+num_writes=$(NUM_WRITES)" \
	    "+verilator+seed+$(SEED)" \
	    "+perf_out=output/$(CR_TAG)/perf.json" \
	    "+perf_scenario=$(CR_TAG)" \
	    > output/$(CR_TAG)/run.log 2>&1; \
	rc=$$?; \
	echo "--- run.log (tail) ---"; tail -10 output/$(CR_TAG)/run.log; \
	if grep -qE '%Error' output/$(CR_TAG)/run.log; then \
	    echo "CR FAIL: reorder_compare error / assertion"; exit 1; \
	elif [ $$rc -ne 0 ] || ! grep -q 'PASS: all' output/$(CR_TAG)/run.log; then \
	    echo "CR FAIL: run did not reach non-vacuous PASS (rc=$$rc)"; exit 1; \
	else \
	    echo "CR PASS: $(CR_TAG) reorder_compare clean, drained, non-vacuous"; \
	fi
```

(`NUM_READS`/`NUM_WRITES`/`SEED` reuse the existing `run-tb-top` defaults at ~171-173.)

- [ ] **Step 2: Fault-injection — prove reorder_compare fires (MANDATORY, before trusting clean)**

reorder_compare compares each master's issued beats vs the beats arriving at its paired slave face. To prove the gate catches a transport corruption, temporarily perturb the slave-side compare tap in the generator. In `sim/tools/gen_tb_top.py`, find the compare-tap emit `assign cmp_slv_req[i] = axi_vip_types_pkg::vip_req_from_flat(slave_axi_req[DST]);` and wrap it with a throwaway fault guard (edit the Python string the generator emits):

```python
    w("`ifdef TB_CR_FAULT")
    w("        assign cmp_slv_req[i] = corrupt_w_data(axi_vip_types_pkg::vip_req_from_flat(slave_axi_req[DST]));")
    w("`else")
    w("        assign cmp_slv_req[i] = axi_vip_types_pkg::vip_req_from_flat(slave_axi_req[DST]);")
    w("`endif")
```

and emit a one-line helper above the generate (`function automatic ... corrupt_w_data` that XORs `w.data[0]`). If a function in generated scope is awkward, instead XOR inline: `... vip_req_from_flat(slave_axi_req[DST]); cmp_slv_req[i].w.data ^= (`TB_CR_FAULT ? 64'h1 : '0)` is not legal on a continuous assign — use the `ifdef` two-arm form above with a small combinational `always_comb` that copies then flips one W-data bit. Keep it minimal; it is reverted in Step 4.

Regenerate + build + run with the fault:
```bash
wsl.exe -e bash -lc 'cd /mnt/e/05_NoC/noc_project && python3 sim/tools/gen_tb_top.py --topology mesh_2x4_vc1 && \
  make -C sim/verilator run-constrained-random TOPOLOGY=mesh_2x4_vc1 RUN_CLASS=constrained_random FILELIST_F=$HOME/noc_build/filelist_mesh_2x4_vc1.f VERILATOR_EXTRA_FLAGS="+define+TB_CR_FAULT" NUM_READS=4 NUM_WRITES=4 SEED=1 > $HOME/t2_fault.log 2>&1; echo "EXIT=$?"; grep -E "^CR (PASS|FAIL)|%Error" $HOME/t2_fault.log | head'
```
Expected: `CR FAIL` with `%Error` from reorder_compare (the flipped W-data byte makes master-issued ≠ slave-arrived). If it does NOT fail, the comparator/gate has no teeth — STOP and investigate before trusting any clean run.

- [ ] **Step 3: Revert the fault + run clean**

```bash
git checkout sim/tools/gen_tb_top.py    # drop the throwaway TB_CR_FAULT guard
wsl.exe -e bash -lc 'cd /mnt/e/05_NoC/noc_project && python3 sim/tools/gen_tb_top.py --topology mesh_2x4_vc1 && \
  make -C sim/verilator run-constrained-random TOPOLOGY=mesh_2x4_vc1 RUN_CLASS=constrained_random FILELIST_F=$HOME/noc_build/filelist_mesh_2x4_vc1.f NUM_READS=4 NUM_WRITES=4 SEED=1 > $HOME/t2_clean.log 2>&1; echo "EXIT=$?"; grep -E "^CR (PASS|FAIL)" $HOME/t2_clean.log'
```
Expected: `CR PASS`. Confirm `git diff sim/tools/gen_tb_top.py` is empty (fault fully reverted).

- [ ] **Step 4: Commit** (only the recipe — the fault guard is reverted)

```bash
git add sim/verilator/Makefile
git commit -m "feat(sim): run-constrained-random recipe + reorder_compare gate (fault-injection verified)"
```

---

### Task 3: Unified `make sim TB= PATTERN= [SEED=]` + full-axis verification

**Files:**
- Modify: `Makefile` (repo root) — rewrite the `sim:` target; document `local.mk`.

**Interfaces:**
- Consumes: Task 1 (RUN_CLASS naming), Task 2 (`run-constrained-random`), Stage 3 (`run-directed`).
- Produces: `make sim TB=tb_<topo> PATTERN=<pat> [SEED=<n>]` — one entry point; PATTERN selects the axis; SEED unset → random+recorded.

- [ ] **Step 1: Rewrite the root `sim:` target**

In `Makefile`, replace the `sim:` recipe (~187-195, currently `build-verilator` + `run_benchmark.py`) with a PATTERN-driven recursive-make dispatcher:

```make
# Unified DV run launcher. TB selects the testbench (topology; accepts a tb_ prefix).
# PATTERN selects the axis: the 4 spatial patterns run directed (file_master +
# scoreboard); constrained_random runs rand_master + reorder_compare. SEED unset ->
# a random 30-bit seed is drawn and printed so any run is replayable.
TB      ?= mesh_4x4_vc1
PATTERN ?= neighbor
_TOPO   := $(TB:tb_%=%)
_CLASS  := $(if $(filter constrained_random,$(PATTERN)),constrained_random,directed)
_SEED   := $(if $(SEED),$(SEED),$(shell bash -c 'echo $$RANDOM$$RANDOM'))

.PHONY: sim
sim:
	@echo ">>> sim TB=$(_TOPO) PATTERN=$(PATTERN) class=$(_CLASS) SEED=$(_SEED)"
ifeq ($(_CLASS),constrained_random)
	$(MAKE) -C sim/verilator run-constrained-random TOPOLOGY=$(_TOPO) RUN_CLASS=constrained_random \
	    SEED=$(_SEED) $(if $(NUM_READS),NUM_READS=$(NUM_READS)) $(if $(NUM_WRITES),NUM_WRITES=$(NUM_WRITES))
else
	$(MAKE) -C sim/verilator run-directed TOPOLOGY=$(_TOPO) RUN_CLASS=directed \
	    PATTERN=$(PATTERN) SEED=$(_SEED) $(if $(TXN),TXNS_PER_NODE=$(TXN)) $(if $(HOTSPOT),HOTSPOT=$(HOTSPOT))
endif
```

`BUILD_ROOT`/`PYTHON3`/`VERILATOR`/`FILELIST_F` are NOT passed here — they flow from root `local.mk` through `sim/build_config.mk`. The `_SEED` echo + the backend's `_s$(SEED)` tag record the seed.

- [ ] **Step 2: Document `local.mk` (no new file)**

Add a comment block near the top of `Makefile` (by the existing per-host `local.mk` note ~73):

```make
# Per-host WSL config: create a gitignored `local.mk` at the repo root with:
#   BUILD_ROOT := $(HOME)/noc_build   # native-Linux build dir (WSL rejects /mnt COFF)
#   PYTHON3    := python3
#   VERILATOR  := verilator
# Then `make sim TB=tb_mesh_4x4_vc1 PATTERN=hotspot` needs no path/tool args.
```

- [ ] **Step 3: Verify FILELIST_F default resolves once BUILD_ROOT is in local.mk**

The default `FILELIST_F = $(COSIM_ROOT)/filelist_$(TOPOLOGY).f` (`sim/build_config.mk:113`) is a text file of source paths; confirm it works on WSL with only `local.mk` set (no `FILELIST_F=` override). Run Step 4; if Verilator rejects the filelist path, add `FILELIST_F := $(BUILD_ROOT)/filelist_$(TB).f` to `local.mk` and note it in the comment.

- [ ] **Step 4: Full-axis verification (the Stage-4 gate)**

On WSL, with `local.mk` set (redirect each; the first CR/directed build is the heavy one):

```bash
wsl.exe -e bash -lc 'cd /mnt/e/05_NoC/noc_project && \
  for p in neighbor transpose uniform_random hotspot constrained_random; do \
    make sim TB=tb_mesh_4x4_vc1 PATTERN=$p SEED=7 > $HOME/t3_$p.log 2>&1; echo "$p EXIT=$?"; \
    grep -E "^(DIRECTED|CR) (PASS|FAIL)|PASS: all 16" $HOME/t3_$p.log | tail -2; \
  done; \
  echo "=== seed random+record (no SEED) ==="; \
  make sim TB=tb_mesh_4x4_vc1 PATTERN=constrained_random > $HOME/t3_rand.log 2>&1; grep -E ">>> sim|^CR (PASS|FAIL)" $HOME/t3_rand.log | head'
```
Expected:
- The 4 spatial patterns → `DIRECTED PASS` + `PASS: all 16 nodes` (Stage 3 directed behavior unchanged through the new entry point — regression).
- `constrained_random` → `CR PASS`.
- No-SEED run → the `>>> sim ... SEED=<big number>` line shows a drawn seed and the run tag carries it (replayable).

- [ ] **Step 5: Update spec/backlog + commit**

Mark Stage 4 done: update `docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md` Stages row 4 + `docs/backlog.md` (Stage 4 → DONE, Stage 5 NEXT). Commit:
```bash
git add Makefile docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md docs/backlog.md
git commit -m "feat(sim): unified make sim TB=/PATTERN=/SEED= launcher; Stage 4 done"
```

---

## Self-Review

- **Spec coverage:** S4-1 naming/retire (Task 1); S4-2 PATTERN→class (Task 3 dispatcher); S4-3/S4-8 unified interface + recursive-make + root local.mk + default repoint + emitter guard (Tasks 1,3); S4-4 seed random+record (Task 3); S4-5 CR checker + gate (Task 2); S4-6 grep `%Error` gate (Task 2); S4-7 fault-injection first (Task 2 Step 2). Stages a/b/c = Tasks 1/2/3.
- **Spec deviation flagged:** spec S4-1 said rename `TB_TRANSPORT_RUN`→`TB_CONSTRAINED_RANDOM`; this plan drops the CR define entirely (CR = the default `else`, only `TB_DIRECTED` remains) — fewer defines, same behavior (ponytail). Called out in Architecture + Task 1 Step 1.
- **Placeholder scan:** none — every step carries the SV/Make/commands. Task 2 Step 2's fault helper notes the legal SV form (two-arm `ifdef` + small `always_comb` flip, not an illegal continuous-assign compound).
- **Type/name consistency:** `constrained_random` (RUN_CLASS + PATTERN value), `run-constrained-random` recipe, `TB_DIRECTED` sole define, `$(TB:tb_%=%)`, `_CLASS`/`_SEED`/`_TOPO` locals, `CR PASS`/`DIRECTED PASS` gate strings — consistent across Tasks 1-3 and the Stage-3 `run-directed` they compose with.
- **Regression guard:** Task 1 Step 6 + Task 3 Step 4 re-run directed to prove Stage 3 behavior is unchanged through the rename + new entry point.
