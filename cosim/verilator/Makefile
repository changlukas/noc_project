VERILATOR     := verilator
TOP           := tb_top
# bash for `set -o pipefail` in run recipes (dash, a common /bin/sh on
# Linux, lacks it).
SHELL         := bash

# Source lists, include dirs, and +define+s are simulator-neutral and shared
# with cosim/vcs/Makefile via cosim/sources.mk. BUILD_ROOT (top-level build/)
# also comes from sources.mk; all Verilator artifacts land under
# $(BUILD_ROOT)/verilator/, run logs under ./output/.
include ../sources.mk

VL_BUILD := $(BUILD_ROOT)/verilator

# --- VCD waveform tracing (opt-in) -------------------------------------------
# TRACE=1 verilates with --trace into separate obj dirs (obj_dir_trace /
# obj_genamba_trace) because --trace changes the generated C++ — sharing an
# obj dir across modes would silently reuse the wrong build. The dump code in
# main.cpp / main_genamba.cpp is guarded by #if VM_TRACE (defined by
# Verilator iff --trace), so non-trace builds contain zero trace code.
TRACE ?= 0
ifeq ($(TRACE),1)
TRACE_FLAGS := --trace --trace-structs
TRACE_SUFFIX := _trace
else
TRACE_FLAGS :=
TRACE_SUFFIX :=
endif
# Deferred (=) on purpose: SCENARIO / GENAMBA_SCENARIO are defined later in
# this file, and the per-run value must win when run-all-trace loops.
# Use sites attach via $(if $(V), $(V),) — a bare " $(V)" would leave a
# double space when empty and break TRACE=0 command-line byte-identity.
ifeq ($(TRACE),1)
TRACE_PLUSARG_TB_TOP  = "+vcd=$(CURDIR)/output/$(SCENARIO)/tb_top.vcd"
TRACE_PLUSARG_GENAMBA = "+vcd=$(CURDIR)/output/genamba_$(GENAMBA_RUN_TAG)/tb_genamba.vcd"
else
TRACE_PLUSARG_TB_TOP  =
TRACE_PLUSARG_GENAMBA =
endif
# -----------------------------------------------------------------------------

SV_SRC := $(TB_TOP_SV_SRC)

# main.cpp is the Verilator-only clock/eval driver; under VCS the simulator
# owns time and the DPI .cpp is the only C++ source.
C_SRC := \
    $(DPI_C_SRC) \
    $(COSIM_ROOT)/verilator/main.cpp

# c_model is header-only: no -l flag needed; include paths suffice.
# yaml-cpp is a static archive built by CMake FetchContent; link via full path.
# Flags shared by every Verilator target in this Makefile (tb_top, genamba).
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
    $(COMMON_DEFINES) \
    -Wno-fatal \
    -Wno-NULLPORT \
    -Wno-REDEFMACRO \
    -Wno-WIDTHEXPAND \
    -Wno-WIDTHTRUNC \
    -CFLAGS "-Wno-attributes"
# -Wno-attributes (g++): verilator's own svdpi.h triggers "'dllimport'
# attribute ignored" under MinGW for every DPI-bearing translation unit —
# toolchain header noise, not project code.
ifeq ($(TRACE),1)
# guarded: += of an empty var would add a trailing space and break TRACE=0
# byte-identity
VERILATOR_COMMON_FLAGS += $(TRACE_FLAGS)
endif

VERILATOR_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --top-module $(TOP) \
    --no-timing \
    --Mdir $(VL_BUILD)/obj_dir$(TRACE_SUFFIX) \
    -I$(COSIM_ROOT)/sv/wb2axip \
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
TBTOP_EXE := $(VL_BUILD)/obj_dir$(TRACE_SUFFIX)/V$(TOP)$(EXEEXT)

all: $(TBTOP_EXE)

# Python3 executable.  On Windows with MSYS2 use `py -3`; on Linux/macOS
# `python3` is standard.  Override at the command line if needed:
#   make build-verilator PYTHON3=py
PYTHON3 ?= python3

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
# LDFLAGS_EXTRA: set PE stack reserve to 64 MiB so MinGW pthread initialises
#   correctly on Windows (avoids STATUS_STACK_OVERFLOW 0xc00000fd).
$(TBTOP_EXE): $(SV_SRC) $(C_SRC) $(TB_TOP_INC_DEPS) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(VL_BUILD)/obj_dir$(TRACE_SUFFIX)
	$(VERILATOR) $(VERILATOR_FLAGS) $(SV_SRC) $(C_SRC)
	$(MAKE) -C $(VL_BUILD)/obj_dir$(TRACE_SUFFIX) -f V$(TOP).mk \
	    PYTHON3="$(PYTHON3)" \
	    "USER_LDFLAGS=-Wl,--stack,67108864"

# Run tb_top with cwd here so the in-project read-dump file lands locally,
# then move it into output/$(SCENARIO)/ alongside run.log. scenario_parser
# resolves data_file: relative to the YAML's own dir so cwd does not matter
# for input resolution.
SCENARIO ?= AX4-BAS-003_single_write_read_aligned
SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(SCENARIO)/scenario.yaml

# tb_top is now bidirectional: it needs two coordinate-variant plusargs
# (node0 = identity / low addr, node1 = +0x1_0000_0000 / coordinate (1,0)).
# Generate the variants for $(SCENARIO) into output/$(SCENARIO)/coord/ then
# pass +scenario_node0/+scenario_node1. The authoritative ctest path
# (test_cosim_integration) builds its own command and does not use this target.
COORD_GEN := $(COSIM_ROOT)/tools/gen_coordinate_scenarios.py

.PHONY: run-tb-top
run-tb-top: $(TBTOP_EXE)
	@mkdir -p output/$(SCENARIO)
	@rm -f output/$(SCENARIO)/tb_top.vcd  # drop stale vcd from a previous TRACE=1 run
	@echo "running scenario $(SCENARIO); output -> output/$(SCENARIO)/"
	@$(PYTHON3) $(COORD_GEN) "$(SCENARIO_ABS)" "output/$(SCENARIO)/coord"
	@$(TBTOP_EXE) "+scenario_node0=output/$(SCENARIO)/coord/node0/scenario.yaml" \
	    "+scenario_node1=output/$(SCENARIO)/coord/node1/scenario.yaml" \
	    "+perf_out=output/$(SCENARIO)/perf.json" \
	    "+perf_scenario=$(SCENARIO)"$(if $(TRACE_PLUSARG_TB_TOP), $(TRACE_PLUSARG_TB_TOP),) \
	    > output/$(SCENARIO)/run.log 2>&1; \
	rc=$$?; \
	if ls master_shell_read_dump*.txt >/dev/null 2>&1; then \
	    mv master_shell_read_dump*.txt output/$(SCENARIO)/; \
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

clean: clean-genamba
	rm -rf $(VL_BUILD)/obj_dir $(VL_BUILD)/obj_dir_trace output
	rm -f master_shell_read_dump*.txt
	rm -f $(VL_BUILD)/master_shell_read_dump*.txt  # ctest runs Vtb_top with cwd build/verilator

# --- gen_amba role-1 testbench (Phase 1) ---
GENAMBA_OBJDIR := $(VL_BUILD)/obj_genamba$(TRACE_SUFFIX)
GENAMBA_SRC := $(GENAMBA_SV_SRC)
GENAMBA_C_SRC := \
    $(DPI_C_SRC) \
    $(COSIM_ROOT)/verilator/main_genamba.cpp
GENAMBA_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --timing \
    --top-module tb_genamba \
    --Mdir $(GENAMBA_OBJDIR) \
    --output-split 0 \
    -I$(COSIM_ROOT)/sv/genamba \
    $(GENAMBA_DEFINES)

# Strict-mode flag injection applies to genamba builds too (mirrors
# VERILATOR_FLAGS above).
GENAMBA_FLAGS += $(VERILATOR_EXTRA_FLAGS)

# Two-phase build mirrors tb_top pattern: Verilator generates C++ in
# $(GENAMBA_OBJDIR), then $(MAKE) compiles it.  --timing is used instead of
# --no-timing so the SV $time-based $display calls are accurate.
# --output-split 0: Verilator 5.036's default 20000-statement split can
# separate a coroutine's `__Vfork_N__sync` declaration from its use across
# generated .cpp chunks ("was not declared in this scope" C++ error) when
# fork-bearing tasks grow. Disabling the split keeps each coroutine whole;
# compile-time cost is acceptable at this design size.
# -LDFLAGS "-Wl,--stack,N" is intentionally omitted: Verilator 5.036 on
# Windows/MSYS2 escapes commas in generated .mk, breaking the link step.
#
# Incremental: the exe depends on SV + C sources, so an unchanged tree
# skips both Verilation and the C++ compile on repeat `make run-genamba`.
# `genamba` stays as the friendly alias. $(EXEEXT) defined near `all:`.
GENAMBA_EXE := $(GENAMBA_OBJDIR)/Vtb_genamba$(EXEEXT)
# sed pass between Verilation and C++ compile: Verilator 5.036 on Windows
# joins include-resolved source paths with backslashes and embeds them in
# generated C-string literals (fork-sync done() debug info). `\g` etc. are
# invalid escapes (2300+ g++ warnings) and `\t` silently corrupts to TAB.
# Rewrite only backslashes that follow a drive-letter path prefix —
# legitimate \n escapes in $display format strings never match.
$(GENAMBA_EXE): $(GENAMBA_SRC) $(GENAMBA_C_SRC) $(GENAMBA_INC_DEPS) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(GENAMBA_OBJDIR)
	$(VERILATOR) $(GENAMBA_FLAGS) $(GENAMBA_SRC) $(GENAMBA_C_SRC)
	sed -i -E ':a; s|([A-Za-z]:/[A-Za-z0-9_./]*)\\([A-Za-z0-9_])|\1/\2|; ta' \
	    $(GENAMBA_OBJDIR)/*.cpp $(GENAMBA_OBJDIR)/*.h
	$(MAKE) -C $(GENAMBA_OBJDIR) -f Vtb_genamba.mk \
	    PYTHON3="$(PYTHON3)"

.PHONY: genamba
genamba: $(GENAMBA_EXE)

# Build + run in one shot. Console output streams live AND lands in
# output/genamba_$(GENAMBA_RUN_TAG)/run.log (pipefail keeps the sim's
# exit code through the tee).
# Default cmodel_init() input is the standalone stub (no scenario tree
# dependency); pass GENAMBA_SCENARIO=<ax4-id> to point at a tree scenario
# instead (content is still inert: NMU/NSU take no config from it).
GENAMBA_SCENARIO ?=
ifeq ($(GENAMBA_SCENARIO),)
GENAMBA_SCENARIO_ABS := $(COSIM_ROOT)/genamba_init.yaml
GENAMBA_RUN_TAG := default
else
GENAMBA_SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(GENAMBA_SCENARIO)/scenario.yaml
GENAMBA_RUN_TAG := $(GENAMBA_SCENARIO)
endif
.PHONY: run-genamba
run-genamba: genamba
	@mkdir -p output/genamba_$(GENAMBA_RUN_TAG)
	@rm -f output/genamba_$(GENAMBA_RUN_TAG)/tb_genamba.vcd  # drop stale vcd from a previous TRACE=1 run
	@set -o pipefail; \
	$(GENAMBA_EXE) "+scenario=$(GENAMBA_SCENARIO_ABS)"$(if $(TRACE_PLUSARG_GENAMBA), $(TRACE_PLUSARG_GENAMBA),) \
	    2>&1 | tee output/genamba_$(GENAMBA_RUN_TAG)/run.log

.PHONY: clean-genamba
clean-genamba:
	rm -rf $(VL_BUILD)/obj_genamba $(VL_BUILD)/obj_genamba_trace \
	       $(VL_BUILD)/obj_genamba_tester

# --- pure-referee: gen_amba's own axi_tester drives the bridge ---------------
# Stimulus, data, and compares are upstream code (test selection via the
# upstream plusargs). The shared main is retargeted to the tester top via
# GENAMBA_TOP define + a force-include of the matching verilated header.
# MULTIPLE_OUTSTANDING is excluded from the default set: upstream task has
# a known index bug (see ATTRIBUTION.md, "Known upstream issue").
TESTER_OBJDIR := $(VL_BUILD)/obj_genamba_tester
TESTER_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --timing \
    --top-module tb_genamba_tester \
    --Mdir $(TESTER_OBJDIR) \
    --output-split 0 \
    -I$(COSIM_ROOT)/sv/genamba \
    $(GENAMBA_DEFINES) \
    -CFLAGS "-DGENAMBA_TESTER_TOP"
TESTER_FLAGS += $(VERILATOR_EXTRA_FLAGS)

TESTER_EXE := $(TESTER_OBJDIR)/Vtb_genamba_tester$(EXEEXT)
GENAMBA_TESTER_TESTS ?= +SINGLE_TEST=1 +BURST_TEST=1 +SINGLE_TEST_MEM=1 +BURST_TEST_MEM=1

$(TESTER_EXE): $(GENAMBA_TESTER_SV_SRC) $(GENAMBA_C_SRC) $(GENAMBA_INC_DEPS) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(TESTER_OBJDIR)
	$(VERILATOR) $(TESTER_FLAGS) $(GENAMBA_TESTER_SV_SRC) $(GENAMBA_C_SRC)
	sed -i -E ':a; s|([A-Za-z]:/[A-Za-z0-9_./]*)\\([A-Za-z0-9_])|\1/\2|; ta' \
	    $(TESTER_OBJDIR)/*.cpp $(TESTER_OBJDIR)/*.h
	$(MAKE) -C $(TESTER_OBJDIR) -f Vtb_genamba_tester.mk \
	    PYTHON3="$(PYTHON3)"

.PHONY: genamba-tester run-genamba-tester
genamba-tester: $(TESTER_EXE)

run-genamba-tester: $(TESTER_EXE)
	@mkdir -p output/genamba_tester
	@set -o pipefail; \
	$(TESTER_EXE) "+scenario=$(GENAMBA_SCENARIO_ABS)" $(GENAMBA_TESTER_TESTS) \
	    2>&1 | tee output/genamba_tester/run.log

# --- batch: one vcd per scenario, both TBs ------------------------------------
# Mirrors cosim/vcs run-all-fsdb (see that header for semantics): pre-builds
# the trace binary once, then runs every scenario; a scenario counts PASS
# only if the sim exits 0 AND its vcd is non-empty. NOT a regression gate
# (always exits 0 — ctest is the gate). AX4-INF-001 fails by design.
SCENARIOS_ALL := $(notdir $(wildcard $(PROJ_ROOT)/tests/scenarios/AX4-*))

.PHONY: run-all-trace
run-all-trace:
	@if ! $(MAKE) all TRACE=1; then \
	    echo ""; echo "run-all-trace: trace build failed — no scenarios were run."; \
	    exit 0; \
	fi; \
	pass=(); fail=(); vcds=(); \
	for s in $(SCENARIOS_ALL); do \
	    echo ""; echo "=== run-all-trace: $$s ==="; \
	    if $(MAKE) run-tb-top SCENARIO=$$s TRACE=1 && test -s "output/$$s/tb_top.vcd"; then \
	        pass+=("$$s"); vcds+=("output/$$s/tb_top.vcd"); \
	    else \
	        fail+=("$$s"); \
	    fi; \
	done; \
	echo ""; echo "=== run-all-trace: tb_genamba ($(GENAMBA_SCENARIO)) ==="; \
	if $(MAKE) run-genamba TRACE=1 && test -s "output/genamba_$(GENAMBA_RUN_TAG)/tb_genamba.vcd"; then \
	    pass+=("genamba_$(GENAMBA_SCENARIO)"); vcds+=("output/genamba_$(GENAMBA_RUN_TAG)/tb_genamba.vcd"); \
	else \
	    fail+=("genamba_$(GENAMBA_SCENARIO)"); \
	fi; \
	echo ""; echo "================ run-all-trace summary ================"; \
	echo "PASS (with vcd): $${#pass[@]}"; \
	for s in "$${pass[@]}"; do echo "  PASS $$s"; done; \
	echo "FAIL (no vcd or sim failed): $${#fail[@]}"; \
	for s in "$${fail[@]}"; do \
	    case "$$s" in \
	        AX4-INF-*) echo "  FAIL $$s  (fails by design)";; \
	        *)         echo "  FAIL $$s";; \
	    esac; \
	done; \
	echo "--- waveforms ---"; \
	if [ $${#vcds[@]} -gt 0 ]; then \
	    ls -lh "$${vcds[@]}" | awk '{print "  " $$5 "\t" $$9}'; \
	else \
	    echo "  (none)"; \
	fi; \
	exit 0
