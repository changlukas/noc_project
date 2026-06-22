# Simulator-neutral source lists shared by cosim/verilator/Makefile and
# cosim/vcs/Makefile. Keep ONLY file lists, include dirs, and +define+s here;
# simulator-specific flags stay in the per-simulator Makefiles.
#
# COSIM_ROOT is derived from this file's own location so any includer depth
# works.
COSIM_ROOT  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PROJ_ROOT   := $(patsubst %/,%,$(dir $(COSIM_ROOT)))

# Per-host overrides (gitignored, repo-root local.mk) — same file the root
# Makefile reads, so cosim runs pick up host knobs (e.g. DPI_CXX, VCS,
# VERDI_HOME) without per-invocation flags. Optional.
-include $(PROJ_ROOT)/local.mk

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

# GCC < 9 keeps std::filesystem in a separate library (libstdc++fs); the DPI
# pulls it in via scenario_parser.hpp, so the simv/obj link needs -lstdc++fs.
# GCC >= 9 folded it into libstdc++ and mingw GCC 9+ has no such archive, so the
# flag is added ONLY for old GCC. Auto-detected from the C++ compiler; override
# DPI_CXX if the simulator links with a different compiler than `g++` on PATH.
# Both verilator and vcs accept the -LDFLAGS "..." form.
DPI_CXX        ?= g++
DPI_GXX_MAJOR  := $(shell $(DPI_CXX) -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)
STDCXXFS_LDFLAGS :=
ifneq ($(DPI_GXX_MAJOR),)
ifeq ($(shell test $(DPI_GXX_MAJOR) -lt 9 2>/dev/null && echo 1),1)
STDCXXFS_LDFLAGS := -LDFLAGS "-lstdc++fs"
endif
endif

# --- tb_top cosim ---
TB_TOP_SV_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(COSIM_ROOT)/sv/axi_master_wrap.sv \
    $(COSIM_ROOT)/sv/nmu_wrap.sv \
    $(COSIM_ROOT)/sv/router_wrap.sv \
    $(COSIM_ROOT)/sv/nsu_wrap.sv \
    $(COSIM_ROOT)/sv/axi_slave_wrap.sv \
    $(COSIM_ROOT)/sv/axi_perf_monitor.sv \
    $(COSIM_ROOT)/sv/flit_link_perf_monitor.sv \
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

# Pure-referee variant: gen_amba's own axi_tester drives the bridge with
# its upstream test sequence; no project BFM, no project patterns.
GENAMBA_TESTER_SV_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(COSIM_ROOT)/sv/nmu_wrap.sv \
    $(COSIM_ROOT)/sv/nsu_wrap.sv \
    $(COSIM_ROOT)/sv/genamba/mem_axi.v \
    $(COSIM_ROOT)/sv/genamba/axi_tester.v \
    $(COSIM_ROOT)/sv/tb_genamba_tester.sv

# `include'd task bodies — NOT standalone compile units (never pass them to
# the simulator command line), but they ARE build inputs: list them as extra
# prerequisites on the genamba build rules so editing them triggers a
# rebuild (previously invisible to make).
GENAMBA_INC_DEPS := \
    $(COSIM_ROOT)/sv/genamba/axi_master_tasks.v \
    $(COSIM_ROOT)/sv/genamba/mem_test_tasks.v \
    $(COSIM_ROOT)/sv/genamba/mem_axi_dpram_sync.v

# DPI C++ (cmodel_dpi.cpp) pulls in the c_model headers (shell adapters and
# their transitive includes). The obj-dir sub-make tracks them via -MMD, but
# the TOP-level rules must list them too — otherwise a header-only change
# leaves the simulator binary stale because the sub-make never runs.
DPI_HDR_DEPS := \
    $(wildcard $(PROJ_ROOT)/c_model/include/*.hpp) \
    $(wildcard $(PROJ_ROOT)/c_model/include/*/*.hpp) \
    $(wildcard $(PROJ_ROOT)/c_model/include/*/*/*.hpp) \
    $(wildcard $(PROJ_ROOT)/c_model/tests/common/*.hpp) \
    $(wildcard $(PROJ_ROOT)/specgen/generated/cpp/*.hpp)

GENAMBA_DEFINES := \
    +define+AMBA_AXI4 +define+AMBA_QOS \
    +define+AMBA_AXI_CACHE +define+AMBA_AXI_PROT

CPP_INCLUDE_FLAGS := \
    -I$(COSIM_ROOT)/c \
    -I$(CMODEL_INC) \
    -I$(CMODEL_TESTS) \
    -I$(SPECGEN_INC) \
    -I$(YAMLCPP_INC)
