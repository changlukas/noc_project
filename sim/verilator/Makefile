VERILATOR     := verilator
TOP           := tb_top
# bash for `set -o pipefail` in run recipes (dash, a common /bin/sh on
# Linux, lacks it).
SHELL         := bash

# Topology used to generate tb_top_<TOPOLOGY>.sv. Override to switch topologies.
TOPOLOGY ?= mesh_4x4_vc1

# Build environment config and include dirs are shared with sim/vcs/Makefile
# via sim/build_config.mk. tb_top SV sources are in
# sim/filelist_<TOPOLOGY>.f (generated from TB_TOP_SV_SRC in build_config.mk).
# BUILD_ROOT (top-level build/) also comes from build_config.mk; all Verilator
# artifacts land under $(BUILD_ROOT)/verilator/, run logs under ./output/.
include ../build_config.mk

VL_BUILD := $(BUILD_ROOT)/verilator

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
    -I$(COSIM_ROOT)/sv \
    -I$(SPECGEN_SV_INC) \
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
    -CFLAGS "-Wno-attributes"
# -Wno-attributes (g++): verilator's own svdpi.h triggers "'dllimport'
# attribute ignored" under MinGW for every DPI-bearing translation unit —
# toolchain header noise, not project code.
VERILATOR_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --top-module $(TOP) \
    --timing \
    --output-split 0 \
    --Mdir $(VL_BUILD)/obj_dir_$(TOPOLOGY) \
    -CFLAGS "-g" \
    -CFLAGS "-O0"

# Optional strict-mode flag injection for release gates.
# Override with: make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic"
VERILATOR_EXTRA_FLAGS ?=
VERILATOR_FLAGS += $(VERILATOR_EXTRA_FLAGS)

# Linked binary name is platform-dependent: MinGW ld appends .exe, Linux
# does not. Naming the real artifact makes the source-dependency check
# fire correctly on both (a phony or wrong-name target rebuilds always).
EXEEXT := $(if $(findstring NT,$(shell uname -s)),.exe,)
TBTOP_EXE := $(VL_BUILD)/obj_dir_$(TOPOLOGY)/V$(TOP)$(EXEEXT)

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

# tb_top_<TOPOLOGY>.sv does `include "noc_fabric_<topo>.sv"` (bare filename) for
# the fabric emitted beside it. Put tb_top's OWN directory on the Verilator
# include path so the beside-it fabric resolves. Both tb_top and fabric land in
# sim/sv/, which is already implied by -I$(COSIM_ROOT)/sv.
VERILATOR_FLAGS += -I$(dir $(TB_TOP_SV))

# TOPOLOGY stamp records the last-built topology name. The FORCE-driven rule
# rewrites it ONLY when TOPOLOGY changed (content compare), bumping its mtime so
# the tb_top_<TOPOLOGY>.sv prerequisite check re-emits on a topology switch even
# if the YAML itself hasn't changed. Unchanged topology leaves the stamp alone.
.PHONY: FORCE
FORCE:
$(TOPO_STAMP): FORCE
	@mkdir -p $(VL_BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(TOPOLOGY)" ]; then \
	    echo "$(TOPOLOGY)" > $@; \
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
# sed pass (--timing): Verilator 5.036 on Windows joins fork-sync done() debug
#   source paths with backslashes in generated C-string literals — rewrite them
#   to forward slashes before g++ sees them.
$(TBTOP_EXE): $(FILELIST_F) $(TB_TOP_SV_SRC) $(C_SRC) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(VL_BUILD)/obj_dir_$(TOPOLOGY)
	$(VERILATOR) $(VERILATOR_FLAGS) -f $(FILELIST_F) $(C_SRC)
	sed -i -E ':a; s|([A-Za-z]:/[A-Za-z0-9_./]*)[\\]([A-Za-z0-9_])|\1/\2|; ta' \
	    $(VL_BUILD)/obj_dir_$(TOPOLOGY)/*.cpp $(VL_BUILD)/obj_dir_$(TOPOLOGY)/*.h
	$(MAKE) -C $(VL_BUILD)/obj_dir_$(TOPOLOGY) -f V$(TOP).mk \
	    PYTHON3="$(PYTHON3)" \
	    "USER_LDFLAGS=-Wl,--stack,67108864"

# Run tb_top with cwd here so the in-project read-dump file lands locally,
# then move it into output/$(SCENARIO)/ alongside run.log. scenario_parser
# resolves data_file: relative to the YAML's own dir so cwd does not matter
# for input resolution.
SCENARIO ?= AX4-BAS-003_single_write_read_aligned
SCENARIO_ABS := $(PROJ_ROOT)/sim/test_patterns/$(SCENARIO)/scenario.yaml

# tb_top requires one +scenario_node<i> plusarg per node (its all-node guard
# $finish(1)s otherwise). Materialize all N neighbor-pattern variants from the
# base scenario.yaml via gen_test_patterns.py --pattern neighbor --topology $(TOPOLOGY),
# then pass every +scenario_node<i>= (built by a shell loop over the emitted node dirs).
# N-node logic mirrors the run_benchmark.py flow used by `make sim`.
GEN_TEST_PATTERNS := $(COSIM_ROOT)/tools/gen_test_patterns.py

.PHONY: run-tb-top
run-tb-top: $(TBTOP_EXE)
	@mkdir -p output/$(SCENARIO)
	@echo "running scenario $(SCENARIO) on $(TOPOLOGY); output -> output/$(SCENARIO)/"
	@$(PYTHON3) $(GEN_TEST_PATTERNS) --from "$(SCENARIO_ABS)" --pattern neighbor \
	    --topology $(TOPOLOGY) --out "output/$(SCENARIO)/coord"
	@scn_args=(); \
	for d in output/$(SCENARIO)/coord/node*/; do \
	    i=$$(basename "$$d" | sed 's/^node//'); \
	    scn_args+=("+scenario_node$$i=$$d""scenario.yaml"); \
	done; \
	$(TBTOP_EXE) "$${scn_args[@]}" \
	    "+perf_out=output/$(SCENARIO)/perf.json" \
	    "+perf_scenario=$(SCENARIO)" \
	    > output/$(SCENARIO)/run.log 2>&1; \
	rc=$$?; \
	if ls master_wrap_read_dump*.txt >/dev/null 2>&1; then \
	    mv master_wrap_read_dump*.txt output/$(SCENARIO)/; \
	fi; \
	echo "--- run.log (tail) ---"; \
	tail -8 output/$(SCENARIO)/run.log; \
	$(PYTHON3) perf_cli_summary.py output/$(SCENARIO)/perf.json || true; \
	exit $$rc

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
	rm -f master_wrap_read_dump*.txt
	rm -f $(VL_BUILD)/master_wrap_read_dump*.txt  # ctest runs Vtb_top with cwd build/verilator
