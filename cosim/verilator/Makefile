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

VERILATOR_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --top-module $(TOP) \
    --no-timing \
    --Mdir $(VL_BUILD)/obj_dir \
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
TBTOP_EXE := $(VL_BUILD)/obj_dir/V$(TOP)$(EXEEXT)

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
$(TBTOP_EXE): $(SV_SRC) $(C_SRC) | check-yamlcpp
	@mkdir -p $(VL_BUILD)/obj_dir
	$(VERILATOR) $(VERILATOR_FLAGS) $(SV_SRC) $(C_SRC)
	$(MAKE) -C $(VL_BUILD)/obj_dir -f V$(TOP).mk \
	    PYTHON3="$(PYTHON3)" \
	    "USER_LDFLAGS=-Wl,--stack,67108864"

# Run tb_top with cwd here so the in-project read-dump file lands locally,
# then move it into output/$(SCENARIO)/ alongside run.log. scenario_parser
# resolves data_file: relative to the YAML's own dir so cwd does not matter
# for input resolution.
SCENARIO ?= AX4-BAS-003_single_write_read_aligned
SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(SCENARIO)/scenario.yaml

.PHONY: run-tb-top
run-tb-top: $(TBTOP_EXE)
	@mkdir -p output/$(SCENARIO)
	@echo "running scenario $(SCENARIO); output -> output/$(SCENARIO)/"
	@$(TBTOP_EXE) "+scenario=$(SCENARIO_ABS)" \
	    > output/$(SCENARIO)/run.log 2>&1; \
	rc=$$?; \
	if ls master_shell_read_dump*.txt >/dev/null 2>&1; then \
	    mv master_shell_read_dump*.txt output/$(SCENARIO)/; \
	fi; \
	echo "--- run.log (tail) ---"; \
	tail -8 output/$(SCENARIO)/run.log; \
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
	rm -rf $(VL_BUILD)/obj_dir output
	rm -f master_shell_read_dump*.txt

# --- gen_amba role-1 testbench (Phase 1) ---
GENAMBA_OBJDIR := $(VL_BUILD)/obj_genamba
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
$(GENAMBA_EXE): $(GENAMBA_SRC) $(GENAMBA_C_SRC) | check-yamlcpp
	@mkdir -p $(GENAMBA_OBJDIR)
	$(VERILATOR) $(GENAMBA_FLAGS) $(GENAMBA_SRC) $(GENAMBA_C_SRC)
	sed -i -E ':a; s|([A-Za-z]:/[A-Za-z0-9_./]*)\\([A-Za-z0-9_])|\1/\2|; ta' \
	    $(GENAMBA_OBJDIR)/*.cpp $(GENAMBA_OBJDIR)/*.h
	$(MAKE) -C $(GENAMBA_OBJDIR) -f Vtb_genamba.mk \
	    PYTHON3="$(PYTHON3)"

.PHONY: genamba
genamba: $(GENAMBA_EXE)

# Build + run in one shot. Console output streams live AND lands in
# output/genamba_$(GENAMBA_SCENARIO)/run.log (pipefail keeps the sim's
# exit code through the tee).
GENAMBA_SCENARIO ?= AX4-BAS-001_single_write_no_read
GENAMBA_SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(GENAMBA_SCENARIO)/scenario.yaml
.PHONY: run-genamba
run-genamba: genamba
	@mkdir -p output/genamba_$(GENAMBA_SCENARIO)
	@set -o pipefail; \
	$(GENAMBA_EXE) "+scenario=$(GENAMBA_SCENARIO_ABS)" \
	    2>&1 | tee output/genamba_$(GENAMBA_SCENARIO)/run.log

.PHONY: clean-genamba
clean-genamba:
	rm -rf $(GENAMBA_OBJDIR)
