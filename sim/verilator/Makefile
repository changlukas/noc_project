VERILATOR     := verilator
TOP           := tb_top
# bash for `set -o pipefail` in run recipes (dash, a common /bin/sh on
# Linux, lacks it).
SHELL         := bash

# Topology used to generate tb_top_<TOPOLOGY>.sv. Override to switch topologies.
TOPOLOGY ?= mesh_4x4_vc1

# Run class selects the endpoint/checking flavor compiled into the binary
# (user_node_endpoint.sv `ifdef TB_DIRECTED):
#   constrained_random (default) : rand_master WRAP/EXC, RAND_RESP slave, reorder_compare.
#   directed                     : file_master + scoreboard, INCR two-phase.
RUN_CLASS ?= constrained_random
ifeq ($(filter $(RUN_CLASS),directed constrained_random),)
$(error RUN_CLASS must be directed or constrained_random (got '$(RUN_CLASS)'))
endif

# Build environment config and include dirs are shared with sim/vcs/Makefile
# via sim/build_config.mk. tb_top SV sources are in
# sim/filelist_<TOPOLOGY>.f (generated from TB_TOP_SV_SRC in build_config.mk).
# BUILD_ROOT (top-level build/) also comes from build_config.mk; all Verilator
# artifacts land under $(BUILD_ROOT)/verilator/, run logs under ./output/.
include ../build_config.mk

VL_BUILD := $(BUILD_ROOT)/verilator

# Per-topology, per-run-class obj dir (uniform naming for both classes — the
# regression runner keys on it): building one flavor must not clobber another.
OBJ_DIR := $(VL_BUILD)/obj_dir_$(TOPOLOGY)_$(RUN_CLASS)

# FILELIST_F / FILELIST_GEN_ARGS come from build_config.mk. sim/filelist_<TOPOLOGY>.f
# is a generated artifact (gitignored): regenerated below from TB_TOP_SV_SRC with
# absolute paths whenever build_config.mk changes.

# Waveforms are captured via VCS/FSDB; Verilator VCD tracing is not used.

# main.cpp is the Verilator-only clock/eval driver; under VCS the simulator
# owns time and the DPI .cpp is the only C++ source.
C_SRC := \
    $(DPI_C_SRC) \
    $(COSIM_ROOT)/verilator/main.cpp

# c_model is header-only: no -l flag needed; include paths suffice.
# yaml-cpp is a static archive built by CMake FetchContent; link via full path.
VERILATOR_COMMON_FLAGS := \
    --cc --exe \
    --assert \
    -I$(COSIM_ROOT)/tb \
    -I$(SRC_SV) \
    -I$(SPECGEN_SV_INC) \
    -I$(DV_ROOT)/axi-0.39.7/include \
    -CFLAGS "-std=c++17" \
    -CFLAGS "-DYAML_CPP_STATIC_DEFINE" \
    $(foreach i,$(CPP_INCLUDE_FLAGS),-CFLAGS "$(i)") \
    -LDFLAGS "$(YAMLCPP_LIB)" \
    $(STDCXXFS_LDFLAGS) \
    -Wno-fatal \
    -Wno-NULLPORT \
    -Wno-REDEFMACRO \
    -Wno-WIDTHEXPAND \
    -Wno-WIDTHTRUNC \
    -Wno-ASCRANGE \
    -CFLAGS "-Wno-attributes"
# -Wno-attributes (g++): verilator's own svdpi.h triggers "'dllimport'
# attribute ignored" under MinGW for every DPI-bearing translation unit —
# toolchain header noise, not project code.
VERILATOR_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --top-module $(TOP) \
    --timing \
    --output-split 0 \
    --Mdir $(OBJ_DIR) \
    -CFLAGS "-g" \
    -CFLAGS "-O0"
ifeq ($(RUN_CLASS),directed)
VERILATOR_FLAGS += +define+TB_DIRECTED
endif

# Optional strict-mode flag injection for release gates.
# Override with: make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic"
VERILATOR_EXTRA_FLAGS ?=
VERILATOR_FLAGS += $(VERILATOR_EXTRA_FLAGS)

# Linked binary name is platform-dependent: MinGW ld appends .exe, Linux
# does not. Naming the real artifact makes the source-dependency check
# fire correctly on both (a phony or wrong-name target rebuilds always).
EXEEXT := $(if $(findstring NT,$(shell uname -s)),.exe,)
TBTOP_EXE := $(OBJ_DIR)/V$(TOP)$(EXEEXT)

# PE stack-reserve ldflag (see USER_LDFLAGS note at the link recipe below):
# Windows-only, Linux GNU ld rejects --stack.
ifneq ($(EXEEXT),)
STACK_LDFLAGS := -Wl,--stack,67108864
endif

all: $(TBTOP_EXE)

# filelist.f recipe: placed after `all:` so it is not the default goal.
# Use Python3 for path resolution: Verilator under MSYS2 Perl does not accept
# /e/05_... POSIX drive paths in -f filelists (treats them as module names);
# Python3 on Windows returns E:/... native paths that Verilator handles correctly.
# Duplicated (intentionally) in sim/vcs/Makefile so a standalone `cd sim/vcs &&
# make` regenerates it too; gen_filelist.py + FILELIST_GEN_ARGS keep them in sync.
$(FILELIST_F): ../build_config.mk $(COSIM_ROOT)/tools/gen_filelist.py
	$(PYTHON3) $(COSIM_ROOT)/tools/gen_filelist.py $@ $(FILELIST_GEN_ARGS)

# tb_top_<TOPOLOGY>.sv + noc_fabric_<topo>.sv are generated artifacts; regenerate
# from the topology config before Verilating. Depends on the generator script, the
# topology YAML, and a TOPOLOGY stamp. TB_TOP_SV comes from build_config.mk
# (deferred = so TOPOLOGY expands correctly).
TB_TOP_TOPO   := $(COSIM_ROOT)/topologies/$(TOPOLOGY_BASE).yaml
GEN_TB_TOP    := $(COSIM_ROOT)/tools/gen_tb_top.py
TOPO_STAMP    := $(VL_BUILD)/.topology

# tb_top_<TOPOLOGY>.sv does `include "noc_fabric_<topo>.sv"` (bare filename);
# the fabric is emitted into src/sv/, already implied by -I$(SRC_SV) above.
VERILATOR_FLAGS += -I$(dir $(TB_TOP_SV))

# TOPOLOGY stamp records the last-built topology name. The FORCE-driven rule
# rewrites it ONLY when TOPOLOGY changed (content compare), bumping its mtime so
# the tb_top_<TOPOLOGY>.sv prerequisite check re-emits on a topology switch even
# if the YAML itself hasn't changed. Unchanged topology leaves the stamp alone.
.PHONY: FORCE
FORCE:
$(TOPO_STAMP): FORCE
	@mkdir -p $(VL_BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(TOPOLOGY)_$(RUN_CLASS)" ]; then \
	    echo "$(TOPOLOGY)_$(RUN_CLASS)" > $@; \
	fi

$(TB_TOP_SV): $(TB_TOP_TOPO) $(GEN_TB_TOP) $(TOPO_STAMP)
	$(PYTHON3) $(GEN_TB_TOP) --topology $(TOPOLOGY) --out $(TB_TOP_SV)

# Python3 executable — auto-detected so build/run commands stay identical on
# Windows and Linux. Prefer the Windows `py -3` launcher when present (the
# canonical interpreter with PyYAML; the MSYS2 mingw64 `python3` often lacks
# it, which breaks the scenario/perf scripts below). Falls back to `python3`
# on Linux/macOS. Mirrors the root Makefile. Override via local.mk if needed.
PYTHON3 ?= $(if $(shell command -v py 2>/dev/null),py -3,python3)

# yaml-cpp comes from the c_model CMake build; surface a clear hint instead
# of a mysterious linker failure when it has not been built yet.
.PHONY: check-yamlcpp
check-yamlcpp:
	@if [ ! -f "$(YAMLCPP_LIB)" ]; then \
	    echo "ERROR: yaml-cpp static lib missing ($(YAMLCPP_LIB))."; \
	    echo "Run \`make build-cmodel\` at the repo root first."; \
	    exit 1; \
	fi

# Two-phase build: Verilator generates C++ in $(VL_BUILD)/obj_dir, then make
# compiles it.
# PYTHON3 override: verilator_includer needs a real Python3 interpreter.
# USER_LDFLAGS: set PE stack reserve to 64 MiB so MinGW pthread initialises
#   correctly on Windows (avoids STATUS_STACK_OVERFLOW 0xc00000fd).
#   --stack is PE-only; Linux GNU ld rejects it, so gate on EXEEXT (Windows).
# sed pass (--timing): Verilator 5.036 on Windows joins fork-sync done() debug
#   source paths with backslashes in generated C-string literals — rewrite them
#   to forward slashes before g++ sees them.
$(TBTOP_EXE): $(FILELIST_F) $(TB_TOP_SV_SRC) $(C_SRC) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(OBJ_DIR)
	$(VERILATOR) $(VERILATOR_FLAGS) -f $(FILELIST_F) $(C_SRC)
	sed -i -E ':a; s|([A-Za-z]:/[A-Za-z0-9_./]*)[\\]([A-Za-z0-9_])|\1/\2|; ta' \
	    $(OBJ_DIR)/*.cpp $(OBJ_DIR)/*.h
	$(MAKE) -C $(OBJ_DIR) -f V$(TOP).mk \
	    PYTHON3="$(PYTHON3)" \
	    "USER_LDFLAGS=$(STACK_LDFLAGS)"

# Run tb_top with the pulp VIP endpoints. No scenario files: the per-node
# axi_rand_master/slave are driven by +num_reads/+num_writes and the RNG seed.
NUM_READS  ?= 8
NUM_WRITES ?= 8
SEED       ?= 1
RUN_TAG    ?= vip_$(TOPOLOGY)_$(RUN_CLASS)_s$(SEED)

.PHONY: run-tb-top
run-tb-top: $(TBTOP_EXE)
	@mkdir -p output/$(RUN_TAG)
	@echo "running $(RUN_TAG) (reads=$(NUM_READS) writes=$(NUM_WRITES) seed=$(SEED))"
	$(TBTOP_EXE) \
	    "+num_reads=$(NUM_READS)" "+num_writes=$(NUM_WRITES)" \
	    "+verilator+seed+$(SEED)" \
	    "+sam_config=$(TB_TOP_TOPO)" \
	    "+perf_out=output/$(RUN_TAG)/perf.json" \
	    "+perf_scenario=$(RUN_TAG)" \
	    > output/$(RUN_TAG)/run.log 2>&1; \
	rc=$$?; \
	echo "--- run.log (tail) ---"; \
	tail -8 output/$(RUN_TAG)/run.log; \
	$(PYTHON3) perf_cli_summary.py output/$(RUN_TAG)/perf.json || true; \
	exit $$rc

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
	    "+sam_config=$(TB_TOP_TOPO)" \
	    "+perf_out=output/$(CR_TAG)/perf.json" \
	    "+perf_scenario=$(CR_TAG)" \
	    > output/$(CR_TAG)/run.log 2>&1; \
	rc=$$?; \
	echo "--- run.log: per-node monitors + tail ---"; grep -F '[Monitor' output/$(CR_TAG)/run.log; tail -4 output/$(CR_TAG)/run.log; \
	if grep -qE '%Error' output/$(CR_TAG)/run.log; then \
	    echo "CR FAIL: reorder_compare error / assertion"; exit 1; \
	elif [ $$rc -ne 0 ] || ! grep -q 'PASS: all' output/$(CR_TAG)/run.log; then \
	    echo "CR FAIL: run did not reach non-vacuous PASS (rc=$$rc)"; exit 1; \
	else \
	    echo "CR PASS: $(CR_TAG) reorder_compare clean, drained, non-vacuous"; \
	fi

# Smoke-test the Verilator toolchain with a trivial module before attempting
# the full co-sim build.  Expected output: "TOOLCHAIN OK"
hello:
	echo "module empty_tb; initial \$$finish; endmodule" > /tmp/empty_tb.sv
	$(VERILATOR) --cc --exe --build --top-module empty_tb \
	    -CFLAGS "-std=c++17" /tmp/empty_tb.sv \
	    --Mdir /tmp/empty_tb_obj
	/tmp/empty_tb_obj/Vempty_tb && echo "TOOLCHAIN OK"

clean:
	rm -rf $(VL_BUILD)/obj_dir_* output
