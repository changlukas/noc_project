# VCS build for the cosim testbenches. Source lists are shared with the
# Verilator flow via cosim/sources.mk; everything VCS-specific lives here.
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

# --- [WORKSTATION] adjust to local environment -------------------------------
VCS      ?= vcs
# Typical extras you may need here:
#   VCS_EXTRA += -licqueue
#   CC/CXX overrides if the site default gcc is too old for -std=c++17
VCS_EXTRA ?=
# -----------------------------------------------------------------------------

include ../sources.mk

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

simv_genamba: $(GENAMBA_SV_SRC) $(DPI_C_SRC)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    +incdir+$(COSIM_ROOT)/sv/genamba \
	    $(GENAMBA_DEFINES) \
	    -top tb_genamba \
	    $(GENAMBA_SV_SRC) $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o simv_genamba

.PHONY: genamba run-genamba
genamba: simv_genamba

run-genamba: simv_genamba
	./simv_genamba "+scenario=$(GENAMBA_SCENARIO_ABS)"

# --- tb_top (wb2axip cosim) via tb_top_vcs wrapper ---
SCENARIO ?= AX4-BAS-003_single_write_read_aligned
SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(SCENARIO)/scenario.yaml

simv_tb_top: $(TB_TOP_SV_SRC) $(COSIM_ROOT)/sv/tb_top_vcs.sv $(DPI_C_SRC)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    +incdir+$(COSIM_ROOT)/sv/wb2axip \
	    -top tb_top_vcs \
	    $(TB_TOP_SV_SRC) $(COSIM_ROOT)/sv/tb_top_vcs.sv $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o simv_tb_top

.PHONY: tb_top run-tb-top
tb_top: simv_tb_top

run-tb-top: simv_tb_top
	./simv_tb_top "+scenario=$(SCENARIO_ABS)"

.PHONY: clean
clean:
	rm -rf simv_genamba simv_tb_top simv*.daidir csrc *.log ucli.key vc_hdrs.h
