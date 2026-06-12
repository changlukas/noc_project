# VCS FSDB Per-Pattern Waveform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opt-in (`FSDB=1`) Verdi FSDB waveform dumping for the VCS cosim flow, one independent `.fsdb` per pattern, for both testbenches, plus a `run-all-fsdb` batch target.

**Architecture:** SV dump blocks guarded by `` `ifdef FSDB_DUMP `` in the two VCS top modules (`tb_top_vcs`, `tb_genamba`); the define and Verdi PLI link flags are added only when `FSDB=1`, and FSDB builds use `_fsdb`-suffixed simv/csrc artifacts so the two modes never share binaries. Per-pattern fsdb paths are passed at runtime via a `+fsdb=` plusarg into the existing `output/<scenario>/` directories.

**Tech Stack:** GNU Make (bash recipes), SystemVerilog, VCS + Verdi PLI (workstation only). Spec: `docs/superpowers/specs/2026-06-12-vcs-fsdb-per-pattern-design.md`.

**Verification constraints:** VCS/Verdi do not exist on the Windows dev host. Local gates are: (a) `make -n` dry-run diffs proving `FSDB=0` compile/run commands are unchanged — with one intentional, explicitly-diffed exception: the stale-fsdb `rm -f` line added in Task 4, (b) `verible-verilog-syntax` parse of edited SV including the activated dump branch (sed-strip directives), (c) the Verilator genamba build still compiling (proves the `ifdef` leaks nothing into the default flow). Real-FSDB acceptance happens on the workstation (Task 6 checklist).

---

### Task 1: FSDB dump block in `tb_top_vcs.sv`

**Files:**
- Modify: `cosim/sv/tb_top_vcs.sv` (insert before `endmodule`, line 41)

- [ ] **Step 1: Capture the no-define baseline (the "failing test")**

```bash
cd /d/02_NoC/noc_project
verible-verilog-syntax cosim/sv/tb_top_vcs.sv && echo BASELINE_OK
test "$(grep -c fsdb cosim/sv/tb_top_vcs.sv || true)" -eq 0 && echo NO_FSDB_YET
```

Expected: `BASELINE_OK`, `NO_FSDB_YET`.

- [ ] **Step 2: Add the guarded dump block**

In `cosim/sv/tb_top_vcs.sv`, insert between the `tb_top u_tb (...)` instantiation and `endmodule`:

```systemverilog
    // FSDB waveform dump — compiled in only by the VCS flow with FSDB=1
    // (+define+FSDB_DUMP). Path comes from +fsdb=<abs-path>; the Makefile
    // run recipe supplies output/<scenario>/tb_top.fsdb.
`ifdef FSDB_DUMP
    initial begin
        string fsdb_path;
        if (!$value$plusargs("fsdb=%s", fsdb_path))
            fsdb_path = "dump.fsdb";
        $fsdbDumpfile(fsdb_path);
        $fsdbDumpvars(0, tb_top_vcs); // depth 0 = full hierarchy below top
    end
`endif
```

- [ ] **Step 3: Verify syntax — both branches**

verible v0.0-4007 does NOT parse inactive `ifdef` branches (verified
empirically), so check twice: once as-is, once with the conditional
directives stripped so the dump block becomes active code:

```bash
verible-verilog-syntax cosim/sv/tb_top_vcs.sv && echo PARSE_OK
sed -E '/^[[:space:]]*`(ifdef|ifndef|else|endif)/d' cosim/sv/tb_top_vcs.sv > /tmp/tb_top_vcs_active.sv
verible-verilog-syntax /tmp/tb_top_vcs_active.sv && echo ACTIVE_BRANCH_PARSE_OK
```

Expected: `PARSE_OK`, `ACTIVE_BRANCH_PARSE_OK`.

- [ ] **Step 4: Verify the default flow is untouched**

`tb_top_vcs.sv` is VCS-only (not in any Verilator source list — confirm):

```bash
grep -rn "tb_top_vcs" cosim/sources.mk cosim/verilator/Makefile; echo "exit=$?"
```

Expected: `exit=1` (no matches — Verilator never sees this file).

- [ ] **Step 5: Commit**

```bash
git add cosim/sv/tb_top_vcs.sv
git commit -m "feat(cosim): FSDB dump block in tb_top_vcs (ifdef FSDB_DUMP)"
```

---

### Task 2: FSDB dump block in `tb_genamba.sv`

**Files:**
- Modify: `cosim/sv/tb_genamba.sv` (insert near the end of the module, before the closing `endmodule`)

- [ ] **Step 1: Baseline**

```bash
verible-verilog-syntax cosim/sv/tb_genamba.sv && echo BASELINE_OK
```

Expected: `BASELINE_OK`.

- [ ] **Step 2: Add the guarded dump block**

Same shape as Task 1, top module name `tb_genamba`, default landing file
`tb_genamba.fsdb` so a plusarg-less run is still distinguishable from a
tb_top one:

```systemverilog
    // FSDB waveform dump — compiled in only by the VCS flow with FSDB=1
    // (+define+FSDB_DUMP). Path comes from +fsdb=<abs-path>; the Makefile
    // run recipe supplies output/genamba_<scenario>/tb_genamba.fsdb.
`ifdef FSDB_DUMP
    initial begin
        string fsdb_path;
        if (!$value$plusargs("fsdb=%s", fsdb_path))
            fsdb_path = "tb_genamba.fsdb";
        $fsdbDumpfile(fsdb_path);
        $fsdbDumpvars(0, tb_genamba); // depth 0 = full hierarchy below top
    end
`endif
```

- [ ] **Step 3: Verify syntax — both branches**

```bash
verible-verilog-syntax cosim/sv/tb_genamba.sv && echo PARSE_OK
sed -E '/^[[:space:]]*`(ifdef|ifndef|else|endif)/d' cosim/sv/tb_genamba.sv > /tmp/tb_genamba_active.sv
verible-verilog-syntax /tmp/tb_genamba_active.sv && echo ACTIVE_BRANCH_PARSE_OK
```

Expected: `PARSE_OK`, `ACTIVE_BRANCH_PARSE_OK`. (The sed also activates the
pre-existing `GENAMBA_DBG_AXI` monitor block — that block is plain
always-blocks and parses fine; it has no `` `else `` arm.)

- [ ] **Step 4: Verify the Verilator flow still builds this file (define absent)**

`tb_genamba.sv` IS compiled by Verilator (it is in `GENAMBA_SV_SRC`), so the
`ifdef` must be invisible there:

```bash
cd /d/02_NoC/noc_project/cosim/verilator && make genamba 2>&1 | tail -3
```

Expected: build completes (incremental — fast if obj_genamba is current);
no error mentioning `fsdb`.

- [ ] **Step 5: Commit**

```bash
git add cosim/sv/tb_genamba.sv
git commit -m "feat(cosim): FSDB dump block in tb_genamba (ifdef FSDB_DUMP)"
```

---

### Task 3: Makefile FSDB infrastructure (define, PLI flags, separate artifacts)

**Files:**
- Modify: `cosim/vcs/Makefile` (FSDB block after `include ../sources.mk` at line 31; rewire the two simv rules at lines 63-71 and 86-93)

- [ ] **Step 1: Capture pre-change dry-run baselines (the "failing test")**

```bash
cd /d/02_NoC/noc_project/cosim/vcs
make -n tb_top   > /tmp/base_tb_top.txt   2>&1 || true
make -n genamba  > /tmp/base_genamba.txt  2>&1 || true
```

- [ ] **Step 2a: Extend the existing `[WORKSTATION]` block (lines 21-29)**

The site-specific knobs go where the spec requires — inside the existing
`[WORKSTATION]` block, after `VCS_EXTRA ?=`. Defaults come from the user's
actual workstation config (`cosim/ref/Makefile`):

```make
# FSDB dumping (FSDB=1): Verdi install + PLI registration for $fsdbDump*.
# Defaults match the workstation's cosim/ref/Makefile (Verdi 2020.03,
# PLI under LINUXAMD64). Alternate older install seen in the ref flows:
#   /cadtools/synopsys/verdi/M-2017.03/share/PLI/VCS/LINUX64/{novas.tab,pli.a}
# LD_LIBRARY_PATH may additionally be needed for the FSDB runtime libs
# (e.g. $VERDI_HOME/share/PLI/lib/LINUXAMD64) — verify at first run, record here.
VERDI_HOME ?= /tools/verdi_2020.03
FSDB_PLI   ?= -P $(VERDI_HOME)/share/PLI/VCS/LINUXAMD64/novas.tab \
                 $(VERDI_HOME)/share/PLI/VCS/LINUXAMD64/pli.a
# Heavier known-working combo from the ref flow, for memory dump /
# interactive Verdi debug (slower sim, bigger fsdb):
#   FSDB_EXTRA += -debug_access+all -debug_all +fsdb+all +vcsd
FSDB_EXTRA ?=
```

- [ ] **Step 2b: Add the FSDB mode logic after `VCS_BUILD := $(BUILD_ROOT)/vcs` (line 33)**

```make
# --- FSDB waveform dumping (opt-in; Verdi PLI, workstation only) -------------
# FSDB=1 compiles a separate simv with +define+FSDB_DUMP and the Verdi PLI
# linked in. FSDB and non-FSDB builds use distinct simv/csrc names because
# make does not track flag changes — sharing a binary across modes would
# silently reuse the wrong one.
FSDB ?= 0
ifeq ($(FSDB),1)
ifeq ($(strip $(VERDI_HOME)),)
$(error FSDB=1 requires VERDI_HOME (Verdi install root) to be set)
endif
FSDB_COMPILE_FLAGS := +define+FSDB_DUMP $(FSDB_PLI) $(FSDB_EXTRA)
FSDB_SUFFIX := _fsdb
else
FSDB_COMPILE_FLAGS :=
FSDB_SUFFIX :=
endif
# Deferred (=) on purpose: SCENARIO / GENAMBA_SCENARIO are defined later in
# this file, and the per-run value must win when run-all-fsdb loops.
ifeq ($(FSDB),1)
FSDB_PLUSARG_TB_TOP  = "+fsdb=$(CURDIR)/output/$(SCENARIO)/tb_top.fsdb"
FSDB_PLUSARG_GENAMBA = "+fsdb=$(CURDIR)/output/genamba_$(GENAMBA_SCENARIO)/tb_genamba.fsdb"
else
FSDB_PLUSARG_TB_TOP  =
FSDB_PLUSARG_GENAMBA =
endif
# -----------------------------------------------------------------------------
```

- [ ] **Step 3: Rewire the two simv rules to suffixed artifacts**

Replace the genamba rule (current lines 63-71):

```make
SIMV_GENAMBA := $(VCS_BUILD)/simv_genamba$(FSDB_SUFFIX)

$(SIMV_GENAMBA): $(GENAMBA_SV_SRC) $(DPI_C_SRC) | check-yamlcpp
	@mkdir -p $(VCS_BUILD)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    -Mdir=$(VCS_BUILD)/csrc_genamba$(FSDB_SUFFIX) \
	    +incdir+$(COSIM_ROOT)/sv/genamba \
	    $(GENAMBA_DEFINES) $(FSDB_COMPILE_FLAGS) \
	    -top tb_genamba \
	    $(GENAMBA_SV_SRC) $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o $(SIMV_GENAMBA)
```

Replace the tb_top rule (current lines 86-93):

```make
SIMV_TB_TOP := $(VCS_BUILD)/simv_tb_top$(FSDB_SUFFIX)

$(SIMV_TB_TOP): $(TB_TOP_SV_SRC) $(COSIM_ROOT)/sv/tb_top_vcs.sv $(DPI_C_SRC) | check-yamlcpp
	@mkdir -p $(VCS_BUILD)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    -Mdir=$(VCS_BUILD)/csrc_tb_top$(FSDB_SUFFIX) \
	    +incdir+$(COSIM_ROOT)/sv/wb2axip \
	    $(FSDB_COMPILE_FLAGS) \
	    -top tb_top_vcs \
	    $(TB_TOP_SV_SRC) $(COSIM_ROOT)/sv/tb_top_vcs.sv $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o $(SIMV_TB_TOP)
```

Update the phony aliases and run-recipe prerequisites to the variables
(`genamba: $(SIMV_GENAMBA)`, `run-genamba: $(SIMV_GENAMBA)` and recipe body
`$(SIMV_GENAMBA) ...`; same for `tb_top`/`run-tb-top` with `$(SIMV_TB_TOP)`).

- [ ] **Step 4: Verify FSDB=0 dry-run is byte-identical to baseline**

```bash
cd /d/02_NoC/noc_project/cosim/vcs
make -n tb_top  2>&1 | diff /tmp/base_tb_top.txt  - && echo TB_TOP_UNCHANGED
make -n genamba 2>&1 | diff /tmp/base_genamba.txt - && echo GENAMBA_UNCHANGED
```

Expected: `TB_TOP_UNCHANGED`, `GENAMBA_UNCHANGED` (zero diff).

- [ ] **Step 5: Verify FSDB=1 dry-run adds exactly the right pieces**

Each required token checked independently (default `VERDI_HOME` from the
`[WORKSTATION]` block applies — no override needed):

```bash
make -n tb_top FSDB=1 > /tmp/fsdb_tb_top.txt 2>&1
for tok in "+define+FSDB_DUMP" \
           "/tools/verdi_2020.03/share/PLI/VCS/LINUXAMD64/novas.tab" \
           "/tools/verdi_2020.03/share/PLI/VCS/LINUXAMD64/pli.a" \
           "simv_tb_top_fsdb" \
           "csrc_tb_top_fsdb"; do
  grep -qF "$tok" /tmp/fsdb_tb_top.txt && echo "OK  $tok" || echo "MISS $tok"
done
```

Expected: five `OK` lines, zero `MISS`.

```bash
make -n tb_top FSDB=1 VERDI_HOME= 2>&1 | head -2
```

Expected: `*** FSDB=1 requires VERDI_HOME ... Stop.` (guard fires when
VERDI_HOME is explicitly emptied).

- [ ] **Step 6: Commit**

```bash
git add cosim/vcs/Makefile
git commit -m "build(cosim): FSDB=1 mode for VCS — Verdi PLI link, separate _fsdb artifacts"
```

---

### Task 4: `+fsdb=` plusarg + stale-file hygiene in the run recipes

**Files:**
- Modify: `cosim/vcs/Makefile` (`run-genamba`, `run-tb-top` recipes)

- [ ] **Step 1: Baseline of current run recipes**

```bash
cd /d/02_NoC/noc_project/cosim/vcs
make -n run-tb-top  > /tmp/base_run_tb_top.txt  2>&1 || true
make -n run-genamba > /tmp/base_run_genamba.txt 2>&1 || true
```

- [ ] **Step 2: Extend both run recipes**

`run-tb-top` becomes:

```make
run-tb-top: $(SIMV_TB_TOP)
	@mkdir -p output/$(SCENARIO)
	@rm -f output/$(SCENARIO)/tb_top.fsdb
	@set -o pipefail; \
	$(SIMV_TB_TOP) "+scenario=$(SCENARIO_ABS)" $(FSDB_PLUSARG_TB_TOP) \
	    2>&1 | tee output/$(SCENARIO)/run.log
```

`run-genamba` becomes:

```make
run-genamba: $(SIMV_GENAMBA)
	@mkdir -p output/genamba_$(GENAMBA_SCENARIO)
	@rm -f output/genamba_$(GENAMBA_SCENARIO)/tb_genamba.fsdb
	@set -o pipefail; \
	$(SIMV_GENAMBA) "+scenario=$(GENAMBA_SCENARIO_ABS)" $(FSDB_PLUSARG_GENAMBA) \
	    2>&1 | tee output/genamba_$(GENAMBA_SCENARIO)/run.log
```

(The `rm -f` runs in both modes — deleting a nonexistent fsdb in FSDB=0 mode
is a no-op and keeps the recipe single-path; a leftover fsdb from an earlier
FSDB=1 run of the same scenario is exactly the stale evidence we want gone.
Scope of the guarantee: cleanup runs inside the run recipe, i.e. after the
simv build prerequisite — a compile failure aborts earlier and leaves prior
files untouched, which is fine: no new simulation happened.)

- [ ] **Step 3: Verify dry-runs**

FSDB=0 must differ from the Task 4 Step 1 baseline by exactly the intentional
`rm -f` cleanup lines and nothing else:

```bash
make -n run-tb-top  2>&1 | diff /tmp/base_run_tb_top.txt  - | grep "^>"
make -n run-genamba 2>&1 | diff /tmp/base_run_genamba.txt - | grep "^>"
```

Expected: each diff shows exactly one added line — the
`rm -f output/.../...fsdb` — no other `>` lines, no `<` lines, no `+fsdb=`.

Then FSDB=1 (default `VERDI_HOME` applies):

```bash
make -n run-tb-top  FSDB=1 2>&1 | grep -E "\+fsdb=.*output/AX4-BAS-003.*tb_top.fsdb" && echo PLUSARG_OK
make -n run-genamba FSDB=1 2>&1 | grep -E "\+fsdb=.*output/genamba_AX4-BAS-001.*tb_genamba.fsdb" && echo GENAMBA_PLUSARG_OK
```

Expected: `PLUSARG_OK`, `GENAMBA_PLUSARG_OK`.

- [ ] **Step 4: Commit**

```bash
git add cosim/vcs/Makefile
git commit -m "build(cosim): per-pattern +fsdb plusarg and stale-fsdb cleanup in VCS run recipes"
```

---

### Task 5: `run-all-fsdb` batch target

**Files:**
- Modify: `cosim/vcs/Makefile` (append after `run-tb-top`)

- [ ] **Step 1: Add the target**

```make
# --- batch: one fsdb per scenario, both TBs ----------------------------------
# Produces waveforms; NOT a regression gate (always exits 0 — ctest is the
# gate). Failing patterns are reported but do not stop the batch; their
# partial fsdb is kept for debugging. AX4-INF-001 fails by design.
SCENARIOS_ALL := $(notdir $(wildcard $(PROJ_ROOT)/tests/scenarios/AX4-*))

.PHONY: run-all-fsdb
run-all-fsdb:
	@pass=(); fail=(); \
	for s in $(SCENARIOS_ALL); do \
	    echo ""; echo "=== run-all-fsdb: $$s ==="; \
	    if $(MAKE) run-tb-top SCENARIO=$$s FSDB=1; then \
	        pass+=("$$s"); \
	    else \
	        fail+=("$$s"); \
	    fi; \
	done; \
	echo ""; echo "=== run-all-fsdb: tb_genamba ($(GENAMBA_SCENARIO)) ==="; \
	if $(MAKE) run-genamba FSDB=1; then \
	    pass+=("genamba_$(GENAMBA_SCENARIO)"); \
	else \
	    fail+=("genamba_$(GENAMBA_SCENARIO)"); \
	fi; \
	echo ""; echo "================ run-all-fsdb summary ================"; \
	echo "PASS (with fsdb): $${#pass[@]}"; \
	for s in "$${pass[@]}"; do echo "  PASS $$s"; done; \
	echo "FAIL (fsdb kept for debug): $${#fail[@]}"; \
	for s in "$${fail[@]}"; do \
	    case "$$s" in \
	        AX4-INF-*) echo "  FAIL $$s  (fails by design)";; \
	        *)         echo "  FAIL $$s";; \
	    esac; \
	done; \
	echo "--- waveforms ---"; \
	find output -name '*.fsdb' -exec ls -lh {} \; 2>/dev/null | awk '{print "  " $$5 "\t" $$9}'; \
	exit 0
```

- [ ] **Step 2: Verify scenario enumeration**

```bash
cd /d/02_NoC/noc_project/cosim/vcs
make -n run-all-fsdb 2>&1 | grep -oE "AX4-[A-Z]+-[0-9]+[a-z0-9_]*" | sort -u | wc -l
```

Expected: `37`.

- [ ] **Step 3: Verify the loop logic locally with a stub (no VCS needed)**

The inner `$(MAKE) run-tb-top ...` can't run here, but the bash
collect/summary logic can be exercised by overriding MAKE:

```bash
make run-all-fsdb MAKE=true 2>&1 | tail -15
```

(`MAKE=true` makes every inner invocation a no-op success.) Expected: the
summary prints `PASS (with fsdb): 38` (37 scenarios + genamba), `FAIL ...: 0`,
and the target exits 0.

```bash
make run-all-fsdb MAKE=false > /tmp/batch_fail.txt 2>&1; echo "target exit=$?"
grep -c "FAIL AX4" /tmp/batch_fail.txt
grep "fails by design" /tmp/batch_fail.txt
```

(`MAKE=false` forces every inner invocation to fail.) Expected:
`target exit=0` (fault-tolerant even when everything fails), FAIL count 37,
and the `AX4-INF-001` line carries the `(fails by design)` annotation.

- [ ] **Step 4: Commit**

```bash
git add cosim/vcs/Makefile
git commit -m "build(cosim): run-all-fsdb batch target — one fsdb per scenario, fault-tolerant summary"
```

---

### Task 6: Documentation + workstation bring-up checklist

**Files:**
- Modify: `docs/development.md` ("Dual-simulator support (Verilator / VCS)" section, around lines 217-244)

- [ ] **Step 1: Document the FSDB workflow in `docs/development.md`**

Append to the dual-simulator section:

```markdown
#### FSDB waveform dumping (VCS only)

Opt-in per run; default off (regression and ctest are unaffected):

    cd cosim/vcs
    make run-tb-top SCENARIO=AX4-BUR-002_incr_8beat FSDB=1   # -> output/<scenario>/tb_top.fsdb
    make run-genamba FSDB=1                                  # -> output/genamba_<scenario>/tb_genamba.fsdb
    make run-all-fsdb                                        # all 37 scenarios + genamba, summary at end

Requirements: `VERDI_HOME` defaults to `/tools/verdi_2020.03` (the
workstation's install, per `cosim/ref/Makefile`); override it if the layout
differs. FSDB builds produce separate `simv_*_fsdb` binaries beside the
normal ones; toggling `FSDB` never reuses a binary from the other mode.
For memory dumping / interactive Verdi debug, enable the heavier ref-flow
combo: `FSDB_EXTRA="-debug_access+all -debug_all +fsdb+all +vcsd"`.

First-run validation on the workstation (record results in the
`[WORKSTATION]` block of `cosim/vcs/Makefile`):
1. `FSDB_PLI` paths exist (`$VERDI_HOME/share/PLI/VCS/LINUXAMD64/{novas.tab,pli.a}`).
2. Whether `LD_LIBRARY_PATH` needs the FSDB runtime libs.
3. Open one fsdb in Verdi: top-level AXI interfaces, DPI wrapper boundaries,
   and `faxi` checker state must be visible (not merely a loadable file).
```

- [ ] **Step 2: Cross-file consistency check (per CLAUDE.md doc rules)**

```bash
cd /d/02_NoC/noc_project
grep -rln "run-tb-top\|run-genamba\|FSDB" README.md docs/*.md | sort
```

Review each hit: any doc describing the VCS run flow must not contradict the
new FSDB text. Expected files: `docs/development.md` (just edited) AND
`docs/architecture.md` (already references `run-genamba` today — verify its
wording stays accurate; it should not need changes since FSDB is opt-in). If
`README.md` or others mention the VCS flow, list the mismatches for the user
rather than silently editing (CLAUDE.md: surface cross-file diffs).

- [ ] **Step 3: Commit**

```bash
git add docs/development.md
git commit -m "docs(cosim): FSDB waveform dumping workflow and workstation bring-up checklist"
```

- [ ] **Step 4: Workstation acceptance (deferred — needs real VCS+Verdi)**

Quote for the workstation session (not executable on this host):

```bash
cd cosim/vcs
make run-tb-top SCENARIO=AX4-BAS-003_single_write_read_aligned FSDB=1
ls -lh output/AX4-BAS-003_single_write_read_aligned/tb_top.fsdb   # non-empty, fresh mtime
make run-tb-top SCENARIO=AX4-BAS-003_single_write_read_aligned   # FSDB=0 path
ls output/AX4-BAS-003_single_write_read_aligned/*.fsdb            # expect: no fsdb (deleted, not re-dumped)
make run-genamba FSDB=1
make run-all-fsdb
```

Then open one fsdb in Verdi and verify representative signals per the spec's
acceptance section.
