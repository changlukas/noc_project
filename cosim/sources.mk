# Simulator-neutral source lists shared by cosim/verilator/Makefile and
# cosim/vcs/Makefile. Keep ONLY file lists, include dirs, and +define+s here;
# simulator-specific flags stay in the per-simulator Makefiles.
#
# COSIM_ROOT is derived from this file's own location so any includer depth
# works.
COSIM_ROOT  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PROJ_ROOT   := $(patsubst %/,%,$(dir $(COSIM_ROOT)))

# All build artifacts live under the top-level build/ tree:
#   build/cmodel/    CMake (c_model tests + FetchContent deps)
#   build/verilator/ obj_dir (tb_top) + obj_genamba
#   build/vcs/       simv_* + csrc_* + *.daidir
BUILD_ROOT     := $(PROJ_ROOT)/build

CMODEL_INC     := $(PROJ_ROOT)/c_model/include
CMODEL_TESTS   := $(PROJ_ROOT)/c_model/tests
SPECGEN_INC    := $(PROJ_ROOT)/specgen/generated/cpp
SPECGEN_SV_INC := $(PROJ_ROOT)/specgen/generated/sv
YAMLCPP_INC    := $(BUILD_ROOT)/cmodel/_deps/yaml-cpp-src/include
YAMLCPP_LIB    := $(BUILD_ROOT)/cmodel/_deps/yaml-cpp-build/libyaml-cpp.a

# --- tb_top (wb2axip cosim) ---
TB_TOP_SV_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(COSIM_ROOT)/sv/axi_master_wrap.sv \
    $(COSIM_ROOT)/sv/nmu_wrap.sv \
    $(COSIM_ROOT)/sv/channel_model_wrap.sv \
    $(COSIM_ROOT)/sv/nsu_wrap.sv \
    $(COSIM_ROOT)/sv/axi_slave_wrap.sv \
    $(COSIM_ROOT)/sv/wb2axip/faxi_wstrb.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_master.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_slave.v \
    $(COSIM_ROOT)/sv/tb_top.sv

# DPI implementation shared by every simulator; the C++ *main* drivers
# (main.cpp / main_genamba.cpp) are Verilator-only and listed in
# cosim/verilator/Makefile, NOT here — under VCS the simulator owns time.
DPI_C_SRC := $(COSIM_ROOT)/c/cmodel_dpi.cpp

# --- gen_amba role-1 testbench ---
GENAMBA_SV_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(COSIM_ROOT)/sv/nmu_wrap.sv \
    $(COSIM_ROOT)/sv/nsu_wrap.sv \
    $(COSIM_ROOT)/sv/genamba/mem_axi.v \
    $(COSIM_ROOT)/sv/genamba_master_bfm.sv \
    $(COSIM_ROOT)/sv/tb_genamba.sv

GENAMBA_DEFINES := \
    +define+AMBA_AXI4 +define+AMBA_QOS \
    +define+AMBA_AXI_CACHE +define+AMBA_AXI_PROT

COMMON_DEFINES := +define+assume=assert

CPP_INCLUDE_FLAGS := \
    -I$(COSIM_ROOT)/c \
    -I$(CMODEL_INC) \
    -I$(CMODEL_TESTS) \
    -I$(SPECGEN_INC) \
    -I$(YAMLCPP_INC)
