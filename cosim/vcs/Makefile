# VCS build for the cosim testbenches. Source lists are shared with the
# Verilator flow via cosim/sources.mk; everything VCS-specific lives here.
# Build artifacts land under $(BUILD_ROOT)/vcs/ (top-level build/), run logs
# under ./output/.
#
# NOT runnable on the Windows host (VCS is Linux-only) — written for the
# workstation. Adjust the [WORKSTATION] block below to the local install
# (module load, license, version) before first use.
#
# Differences from the Verilator flow, by design:
# - No C++ main driver: VCS owns simulation time. tb_genamba is already
#   self-clocked; tb_top is wrapped by tb_top_vcs.sv (clock + reset +
#   timeout + final-block finalize, mirroring main.cpp).
# - None of the Verilator workarounds are needed (--output-split,
#   backslash-path sed, EXEEXT); the vendored-task latch/shadow patches
#   are simulator-neutral and stay.

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
# -----------------------------------------------------------------------------

include ../sources.mk

VCS_BUILD := $(BUILD_ROOT)/vcs

# yaml-cpp comes from the c_model CMake build; surface a clear hint instead
# of a mysterious link failure when it has not been built yet.
.PHONY: check-yamlcpp
check-yamlcpp:
	@if [ ! -f "$(YAMLCPP_LIB)" ]; then \
	    echo "ERROR: yaml-cpp static lib missing ($(YAMLCPP_LIB))."; \
	    echo "Run \`make build-cmodel\` at the repo root first."; \
	    exit 1; \
	fi

# DPI C++ is compiled and linked into simv by VCS itself; pass include dirs
# through -CFLAGS and the yaml-cpp static archive directly on the link line.
VCS_COMMON_FLAGS := \
    -full64 -sverilog \
    -timescale=1ns/1ps \
    -assert svaext \
    +incdir+$(COSIM_ROOT)/sv \
    +incdir+$(SPECGEN_SV_INC) \
    $(COMMON_DEFINES) \
    -CFLAGS "-std=c++17" \
    -CFLAGS "-DYAML_CPP_STATIC_DEFINE" \
    $(foreach i,$(CPP_INCLUDE_FLAGS),-CFLAGS "$(i)") \
    $(VCS_EXTRA)

# --- gen_amba role-1 testbench ---
GENAMBA_SCENARIO ?= AX4-BAS-001_single_write_no_read
GENAMBA_SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(GENAMBA_SCENARIO)/scenario.yaml

$(VCS_BUILD)/simv_genamba: $(GENAMBA_SV_SRC) $(DPI_C_SRC) | check-yamlcpp
	@mkdir -p $(VCS_BUILD)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    -Mdir=$(VCS_BUILD)/csrc_genamba \
	    +incdir+$(COSIM_ROOT)/sv/genamba \
	    $(GENAMBA_DEFINES) \
	    -top tb_genamba \
	    $(GENAMBA_SV_SRC) $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o $(VCS_BUILD)/simv_genamba

.PHONY: genamba run-genamba
genamba: $(VCS_BUILD)/simv_genamba

run-genamba: $(VCS_BUILD)/simv_genamba
	@mkdir -p output/genamba_$(GENAMBA_SCENARIO)
	@set -o pipefail; \
	$(VCS_BUILD)/simv_genamba "+scenario=$(GENAMBA_SCENARIO_ABS)" \
	    2>&1 | tee output/genamba_$(GENAMBA_SCENARIO)/run.log

# --- tb_top (wb2axip cosim) via tb_top_vcs wrapper ---
SCENARIO ?= AX4-BAS-003_single_write_read_aligned
SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(SCENARIO)/scenario.yaml

$(VCS_BUILD)/simv_tb_top: $(TB_TOP_SV_SRC) $(COSIM_ROOT)/sv/tb_top_vcs.sv $(DPI_C_SRC) | check-yamlcpp
	@mkdir -p $(VCS_BUILD)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    -Mdir=$(VCS_BUILD)/csrc_tb_top \
	    +incdir+$(COSIM_ROOT)/sv/wb2axip \
	    -top tb_top_vcs \
	    $(TB_TOP_SV_SRC) $(COSIM_ROOT)/sv/tb_top_vcs.sv $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o $(VCS_BUILD)/simv_tb_top

.PHONY: tb_top run-tb-top
tb_top: $(VCS_BUILD)/simv_tb_top

run-tb-top: $(VCS_BUILD)/simv_tb_top
	@mkdir -p output/$(SCENARIO)
	@set -o pipefail; \
	$(VCS_BUILD)/simv_tb_top "+scenario=$(SCENARIO_ABS)" \
	    2>&1 | tee output/$(SCENARIO)/run.log

.PHONY: clean
clean:
	rm -rf $(VCS_BUILD) output csrc *.log ucli.key vc_hdrs.h
