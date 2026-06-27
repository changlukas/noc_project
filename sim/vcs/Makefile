# VCS build for the cosim testbenches. Source lists are shared with the
# Verilator flow via sim/build_config.mk; everything VCS-specific lives here.
# Topology used to generate tb_top_<TOPOLOGY>.sv. Override to switch topologies.
TOPOLOGY ?= mesh_4x4_vc1
# Build artifacts land under $(BUILD_ROOT)/vcs/ (top-level build/), run logs
# under ./output/.
#
# NOT runnable on the Windows host (VCS is Linux-only) — written for the
# workstation. Adjust the [WORKSTATION] block below to the local install
# (module load, license, version) before first use.
#
# Differences from the Verilator flow, by design:
# - No C++ main driver: VCS owns simulation time. tb_top is self-clocked; no
#   wrapper module is needed.
# - None of the Verilator workarounds are needed (--output-split,
#   backslash-path sed, EXEEXT).

# bash for `set -o pipefail` in run recipes.
SHELL := bash

# --- [WORKSTATION] adjust to local environment -------------------------------
VCS      ?= vcs
# Typical extras you may need here:
#   VCS_EXTRA += -licqueue
#   CC/CXX overrides if the site default gcc is too old for -std=c++17
# NOTE: -Mdir (incremental-compile dir, redirects csrc/) flag spelling can
# vary by VCS version — check `vcs -help` on the workstation.
VCS_EXTRA ?=
# FSDB dumping (FSDB=1): Verdi install + PLI registration for $fsdbDump*.
# Defaults match the workstation's local reference Makefile (untracked,
# sim/ref/; Verdi 2020.03,
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
# -----------------------------------------------------------------------------

include ../build_config.mk

VCS_BUILD := $(BUILD_ROOT)/vcs

# FILELIST_F / FILELIST_GEN_ARGS come from build_config.mk. sim/filelist_<TOPOLOGY>.f
# is a generated artifact (gitignored): VCS regenerates it itself (recipe below) so
# a standalone `cd sim/vcs && make` works on a fresh clone where the .f is absent.

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
# Deferred (=) on purpose: SCENARIO is defined later in this file, and the
# per-run value must win when run-all-fsdb loops.
# Use sites attach via $(if $(V), $(V),) — a bare " $(V)" would leave a
# double space when empty and break FSDB=0 command-line byte-identity.
ifeq ($(FSDB),1)
FSDB_PLUSARG_TB_TOP  = "+fsdb=$(CURDIR)/output/$(SCENARIO)/tb_top.fsdb"
else
FSDB_PLUSARG_TB_TOP  =
endif
# -----------------------------------------------------------------------------

# yaml-cpp comes from the c_model CMake build; surface a clear hint instead
# of a mysterious link failure when it has not been built yet.
.PHONY: check-yamlcpp
check-yamlcpp:
	@if [ ! -f "$(YAMLCPP_LIB)" ]; then \
	    echo "ERROR: yaml-cpp static lib missing ($(YAMLCPP_LIB))."; \
	    echo "Run \`make build-cmodel\` at the repo root first."; \
	    exit 1; \
	fi

# filelist.f recipe: placed after check-yamlcpp so it is not the default goal.
# Duplicated from sim/verilator/Makefile (intentionally) so a standalone
# `cd sim/vcs && make` regenerates filelist.f on a fresh clone where it is
# absent; gen_filelist.py + FILELIST_GEN_ARGS keep the two recipes in sync.
$(FILELIST_F): ../build_config.mk $(COSIM_ROOT)/tools/gen_filelist.py
	$(PYTHON3) $(COSIM_ROOT)/tools/gen_filelist.py $@ $(FILELIST_GEN_ARGS)

# tb_top_<TOPOLOGY>.sv + noc_fabric_<topo>.sv are generated artifacts; regenerate
# from the topology config before building. TB_TOP_SV comes from build_config.mk
# (deferred = so TOPOLOGY expands correctly).
TB_TOP_TOPO   := $(COSIM_ROOT)/topologies/$(TOPOLOGY).yaml
GEN_TB_TOP    := $(COSIM_ROOT)/tools/gen_tb_top.py
TOPO_STAMP    := $(VCS_BUILD)/.topology

# TOPOLOGY stamp records the last-built topology. The FORCE-driven rule rewrites
# the stamp ONLY when TOPOLOGY changed (content compare), bumping its mtime so the
# tb_top_<TOPOLOGY>.sv prerequisite check re-emits on a topology switch. Mirrors
# the Verilator Makefile's mechanism exactly. Unchanged topology leaves the stamp
# alone.
.PHONY: FORCE
FORCE:
$(TOPO_STAMP): FORCE
	@mkdir -p $(VCS_BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(TOPOLOGY)" ]; then \
	    echo "$(TOPOLOGY)" > $@; \
	fi

$(TB_TOP_SV): $(TB_TOP_TOPO) $(GEN_TB_TOP) $(TOPO_STAMP)
	$(PYTHON3) $(GEN_TB_TOP) --topology $(TOPOLOGY) --out $(TB_TOP_SV)

# DPI C++ is compiled and linked into simv by VCS itself; pass include dirs
# through -CFLAGS and the yaml-cpp static archive directly on the link line.
VCS_COMMON_FLAGS := \
    -full64 -sverilog \
    -timescale=1ns/1ps \
    -assert svaext \
    +incdir+$(COSIM_ROOT)/sv \
    +incdir+$(SPECGEN_SV_INC) \
    -CFLAGS "-std=c++17" \
    -CFLAGS "-DYAML_CPP_STATIC_DEFINE" \
    $(foreach i,$(CPP_INCLUDE_FLAGS),-CFLAGS "$(i)") \
    $(STDCXXFS_LDFLAGS) \
    $(VCS_EXTRA)
ifeq ($(FSDB),1)
# guarded: += of an empty var would add a trailing space and break FSDB=0 byte-identity
VCS_COMMON_FLAGS += $(FSDB_COMPILE_FLAGS)
endif

# --- tb_top cosim (self-clocked) ---
SCENARIO ?= AX4-BAS-003_single_write_read_aligned
SCENARIO_ABS := $(PROJ_ROOT)/sim/test_patterns/$(SCENARIO)/scenario.yaml

# tb_top requires one +scenario_node<i> plusarg per node (its all-node guard
# $finish(1)s otherwise). Materialize all N neighbor-pattern variants from the
# base scenario.yaml via gen_test_patterns.py --pattern neighbor --topology $(TOPOLOGY),
# then pass every +scenario_node<i>= (built by a shell loop over the emitted node
# dirs). Mirrors sim/verilator/Makefile run-tb-top. PYTHON3 must have
# PyYAML — on a host where an EDA tool shadows a broken python3 on PATH, pin it
# in local.mk:  PYTHON3 := /usr/bin/python3.12
PYTHON3           ?= $(if $(shell command -v py 2>/dev/null),py -3,python3)
GEN_TEST_PATTERNS := $(COSIM_ROOT)/tools/gen_test_patterns.py

# Per-topology simv + csrc dir (mirrors the Verilator obj_dir_$(TOPOLOGY)
# isolation): building one topology must not clobber another's binary.
SIMV_TB_TOP := $(VCS_BUILD)/simv_tb_top_$(TOPOLOGY)$(FSDB_SUFFIX)

$(SIMV_TB_TOP): $(FILELIST_F) $(TB_TOP_SV_SRC) $(DPI_C_SRC) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(VCS_BUILD)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    -Mdir=$(VCS_BUILD)/csrc_tb_top_$(TOPOLOGY)$(FSDB_SUFFIX) \
	    -top tb_top \
	    -f $(FILELIST_F) $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o $(SIMV_TB_TOP)

.PHONY: tb_top run-tb-top
tb_top: $(SIMV_TB_TOP)

run-tb-top: $(SIMV_TB_TOP)
	@mkdir -p output/$(SCENARIO)
	@rm -f output/$(SCENARIO)/tb_top.fsdb  # drop stale fsdb from a previous FSDB=1 run
	@$(PYTHON3) $(GEN_TEST_PATTERNS) --from "$(SCENARIO_ABS)" --pattern neighbor \
	    --topology $(TOPOLOGY) --out "output/$(SCENARIO)/coord"
	@set -o pipefail; \
	scn_args=(); \
	for d in output/$(SCENARIO)/coord/node*/; do \
	    i=$$(basename "$$d" | sed 's/^node//'); \
	    scn_args+=("+scenario_node$$i=$$d""scenario.yaml"); \
	done; \
	$(SIMV_TB_TOP) \
	    "$${scn_args[@]}" \
	    "+perf_out=output/$(SCENARIO)/perf.json" \
	    "+perf_scenario=$(SCENARIO)"$(if $(FSDB_PLUSARG_TB_TOP), $(FSDB_PLUSARG_TB_TOP),) \
	    2>&1 | tee output/$(SCENARIO)/run.log
	@$(PYTHON3) $(COSIM_ROOT)/verilator/perf_cli_summary.py output/$(SCENARIO)/perf.json || true

# --- batch: one fsdb per scenario, both TBs ----------------------------------
# Produces waveforms; NOT a regression gate (always exits 0 — ctest is the
# gate). The FSDB tb_top binary is built once up front — if that build fails
# the batch stops before running any scenario. A scenario counts PASS only if
# the run succeeds AND its fsdb exists non-empty; failing patterns are
# reported but do not stop the batch; their partial fsdb is kept for
# debugging. AX4-INF-001 fails by design.
SCENARIOS_ALL := $(notdir $(wildcard $(PROJ_ROOT)/sim/test_patterns/AX4-*))

.PHONY: run-all-fsdb
run-all-fsdb:
	@if ! $(MAKE) tb_top FSDB=1; then \
	    echo ""; echo "run-all-fsdb: FSDB build failed — no scenarios were run."; \
	    exit 0; \
	fi; \
	pass=(); fail=(); fsdbs=(); \
	for s in $(SCENARIOS_ALL); do \
	    echo ""; echo "=== run-all-fsdb: $$s ==="; \
	    if $(MAKE) run-tb-top SCENARIO=$$s FSDB=1 && test -s "output/$$s/tb_top.fsdb"; then \
	        pass+=("$$s"); fsdbs+=("output/$$s/tb_top.fsdb"); \
	    else \
	        fail+=("$$s"); \
	    fi; \
	done; \
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
	if [ $${#fsdbs[@]} -gt 0 ]; then \
	    ls -lh "$${fsdbs[@]}" | awk '{print "  " $$5 "\t" $$9}'; \
	else \
	    echo "  (none)"; \
	fi; \
	exit 0

.PHONY: clean
clean:
	rm -rf $(VCS_BUILD) output csrc *.log ucli.key vc_hdrs.h
	rm -rf verdiLog novas.* master_wrap_read_dump*.txt  # Verdi/FSDB cwd droppings + c_model read dumps
