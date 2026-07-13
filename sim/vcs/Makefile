# VCS build for the cosim testbenches. Source lists are shared with the
# Verilator flow via sim/build_config.mk; everything VCS-specific lives here.
# Topology used to generate tb_top_<TOPOLOGY>.sv. Override to switch topologies.
TOPOLOGY ?= mesh_4x4_vc1
# Run class (mirrors sim/verilator/Makefile): directed only.
RUN_CLASS ?= directed
ifeq ($(filter $(RUN_CLASS),directed),)
$(error RUN_CLASS must be directed (got '$(RUN_CLASS)'))
endif
# Build artifacts land under $(BUILD_ROOT)/vcs/ (top-level build/), run logs
# under ./output/.
#
# NOT runnable on the Windows host (VCS is Linux-only) — written for the
# workstation. Adjust the [WORKSTATION] block below to the local install
# (module load, license, version) before first use.
#
# Differences from the Verilator flow, by design:
# - No C++ main driver: VCS owns simulation time. tb_top is self-clocked; no
#   wrapper module is needed.
# - None of the Verilator workarounds are needed (--output-split,
#   backslash-path sed, EXEEXT).

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
# FSDB dumping (FSDB=1): Verdi install + PLI registration for $fsdbDump*.
# Defaults match the workstation's local reference Makefile (untracked,
# sim/ref/; Verdi 2020.03,
# PLI under LINUXAMD64). Alternate older install seen in the ref flows:
#   /cadtools/synopsys/verdi/M-2017.03/share/PLI/VCS/LINUX64/{novas.tab,pli.a}
# LD_LIBRARY_PATH may additionally be needed for the FSDB runtime libs
# (e.g. $VERDI_HOME/share/PLI/lib/LINUXAMD64) — verify at first run, record here.
VERDI_HOME ?= /tools/verdi_2020.03
FSDB_PLI   ?= -P $(VERDI_HOME)/share/PLI/VCS/LINUXAMD64/novas.tab \
                 $(VERDI_HOME)/share/PLI/VCS/LINUXAMD64/pli.a
# Heavier known-working combo from the ref flow, for memory dump /
# interactive Verdi debug (slower sim, bigger fsdb):
#   FSDB_EXTRA += -debug_access+all -debug_all +fsdb+all +vcsd
FSDB_EXTRA ?=
# -----------------------------------------------------------------------------

include ../build_config.mk

VCS_BUILD := $(BUILD_ROOT)/vcs

# FILELIST_F / FILELIST_GEN_ARGS come from build_config.mk. sim/filelist_<TOPOLOGY>.f
# is a generated artifact (gitignored): VCS regenerates it itself (recipe below) so
# a standalone `cd sim/vcs && make` works on a fresh clone where the .f is absent.

# --- FSDB waveform dumping (opt-in; Verdi PLI, workstation only) -------------
# FSDB=1 compiles a separate simv with +define+FSDB_DUMP and the Verdi PLI
# linked in. FSDB and non-FSDB builds use distinct simv/csrc names because
# make does not track flag changes — sharing a binary across modes would
# silently reuse the wrong one.
FSDB ?= 0
ifeq ($(FSDB),1)
ifeq ($(strip $(VERDI_HOME)),)
$(error FSDB=1 requires VERDI_HOME (Verdi install root) to be set)
endif
FSDB_COMPILE_FLAGS := +define+FSDB_DUMP $(FSDB_PLI) $(FSDB_EXTRA)
FSDB_SUFFIX := _fsdb
else
FSDB_COMPILE_FLAGS :=
FSDB_SUFFIX :=
endif
# -----------------------------------------------------------------------------

# yaml-cpp comes from the c_model CMake build; surface a clear hint instead
# of a mysterious link failure when it has not been built yet.
.PHONY: check-yamlcpp
check-yamlcpp:
	@if [ ! -f "$(YAMLCPP_LIB)" ]; then \
	    echo "ERROR: yaml-cpp static lib missing ($(YAMLCPP_LIB))."; \
	    echo "Run \`make build-cmodel\` at the repo root first."; \
	    exit 1; \
	fi

# filelist.f recipe: placed after check-yamlcpp so it is not the default goal.
# Duplicated from sim/verilator/Makefile (intentionally) so a standalone
# `cd sim/vcs && make` regenerates filelist.f on a fresh clone where it is
# absent; gen_filelist.py + FILELIST_GEN_ARGS keep the two recipes in sync.
$(FILELIST_F): ../build_config.mk $(COSIM_ROOT)/tools/gen_filelist.py
	$(PYTHON3) $(COSIM_ROOT)/tools/gen_filelist.py $@ $(FILELIST_GEN_ARGS)

# tb_top_<TOPOLOGY>.sv + noc_fabric_<topo>.sv are generated artifacts; regenerate
# from the topology config before building. TB_TOP_SV comes from build_config.mk
# (deferred = so TOPOLOGY expands correctly).
TB_TOP_TOPO   := $(COSIM_ROOT)/topologies/$(TOPOLOGY_BASE).yaml
GEN_TB_TOP    := $(COSIM_ROOT)/tools/gen_tb_top.py
TOPO_STAMP    := $(VCS_BUILD)/.topology

# TOPOLOGY stamp records the last-built topology. The FORCE-driven rule rewrites
# the stamp ONLY when TOPOLOGY changed (content compare), bumping its mtime so the
# tb_top_<TOPOLOGY>.sv prerequisite check re-emits on a topology switch. Mirrors
# the Verilator Makefile's mechanism exactly. Unchanged topology leaves the stamp
# alone.
.PHONY: FORCE
FORCE:
$(TOPO_STAMP): FORCE
	@mkdir -p $(VCS_BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(TOPOLOGY)_$(RUN_CLASS)" ]; then \
	    echo "$(TOPOLOGY)_$(RUN_CLASS)" > $@; \
	fi

$(TB_TOP_SV): $(TB_TOP_TOPO) $(GEN_TB_TOP) $(TOPO_STAMP)
	$(PYTHON3) $(GEN_TB_TOP) --topology $(TOPOLOGY) --out $(TB_TOP_SV)

# DPI C++ is compiled and linked into simv by VCS itself; pass include dirs
# through -CFLAGS and the yaml-cpp static archive directly on the link line.
VCS_COMMON_FLAGS := \
    -full64 -sverilog \
    -timescale=1ns/1ps \
    -assert svaext \
    +incdir+$(COSIM_ROOT)/tb \
    +incdir+$(SRC_SV) \
    +incdir+$(SPECGEN_SV_INC) \
    +incdir+$(DV_ROOT)/axi-0.39.7/include \
    -CFLAGS "-std=c++17" \
    -CFLAGS "-DYAML_CPP_STATIC_DEFINE" \
    $(foreach i,$(CPP_INCLUDE_FLAGS),-CFLAGS "$(i)") \
    $(STDCXXFS_LDFLAGS) \
    $(VCS_EXTRA)
ifeq ($(FSDB),1)
# guarded: += of an empty var would add a trailing space and break FSDB=0 byte-identity
VCS_COMMON_FLAGS += $(FSDB_COMPILE_FLAGS)
endif

# --- tb_top cosim (self-clocked) ---
PYTHON3    ?= $(if $(shell command -v py 2>/dev/null),py -3,python3)

# Per-topology, per-run-class simv + csrc dir (mirrors the Verilator
# obj_dir_$(TOPOLOGY)_$(RUN_CLASS) isolation): building one flavor must not
# clobber another's binary.
SIMV_TB_TOP := $(VCS_BUILD)/simv_tb_top_$(TOPOLOGY)_$(RUN_CLASS)$(FSDB_SUFFIX)

$(SIMV_TB_TOP): $(FILELIST_F) $(TB_TOP_SV_SRC) $(DPI_C_SRC) $(DPI_HDR_DEPS) | check-yamlcpp
	@mkdir -p $(VCS_BUILD)
	$(VCS) $(VCS_COMMON_FLAGS) \
	    -Mdir=$(VCS_BUILD)/csrc_tb_top_$(TOPOLOGY)_$(RUN_CLASS)$(FSDB_SUFFIX) \
	    -top tb_top \
	    -f $(FILELIST_F) $(DPI_C_SRC) $(YAMLCPP_LIB) \
	    -o $(SIMV_TB_TOP)

.PHONY: tb_top
tb_top: $(SIMV_TB_TOP)

.PHONY: clean
clean:
	rm -rf $(VCS_BUILD) output csrc *.log ucli.key vc_hdrs.h
	rm -rf verdiLog novas.*  # Verdi/FSDB cwd droppings
