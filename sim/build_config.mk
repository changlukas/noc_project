# Build environment config and simulator-neutral source lists shared by
# sim/verilator/Makefile and sim/vcs/Makefile.
#
# tb_top SV sources live in sim/filelist_<TOPOLOGY>.f (generated from
# TB_TOP_SV_SRC below); both Makefiles use -f filelist_<TOPOLOGY>.f for the
# tb_top verilate/compile step.
#
# COSIM_ROOT is derived from this file's own location so any includer depth
# works.
COSIM_ROOT  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PROJ_ROOT   := $(patsubst %/,%,$(dir $(COSIM_ROOT)))

# Per-host overrides (gitignored, repo-root local.mk) — same file the root
# Makefile reads, so sim runs pick up host knobs (e.g. DPI_CXX, VCS,
# VERDI_HOME) without per-invocation flags. Optional.
-include $(PROJ_ROOT)/local.mk

# All build artifacts live under the top-level build/ tree:
#   build/cmodel/    CMake (c_model tests + FetchContent deps)
#   build/verilator/ obj_dir (tb_top)
#   build/vcs/       simv_* + csrc_* + *.daidir
# ?= (not :=) so a repo-root local.mk BUILD_ROOT (included above) survives — WSL
# needs a native-Linux build dir; /mnt COFF archives are rejected by the WSL ld.
# A command-line BUILD_ROOT= still wins over both.
BUILD_ROOT     ?= $(PROJ_ROOT)/build

CMODEL_INC     := $(PROJ_ROOT)/src/c_model/include
CMODEL_TESTS   := $(PROJ_ROOT)/src/c_model/tests
SPECGEN_INC    := $(PROJ_ROOT)/specgen/generated/cpp
SPECGEN_SV_INC := $(PROJ_ROOT)/specgen/generated/sv
SRC_SV         := $(PROJ_ROOT)/src/sv
SRC_DPI        := $(PROJ_ROOT)/src/dpi
# Imported DV/VIP source set (pulp axi + common_verification + FlooNoC monitors).
# NEVER edit files under $(DV_ROOT) — they are vendored verbatim.
DV_ROOT        := $(COSIM_ROOT)/dv

# yaml-cpp HEADERS: online builds clone the source into the build tree; offline
# (DEPS_SRC) builds use the pre-fetched source IN PLACE, so the headers live
# there, not under build/. Gate on DEPS_SRC (same condition the root Makefile
# uses for FetchContent), NOT on a build-tree wildcard — the build tree may not
# be populated yet at parse time. `cd sim/<sim> && make` does not run the root
# Makefile, so resolve DEPS_SRC here too (auto-detect ~/noc_offline_deps;
# pinnable via local.mk).
DEPS_SRC ?= $(wildcard $(HOME)/noc_offline_deps)
ifneq ($(strip $(DEPS_SRC)),)
YAMLCPP_INC := $(DEPS_SRC)/yaml-cpp-src/include
else
YAMLCPP_INC := $(BUILD_ROOT)/cmodel/_deps/yaml-cpp-src/include
endif
# yaml-cpp LIBRARY is always a build artifact, so it stays under the build tree.
YAMLCPP_LIB    := $(BUILD_ROOT)/cmodel/_deps/yaml-cpp-build/libyaml-cpp.a

# GCC < 9 keeps std::filesystem in a separate library (libstdc++fs); the DPI
# pulls it in via scenario_parser.hpp, so the link needs -lstdc++fs. GCC >= 9
# folded it into libstdc++ and mingw GCC 9+ has no such archive, so the flag is
# added ONLY for old GCC. Auto-detected from the C++ compiler; override DPI_CXX
# if the simulator links with a different compiler than `g++` on PATH.
#
# --whole-archive wrap: VCS injects -LDFLAGS at the FRONT of the link line,
# before cmodel_dpi.o. GNU ld is order-sensitive — a plain `-lstdc++fs` ahead
# of the object that needs it gets discarded ("undefined reference to
# std::filesystem::..."). --whole-archive forces every libstdc++fs member in
# unconditionally, so resolution no longer depends on link order. Both verilator
# and vcs accept the -LDFLAGS "..." form.
DPI_CXX        ?= g++
DPI_GXX_MAJOR  := $(shell $(DPI_CXX) -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)
STDCXXFS_LDFLAGS :=
ifneq ($(DPI_GXX_MAJOR),)
ifeq ($(shell test $(DPI_GXX_MAJOR) -lt 9 2>/dev/null && echo 1),1)
STDCXXFS_LDFLAGS := -LDFLAGS "-Wl,--whole-archive -lstdc++fs -Wl,--no-whole-archive"
endif
endif

# --- tb_top sim ---
# TB_TOP_SV_SRC is consumed by the filelist_<TOPOLOGY>.f generation recipe below
# and by VCS (via -f filelist_<TOPOLOGY>.f). Paths are relative to COSIM_ROOT so
# the variable stays readable; gen_filelist.py absolutizes them.
# TB_TOP_SV is the generated top file; per-topology so multiple tbs coexist
# (tb_top_<TOPOLOGY>.sv). Use deferred = so TOPOLOGY expansion is lazy.
TB_TOP_SV = $(COSIM_ROOT)/tb/tb_top_$(TOPOLOGY).sv
# Extract num_vc suffix from TOPOLOGY name (e.g. mesh_4x4_vc4 -> vc4 -> noc_types_pkg_vc4.sv).
# Strip a trailing _rob suffix first (e.g. mesh_4x4_vc2_rob -> mesh_4x4_vc2) so the _vc split
# always lands on the vc word (vc2), not "vc2_rob" which has no matching noc_types_pkg file.
# $(lastword $(subst _vc, vc,...)) then splits on "_vc" and takes the last word (e.g. "vc2").
TOPOLOGY_BASE = $(TOPOLOGY:_rob=)
# pulp AXI crossbar subset (sim/dv/README.md): the tile decoder and the memory
# behind it in user_node_endpoint.sv. Taken at v0.39.7 / v1.37.0, the versions
# already vendored, so nothing here mixes releases -- at v0.39.7
# axi_demux_id_counters is a second module inside axi_demux_simple.sv rather
# than its own file, which a master-branch copy would get wrong.
#
# Declaration order matters for the packages and for common_cells, which the
# axi modules instantiate.
PULP_AXI  := $(DV_ROOT)/axi-0.39.7/src
PULP_CC   := $(DV_ROOT)/common_cells-1.37.0/src
XBAR_SRC := \
    $(PULP_CC)/lzc.sv \
    $(PULP_CC)/rr_arb_tree.sv \
    $(PULP_CC)/fifo_v3.sv \
    $(PULP_CC)/delta_counter.sv \
    $(PULP_CC)/counter.sv \
    $(PULP_CC)/spill_register_flushable.sv \
    $(PULP_CC)/spill_register.sv \
    $(PULP_CC)/stream_register.sv \
    $(PULP_CC)/lfsr_16bit.sv \
    $(PULP_CC)/stream_delay.sv \
    $(PULP_AXI)/axi_id_prepend.sv \
    $(PULP_AXI)/axi_id_remap.sv \
    $(PULP_AXI)/axi_atop_filter.sv \
    $(PULP_AXI)/axi_err_slv.sv \
    $(PULP_AXI)/axi_cut.sv \
    $(PULP_AXI)/axi_multicut.sv \
    $(PULP_AXI)/axi_demux_simple.sv \
    $(PULP_AXI)/axi_demux.sv \
    $(PULP_AXI)/axi_mux.sv \
    $(PULP_AXI)/axi_xbar_unmuxed.sv \
    $(PULP_AXI)/axi_xbar.sv \
    $(PULP_AXI)/axi_delayer.sv \
    $(PULP_AXI)/axi_sim_mem.sv

# noc_fabric_<topo>.sv is emitted alongside tb_top by gen_tb_top.py and `include`d
# BY tb_top, so it must never enter TB_TOP_SV_SRC (that would define the module
# twice). It is still a real compile input: each simulator's binary rule lists it
# separately, otherwise a hand-edit (e.g. a debug probe) leaves the binary "up to
# date" and the edit is silently never compiled.
NOC_FABRIC_SV = $(SRC_SV)/noc_fabric_$(TOPOLOGY_BASE).sv
TOPOLOGY_NOC_TYPES_PKG = $(SPECGEN_SV_INC)/noc_types_pkg_$(lastword $(subst _vc, vc,$(TOPOLOGY_BASE))).sv
TB_TOP_SV_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(TOPOLOGY_NOC_TYPES_PKG) \
    $(SPECGEN_SV_INC)/ni_flit_pkg.sv \
    $(DV_ROOT)/common_cells-1.37.0/src/cf_math_pkg.sv \
    $(DV_ROOT)/common_cells-1.37.0/src/addr_decode_dync.sv \
    $(DV_ROOT)/common_cells-1.37.0/src/addr_decode.sv \
    $(DV_ROOT)/common_verification-0.2.5/src/rand_id_queue.sv \
    $(DV_ROOT)/axi-0.39.7/src/axi_pkg.sv \
    $(DV_ROOT)/axi-0.39.7/src/axi_intf.sv \
    $(DV_ROOT)/axi-0.39.7/src/axi_test.sv \
    $(DV_ROOT)/floonoc-test/axi_bw_monitor.sv \
    $(XBAR_SRC) \
    $(COSIM_ROOT)/tb/axi_vip_types_pkg.sv \
    $(SRC_SV)/nmu_wrap.sv \
    $(SRC_SV)/router_wrap.sv \
    $(SRC_SV)/nsu_wrap.sv \
    $(SRC_SV)/ni_wrap.sv \
    $(COSIM_ROOT)/tb/user_node_endpoint.sv \
    $(COSIM_ROOT)/tb/link_perf_monitor.sv \
    $(TB_TOP_SV)

# sim/filelist_<TOPOLOGY>.f is a GENERATED build artifact (gitignored), not
# committed — it bakes in host-absolute paths. Both sim flows regenerate it
# from TB_TOP_SV_SRC via gen_filelist.py before use. The recipe is duplicated
# in each Makefile (rather than defined here) so it never becomes the default
# goal of an includer. FILELIST_F / FILELIST_GEN_ARGS centralize the shared
# bits so the two recipes stay in sync.
FILELIST_F = $(COSIM_ROOT)/filelist_$(TOPOLOGY).f
# gen_filelist.py args: <out> <incdir...> -- <src...>. The incdirs mirror the
# -I/+incdir+ the simulators already pass; listing them in the .f makes it
# self-contained for tool-native -f consumption.
FILELIST_GEN_ARGS = $(SPECGEN_SV_INC) $(COSIM_ROOT)/tb $(SRC_SV) \
    $(DV_ROOT)/axi-0.39.7/include $(DV_ROOT)/common_cells-1.37.0/include -- $(TB_TOP_SV_SRC)

# DPI implementation shared by every simulator; the C++ *main* driver
# (main.cpp) is Verilator-only and listed in sim/verilator/Makefile, NOT here
# — under VCS the simulator owns time.
DPI_C_SRC := $(SRC_DPI)/cmodel_dpi.cpp

# DPI C++ (cmodel_dpi.cpp) pulls in the c_model headers (wrap adapters and
# their transitive includes). The obj-dir sub-make tracks them via -MMD, but
# the TOP-level rules must list them too — otherwise a header-only change
# leaves the simulator binary stale because the sub-make never runs.
DPI_HDR_DEPS := \
    $(wildcard $(PROJ_ROOT)/src/c_model/include/*.hpp) \
    $(wildcard $(PROJ_ROOT)/src/c_model/include/*/*.hpp) \
    $(wildcard $(PROJ_ROOT)/src/c_model/include/*/*/*.hpp) \
    $(wildcard $(PROJ_ROOT)/src/c_model/tests/common/*.hpp) \
    $(wildcard $(PROJ_ROOT)/specgen/generated/cpp/*.hpp)

CPP_INCLUDE_FLAGS := \
    -I$(SRC_DPI) \
    -I$(CMODEL_INC) \
    -I$(CMODEL_TESTS) \
    -I$(SPECGEN_INC) \
    -I$(YAMLCPP_INC)
