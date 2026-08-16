VERILATOR     := verilator
TOP           := tb_top
# bash for `set -o pipefail` in run recipes (dash, a common /bin/sh on
# Linux, lacks it).
SHELL         := bash

# Topology used to generate tb_top_<TOPOLOGY>.sv. Override to switch topologies.
TOPOLOGY ?= mesh_4x4_vc1

# Run class selects the endpoint/checking flavor compiled into the binary:
#   directed (only) : file_master + scoreboard, INCR two-phase.
RUN_CLASS ?= directed
ifeq ($(filter $(RUN_CLASS),directed),)
$(error RUN_CLASS must be directed (got '$(RUN_CLASS)'))
endif

# Build environment config and include dirs are shared with sim/vcs/Makefile
# via sim/build_config.mk. tb_top SV sources are in
# sim/filelist_<TOPOLOGY>.f (generated from TB_TOP_SV_SRC in build_config.mk).
# BUILD_ROOT (top-level build/) also comes from build_config.mk; all Verilator
# artifacts land under $(BUILD_ROOT)/verilator/, run logs under ./output/.
include ../build_config.mk

VL_BUILD := $(BUILD_ROOT)/verilator

# Per-topology, per-run-class, per-READ_ROB obj dir: building one flavor must
# not clobber another. READ_ROB is in the name because it changes the emitted
# tb_top, so two READ_ROB values are two different builds of the same topology.
# DMA joins it for the same reason -- it swaps the endpoint and the top.
OBJ_DIR := $(VL_BUILD)/obj_dir_$(TOPOLOGY)_$(RUN_CLASS)_rob$(READ_ROB)$(if $(filter 1,$(DMA)),_dma)

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
    -I$(DV_ROOT)/common_cells-1.37.0/include \
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
    -Wno-ASCRANGE
# --output-split is left at Verilator's default (20000 bytes). It used to be
# forced to 0, which emits the whole design as unsplit .cpp files: one generated
# function reached 12648 lines and g++ 15 died on it with an internal compiler
# error during its own "no-opt dfinit" RTL pass, intermittently and only under
# memory pressure, which reads as a flaky environment rather than a build
# setting. Splitting keeps every translation unit small enough that no compiler
# has to hold a function that size.
VERILATOR_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --top-module $(TOP) \
    --timing \
    --Mdir $(OBJ_DIR) \
    -CFLAGS "-g" \
    -CFLAGS "-O0"

# Optional strict-mode flag injection for release gates.
# Override with: make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic"
VERILATOR_EXTRA_FLAGS ?=
VERILATOR_FLAGS += $(VERILATOR_EXTRA_FLAGS)

# Name the real artifact, not a phony: a wrong-name target rebuilds always.
TBTOP_EXE := $(OBJ_DIR)/V$(TOP)

all: $(TBTOP_EXE)

# filelist.f recipe: placed after `all:` so it is not the default goal.
# gen_filelist.py absolutizes the relative paths build_config.mk carries.
# Duplicated (intentionally) in sim/vcs/Makefile so a standalone `cd sim/vcs &&
# make` regenerates it too; gen_filelist.py + FILELIST_GEN_ARGS keep them in sync.
$(FILELIST_F): ../build_config.mk $(COSIM_ROOT)/tools/gen_filelist.py
	$(PYTHON3) $(COSIM_ROOT)/tools/gen_filelist.py $@ $(FILELIST_GEN_ARGS)

# tb_top_<TOPOLOGY>.sv is a generated artifact; regenerate
# from the topology config before Verilating. Depends on the generator script, the
# topology YAML, and a TOPOLOGY stamp. TB_TOP_SV comes from build_config.mk
# (deferred = so TOPOLOGY expands correctly).
TB_TOP_TOPO   := $(COSIM_ROOT)/topologies/$(TOPOLOGY).yaml
GEN_TB_TOP    := $(COSIM_ROOT)/tools/gen_tb_top.py
TOPO_STAMP    := $(VL_BUILD)/.topology

# tb_top_<TOPOLOGY>.sv does `include "noc_fabric.sv"` (bare filename); the
# fabric lives in ref_model/top/, already implied by -I$(SRC_SV) above.
VERILATOR_FLAGS += -I$(dir $(TB_TOP_SV))

# TOPOLOGY stamp records the last-built topology name. The FORCE-driven rule
# rewrites it ONLY when TOPOLOGY changed (content compare), bumping its mtime so
# the tb_top_<TOPOLOGY>.sv prerequisite check re-emits on a topology switch even
# if the YAML itself hasn't changed. READ_ROB joins it because it also changes
# what the generator emits. Unchanged stamp leaves the file alone.
.PHONY: FORCE
FORCE:
$(TOPO_STAMP): FORCE
	@mkdir -p $(VL_BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(TOPOLOGY)_$(RUN_CLASS)_rob$(READ_ROB)$(DMA)$(if $(filter 1,$(DMA)),_j$(DMA_JOBS_PER_NODE)_l$(DMA_LENGTH)_rw$(DMA_RW))" ]; then \
	    echo "$(TOPOLOGY)_$(RUN_CLASS)_rob$(READ_ROB)$(DMA)$(if $(filter 1,$(DMA)),_j$(DMA_JOBS_PER_NODE)_l$(DMA_LENGTH)_rw$(DMA_RW))" > $@; \
	fi

# DMA_JOBS_PER_NODE / DMA_LENGTH / _DMA_JOB_ARGS come from build_config.mk, so
# the top's compare and the job files it compares cannot be built from different
# geometries.
GEN_DMA_JOBS := $(COSIM_ROOT)/tools/gen_dma_jobs.py

$(TB_TOP_SV): $(TB_TOP_TOPO) $(GEN_TB_TOP) $(TOPO_STAMP) $(SPECGEN_SV_INC)/ni_params_pkg.sv
	$(PYTHON3) $(GEN_TB_TOP) --topology $(TOPOLOGY) --read-rob $(READ_ROB) \
	    $(if $(filter 1,$(DMA)),--dma $(_DMA_JOB_ARGS)) --out $(TB_TOP_SV)

# topology_<geometry>_pkg.sv (build_config.mk's TOPOLOGY_PKG_SV): the
# per-geometry address-map package tb_top's own TILE_BASE_ADDR / TILE_SIZE /
# NOC_EGRESS_BASE / peripheral table are relocated from. No TOPO_STAMP, unlike
# TB_TOP_SV above: the package doesn't vary with RUN_CLASS/READ_ROB/DMA, only
# with the address map the topology YAML declares, so it rebuilds only when
# that YAML or the generator changes.
$(TOPOLOGY_PKG_SV): $(TB_TOP_TOPO) $(GEN_TB_TOP)
	$(PYTHON3) $(GEN_TB_TOP) --topology $(TOPOLOGY) --emit-topology-pkg --out $(TOPOLOGY_PKG_SV)

# Mirrors the root Makefile. Override via local.mk if needed.
PYTHON3 ?= python3

# yaml-cpp comes from the c_model CMake build; surface a clear hint instead
# of a mysterious linker failure when it has not been built yet.
.PHONY: check-yamlcpp
check-yamlcpp:
	@if [ ! -f "$(YAMLCPP_LIB)" ]; then \
	    echo "ERROR: yaml-cpp static lib missing ($(YAMLCPP_LIB))."; \
	    echo "Run \`make build-verilator\` at the repo root first (builds only the"; \
	    echo "yaml-cpp target, not the full c_model test suite)."; \
	    exit 1; \
	fi

# Parallelism for the generated C++ build only, not for this makefile. Four and
# not $(nproc): the g++ 15 ICE documented above is memory pressure, and six
# concurrent cc1plus with their PCHs exhausted a 15 GB host mid-build. Measured
# on mesh_4x4_vc1_periph4 with a warm ccache, cold obj_dir: 18 s serial, 5 s at
# -j4, against 14 s of Verilator elaboration that is single-threaded either way.
BUILD_JOBS ?= 4

# Two-phase build: Verilator generates C++ in $(VL_BUILD)/obj_dir, then make
# compiles it.
# PYTHON3 override: verilator_includer needs a real Python3 interpreter.
$(TBTOP_EXE): $(FILELIST_F) $(TB_TOP_SV_SRC) $(NOC_FABRIC_SV) $(C_SRC) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(OBJ_DIR)
	$(VERILATOR) $(VERILATOR_FLAGS) -f $(FILELIST_F) $(C_SRC)
	$(if $(ELABORATE_ONLY),echo "ELABORATE_ONLY: elaborated - C++ build skipped",$(MAKE) -j $(BUILD_JOBS) -C $(OBJ_DIR) -f V$(TOP).mk PYTHON3="$(PYTHON3)")

# Elaboration alone: Verilator reads the SV and emits its C++, and the g++ build
# of that C++ is skipped. Enough to prove a top elaborates, an order of magnitude
# cheaper, and the only goal a top with no run path yet can serve.
.PHONY: elaborate
elaborate: ELABORATE_ONLY := 1
elaborate: $(TBTOP_EXE)

# The whole build, run excluded: Verilator plus the g++ of what it emitted.
.PHONY: build
build: $(TBTOP_EXE)

# NO_BUILD=1 makes run-directed a run and nothing else, so it cannot silently
# spend a build's worth of time. It reports the missing binary instead.
.PHONY: check-built
check-built:
	@[ -x "$(TBTOP_EXE)" ] || { \
	    echo "ERROR: $(TOPOLOGY) is not built. Run: make sim-build TB=$(TOPOLOGY)"; exit 1; }

SEED       ?= 1

# One directed run recipe, three injection modes.
#   INJECTION_MODE=0 (default): two phases, writes drain before reads, scoreboard gates.
#   INJECTION_MODE=1: one phase, reads and writes interleave, paced per cycle by
#     INJECTION_RATE. The scoreboard's write-before-read precondition fails, so the
#     testbench disarms it and the axi_bw_monitor gates instead.
#   INJECTION_MODE=2: one phase, writes paced as mode 1, each read issues after its
#     paired write's B response, scoreboard gates as mode 0 (checked-continuous).
# INJECTION_COUNT bounds the run. Past saturation the queues never empty, and
# wait_b/wait_r would not terminate without it (booksim solves the same problem
# with sim_type=throughput, which skips its drain).
PATTERN         ?= neighbor
# Multicast mask shape (multicast pattern only): row | col | submesh. One shape
# per run -- concurrent multicast trees must stay pairwise disjoint (R1).
MCAST_SHAPE     ?= row
INJECTION_MODE  ?= 0
INJECTION_RATE  ?= 1.0
# Directed default is 64: measured on mesh_4x4_vc4 uniform_random at 4 ids,
# the RoB high-water mark rises 3 -> 12 -> 60 (of 128) across counts 4, 16, 64
# and then saturates at 75 by 128, so 64 is the largest count where injection
# still sets the load rather than the fabric. At 4 the RoB sat at 2% and no
# depth was anywhere near binding.
INJECTION_COUNT ?= $(if $(filter 1 2,$(INJECTION_MODE)),200,64)
HOTSPOT         ?= 5

# Distinct AXI ids one initiator draws from (gen_test_patterns
# --ids-per-initiator). Unset leaves the generator default (1). >1 exercises
# cross-id B reordering (per-id interlock).
IDS_PER_INITIATOR ?=

# NSU MetaBuffer depths. No ?= default: unset leaves the plusarg unpassed (see
# the $(if) guards below) and the tb falls back to its
# ni_params_pkg::NSU_META_BUFFER_*_DFLT-initialised local.

# NMU RoB pool depths, per direction. Both <= 256 (ordering_tag is 8 bits). No ?=
# default: unset leaves the plusarg unpassed (see the $(if) guards below) and
# the tb falls back to its ni_params_pkg::NMU_ROB_*_DFLT-initialised local.

# AXI burst length passed to gen_test_patterns (--len). 0 = single beat.
# At --size 5 (32 B/beat), 63 gives 64 beats = 2048 B, inside the 4 KB boundary.
BURST_LEN       ?= 0

# Per-AXI-ID order-list depth (FlooNoC MaxRoTxnsPerId, floo_rob.sv:12). No ?=
# default; see B_ROB_DEPTH note above.

# _CONTINUOUS is non-empty exactly when INJECTION_MODE is 1. Kept on one line:
# a backslash continuation inside a `?=` value folds the leading whitespace of
# the next line into the value.
_CONTINUOUS    := $(filter 1,$(INJECTION_MODE))
_CHECKED       := $(filter 2,$(INJECTION_MODE))
# Pattern tag: multicast runs are per-shape, so the shape joins the stim dir /
# output tag (row and submesh runs must not share stimulus or logs).
_PATTERN_TAG   := $(PATTERN)$(if $(filter multicast,$(PATTERN)),_$(MCAST_SHAPE))
# DMA=1 replaces the pattern stimulus with iDMA job files, so its stimulus and
# its run tag key on the job geometry rather than on PATTERN / INJECTION_COUNT,
# neither of which a DMA run reads.
#
# Stimulus is plain text and sits in the tree, beside the run logs it pairs with
# (test_patterns/<tag>/ next to output/<tag>/). Only linked artifacts have to
# live under $(BUILD_ROOT): the Linux linker rejects a yaml-cpp built on a /mnt
# mount, which is a constraint on object files and binaries, not on text.
STIM_BASE      ?= test_patterns
# Stimulus keys on the GEOMETRY, not on the topology file name: gen_test_patterns
# reads x_dim, y_dim and address_map and never reads num_vc, so mesh_4x4_vc1,
# _vc2, _vc4 and _vc8 emit byte-identical files. Stripping _vc<N> collapses those
# four onto one directory instead of four copies under four names. VC count is a
# router parameter -- it changes buffering and arbitration, not who sends what.
_GEOMETRY      := $(shell echo $(TOPOLOGY) | sed 's/_vc[0-9]*//')
# The leaf is the pattern exactly as PATTERN= spells it, so `ls` over a geometry
# IS the pattern list. INJECTION_COUNT is deliberately not in the path: it is a
# property of the batch, not of one pattern, and the recipe below regenerates
# unconditionally before every run, so a differing count overwrites rather than
# going stale. Set STIM_BASE to hold two batches side by side.
ifeq ($(DMA),1)
STIM_ROOT      ?= $(STIM_BASE)/$(_GEOMETRY)/dma_j$(DMA_JOBS_PER_NODE)_l$(DMA_LENGTH)_rw$(DMA_RW)
else
STIM_ROOT      ?= $(STIM_BASE)/$(_GEOMETRY)/$(_PATTERN_TAG)
endif
# READ_ROB joins the run tag, not STIM_ROOT: it changes what the NI does with a
# response, not the stimulus, so two READ_ROB values share generated patterns and
# must not share output/<tag>/ (run.log, result.csv, perf.json all live there).
_TOPO_TAG      := $(TOPOLOGY)_rob$(READ_ROB)
ifeq ($(DMA),1)
SIM_TAG        ?= dma_$(_TOPO_TAG)_j$(DMA_JOBS_PER_NODE)_l$(DMA_LENGTH)_rw$(DMA_RW)
else
SIM_TAG        ?= $(if $(_CONTINUOUS),continuous_$(_TOPO_TAG)_$(_PATTERN_TAG)_r$(INJECTION_RATE)_s$(SEED),$(if $(_CHECKED),checked_$(_TOPO_TAG)_$(_PATTERN_TAG)_r$(INJECTION_RATE)_s$(SEED),directed_$(_TOPO_TAG)_$(_PATTERN_TAG)_s$(SEED)))
endif
GEN_PATTERNS   := $(COSIM_ROOT)/tools/gen_test_patterns.py
EMIT_CSV       := $(COSIM_ROOT)/tools/emit_result_csv.py
# HOTSPOT_PERIPHERALS=1 moves the hotspot set off the router array (every tile
# targets the peripheral on its row), so HOTSPOT names nothing and is not passed.
_HOTSPOT_ARGS  := $(if $(filter hotspot,$(PATTERN)),$(if $(HOTSPOT_PERIPHERALS),--hotspot-peripherals,--hotspot $(HOTSPOT)),)
_MCAST_ARGS    := $(if $(filter multicast,$(PATTERN)),--mcast-shape $(MCAST_SHAPE),)

.PHONY: gen-directed
gen-directed:
	@mkdir -p $(STIM_ROOT)
	$(if $(filter 1,$(DMA)),,$(PYTHON3) $(GEN_PATTERNS) --topology $(TOPOLOGY) \
	    --out $(STIM_ROOT) --pattern $(PATTERN) \
	    --transactions-per-node $(INJECTION_COUNT) --size 5 --len $(BURST_LEN) \
	    --seed $(SEED) $(_HOTSPOT_ARGS) $(_MCAST_ARGS) \
	    $(if $(IDS_PER_INITIATOR),--ids-per-initiator $(IDS_PER_INITIATOR)))
	$(if $(filter 1,$(DMA)),$(PYTHON3) $(GEN_DMA_JOBS) --topology $(TOPOLOGY) \
	    --out $(STIM_ROOT) $(_DMA_JOB_ARGS))
	@echo "stimulus: $(STIM_ROOT)"

# Parse-time, unlike the RUN_CLASS guard at :288 which must be a recipe check:
# nothing target-overrides STIM_ROOT, so its value is final here, and firing
# before the first prerequisite is what keeps a missing stimulus tree from
# costing a build first.
ifneq ($(filter run-directed,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(STIM_ROOT)),)
$(error no stimulus at $(STIM_ROOT). Run: make sim-gen TB=$(TOPOLOGY) PATTERN=$(PATTERN))
endif
endif

.PHONY: run-directed
run-directed: $(if $(NO_BUILD),check-built,$(TBTOP_EXE))
	# Guard runs first in the recipe (before emit/run), so a wrong RUN_CLASS exits
	# before a vacuous no-scoreboard run. Deliberately a recipe check, not a
	# parse-time $(error): OBJ_DIR/TBTOP_EXE are := fixed at parse, so a target-
	# specific override cannot re-key them; the cost is a wasted wrong-flavor build.
	@[ "$(RUN_CLASS)" = "directed" ] || { echo "ERROR: run-directed requires RUN_CLASS=directed (got '$(RUN_CLASS)')"; exit 1; }
	@mkdir -p output/$(SIM_TAG)
	@echo "running $(SIM_TAG) (mode=$(INJECTION_MODE) rate=$(INJECTION_RATE) count=$(INJECTION_COUNT) pattern=$(PATTERN))"
	$(TBTOP_EXE) \
	    "+stim_dir=$(STIM_ROOT)" \
	    "+num_reads=$(INJECTION_COUNT)" "+num_writes=$(INJECTION_COUNT)" \
	    "+injection_mode=$(INJECTION_MODE)" "+injection_rate=$(INJECTION_RATE)" \
	    $(if $(MAX_UNIQUE_IDS),"+max_unique_ids=$(MAX_UNIQUE_IDS)") \
	    $(if $(MAX_OUTSTANDING),"+max_outstanding=$(MAX_OUTSTANDING)") \
	    $(if $(B_ROB_DEPTH),"+b_rob_depth=$(B_ROB_DEPTH)") \
	    $(if $(R_ROB_DEPTH),"+r_rob_depth=$(R_ROB_DEPTH)") \
	    $(if $(MAX_TXNS_PER_ID),"+max_txns_per_id=$(MAX_TXNS_PER_ID)") \
	    $(if $(MCAST_FAULT),"+mcast_fault=$(MCAST_FAULT)") \
	    $(if $(TIMEOUT_CYCLES),"+timeout_cycles=$(TIMEOUT_CYCLES)") \
	    $(if $(DECERR_FAULT),"+decerr_fault=$(DECERR_FAULT)") \
	    $(if $(DECERR_FAULT_WR),"+decerr_fault_wr=$(DECERR_FAULT_WR)") \
	    "+verilator+seed+$(SEED)" \
	    "+sam_config=$(TB_TOP_TOPO)" \
	    "+perf_out=output/$(SIM_TAG)/perf.json" \
	    "+perf_scenario=$(SIM_TAG)" \
	    > output/$(SIM_TAG)/run.log 2>&1; \
	rc=$$?; \
	echo "--- run.log: per-node monitors + tail ---"; grep -F '[Monitor' output/$(SIM_TAG)/run.log; grep -vF '[HWM]' output/$(SIM_TAG)/run.log | tail -4; \
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
	        $(if $(MAX_UNIQUE_IDS),--max-unique-ids $(MAX_UNIQUE_IDS)) \
	        $(if $(MAX_OUTSTANDING),--max-outstanding $(MAX_OUTSTANDING)); \
	    echo "CONTINUOUS PASS: $(SIM_TAG)"; \
	elif [ $$rc -ne 0 ] || ! grep -q 'PASS: all' output/$(SIM_TAG)/run.log; then \
	    echo "DIRECTED FAIL: run did not reach non-vacuous PASS (rc=$$rc)"; exit 1; \
	elif [ "$(DMA)" = "1" ]; then \
	    echo "DMA PASS: $(SIM_TAG) every job retired, every region intact"; \
	else \
	    echo "$(if $(_CHECKED),CHECKED,DIRECTED) PASS: $(SIM_TAG) scoreboard clean, non-vacuous"; \
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
	# Generated stimulus goes; the tracked example stays. `clean` means "leave what
	# git tracks", and -fdx is exactly that predicate -- an rm -rf here would delete
	# the committed example, and leaving the directory alone is what put this
	# Makefile in violation of the root's clean contract to begin with.
	@git clean -fdxq $(STIM_BASE) 2>/dev/null || rm -rf $(STIM_BASE)
