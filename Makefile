# Top-level Makefile — BUILD ONLY. Run all targets from repo root.
#
# Convention: the root builds (c_model + Verilator), runs the test gates, and
# runs simulations via `make sim TB=... PATTERN=...`.
# Run logs land in sim/verilator/output/<scenario>/run.log.
#
# All build artifacts live under the top-level build/ tree (gitignored):
#   build/cmodel/    CMake (c_model tests + FetchContent deps)
#   build/verilator/ obj_dir (tb_top)
#   build/vcs/       simv_* + csrc_* (workstation)

CMODEL_DIR      := src/c_model
BUILD_ROOT      := build
# Recursive (=) not immediate (:=): BUILD_ROOT can be overridden by `-include
# local.mk` below (e.g. $(HOME)/noc_build on WSL), and CMODEL_BUILD must pick up
# that override rather than freezing the pre-include `build` value.
CMODEL_BUILD     = $(BUILD_ROOT)/cmodel
COSIM_VERILATOR := sim/verilator
COSIM_VCS       := sim/vcs

.PHONY: help build build-cmodel build-yamlcpp build-verilator test \
        specgen_pytest sim \
        clean clean-cmodel clean-verilator clean-vcs clean-specgen-cache

help:
	@echo "Build (from repo root):"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only -> build/cmodel/"
	@echo "  make build-verilator  Verilator binaries -> build/verilator/"
	@echo ""
	@echo "Simulate:"
	@echo "  make sim TB=tb_<topo> PATTERN=<p> [SEED=<n>]   directed (neighbor/transpose/uniform_random/hotspot)"
	@echo "  make sim TB=tb_mesh_4x4_vc8 PATTERN=neighbor"
	@echo "  Vars: INJECTION_MODE= INJECTION_RATE= INJECTION_COUNT= MAX_UNIQUE_IDS= MAX_OUTSTANDING= HOTSPOT= (directed only); SEED unset draws + prints a random seed"
	@echo ""
	@echo "Test:"
	@echo "  make test             run c_model ctest suite"
	@echo ""
	@echo "Clean:"
	@echo "  make clean                  everything (build/ + per-sim output/ + generated stimulus)"
	@echo "  make clean-cmodel           build/cmodel/"
	@echo "  make clean-verilator        build/verilator/ + sim/verilator/output/"
	@echo "  make clean-vcs              build/vcs/ + sim/vcs/output/ + Verdi droppings"
	@echo "  make clean-specgen-cache    specgen __pycache__/"

# --- build ---

build: build-cmodel build-verilator

# CMake configure runs only when CMakeCache.txt is missing (first time or after
# clean-cmodel). Subsequent `make build-cmodel` is pure `cmake --build`, which
# avoids reconfigure triggering ninja to re-run side-effect custom targets
# (e.g. codegen_check) under a different subprocess env.
#
# TOOLPATH hardening: recipes run regardless of how complete the invoking
# shell's PATH is. Three deficits seen in practice on Windows/Git Bash:
# - mingw64/bin missing -> verilator (perl script) + g++ unresolvable
# - usr/bin missing     -> MSYS make/coreutils unresolvable
# - System32 missing    -> ninja's `cmd.exe /C` link rules (gtest discovery
#   POST_BUILD) fail with "'cmd.exe' is not recognized"
# MSYS dirs are PREpended (their coreutils must shadow Windows homonyms like
# find/sort); System32 is APPended (only cmd.exe is needed from there).
# All three are no-ops on Linux/macOS. LC_ALL=C silences MSYS perl locale
# complaints under non-UTF-8 Windows locales.
TOOLPATH := PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$$PATH:/c/Windows/System32" LC_ALL=C

# Per-host overrides (gitignored). Lets a machine pin CMAKE / DEPS_SRC / etc.
# once so the command line stays identical everywhere. Optional — the
# auto-detection below covers the common Windows + RHEL cases with no file.
#
# Per-host WSL config: create a gitignored `local.mk` at the repo root with:
#   BUILD_ROOT := $(HOME)/noc_build   # native-Linux build dir (WSL rejects /mnt COFF)
#   PYTHON3    := python3
#   VERILATOR  := verilator
# Then `make sim TB=tb_mesh_4x4_vc1 PATTERN=hotspot` needs no path/tool args.
-include local.mk

# CMake binary — auto-detected so the same `make build` works on every host.
# Prefer `cmake3` when present (RHEL ships the modern 3.x under that name, while
# bare `cmake` may be an ancient one shadowed onto PATH by e.g. a Xilinx SDK);
# otherwise fall through to `cmake` resolved at recipe time. Override in
# local.mk or on the command line if neither is right. Build needs cmake >= 3.14
# (FetchContent_MakeAvailable + gtest 1.14).
CMAKE ?= $(shell command -v cmake3 2>/dev/null || echo cmake)

# Extra cmake configure flags (escape hatch for host quirks). Common need:
# pin the Python interpreter when an EDA tool (e.g. Calibre) puts a broken
# python3 on PATH ahead of the system one and CMake's find_package(Python3)
# picks it. Set in local.mk, e.g.:
#   CMAKE_EXTRA := -DPython3_EXECUTABLE=/usr/bin/python3.12
CMAKE_EXTRA ?=

# Offline / firewalled hosts: FetchContent cannot download googletest/yaml-cpp.
# DEPS_SRC points at a dir holding pre-fetched googletest-src/ + yaml-cpp-src/
# (copied from an online host's build/cmodel/_deps/; .git subdirs not needed).
# Auto-engages if ~/noc_offline_deps exists, so the offline host needs no flag
# once the sources are unpacked there; override the path in local.mk if elsewhere.
# FULLY_DISCONNECTED turns any accidental download into a hard error, not a hang.
DEPS_SRC ?= $(wildcard $(HOME)/noc_offline_deps)
ifneq ($(strip $(DEPS_SRC)),)
CMAKE_DEPS_FLAGS := \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$(DEPS_SRC)/googletest-src \
    -DFETCHCONTENT_SOURCE_DIR_YAML-CPP=$(DEPS_SRC)/yaml-cpp-src
else
CMAKE_DEPS_FLAGS :=
endif

build-cmodel: $(CMODEL_BUILD)/CMakeCache.txt
	@$(TOOLPATH) $(CMAKE) --build $(CMODEL_BUILD) -j

# Sim needs only the yaml-cpp static lib (+ c_model/yaml-cpp headers via -I).
# Build that one target, not the whole c_model tree, so the gtest unit-test
# executables are never compiled (two of them hit a host GCC ICE).
build-yamlcpp: $(CMODEL_BUILD)/CMakeCache.txt
	@$(TOOLPATH) $(CMAKE) --build $(CMODEL_BUILD) --target yaml-cpp -j

$(CMODEL_BUILD)/CMakeCache.txt:
	@$(TOOLPATH) $(CMAKE) -S $(CMODEL_DIR) -B $(CMODEL_BUILD) $(CMAKE_DEPS_FLAGS) $(CMAKE_EXTRA)

# Default topology for standalone build-verilator.
# make sim overrides this by passing TOPOLOGY=$(TB) explicitly.
TOPOLOGY  ?= mesh_4x4_vc1
RUN_CLASS ?= directed

build-verilator: build-yamlcpp
	@$(TOOLPATH) $(MAKE) -C $(COSIM_VERILATOR) TOPOLOGY=$(TOPOLOGY) RUN_CLASS=$(RUN_CLASS)

# --- test ---

# TEST_TMPDIR: gtest's TempDir() checks TEST_TMPDIR before TEMP. MSYS sh
# (which executes make recipes) can strip/empty TEMP, making TempDir() fall
# back to a nonexistent temp dir and failing every test that writes a
# read-dump. Point it at a build-tree dir using a native path (`pwd -W` in
# MSYS sh; plain pwd elsewhere).
CTEST_CMD = mkdir -p $(CMODEL_BUILD)/test_tmp && cd $(CMODEL_BUILD) &&     TEST_TMPDIR="$$(pwd -W 2>/dev/null || pwd)/test_tmp" ctest --output-on-failure

test: build-cmodel
	@$(TOOLPATH) sh -c '$(CTEST_CMD)'

# Python interpreter: prefer the Windows `py -3` launcher when present
# (canonical on this project's Windows setup), fall back to python3
# (Linux/macOS and MSYS2 shells without the launcher on PATH).
PYTHON3 ?= $(if $(shell command -v py 2>/dev/null),py -3,python3)

# specgen codegen/golden drift gate. Runs the specgen pytest suite so a stale
# golden (e.g. an un-regenerated SV package) cannot pass silently. The pytest
# package is not present in every interpreter on this project's Windows setup
# (the MSYS2 mingw64 python lacks it); probe a candidate list and run the first
# interpreter that can import pytest. Fail loudly if none can -- a silent skip
# would defeat the gate.
SPECGEN_PYTEST_CANDIDATES := $(PYTHON3) python3 "py -3" python
specgen_pytest:
	@interp=""; \
	for cand in $(SPECGEN_PYTEST_CANDIDATES); do \
	    if $$cand -c "import pytest" >/dev/null 2>&1; then interp="$$cand"; break; fi; \
	done; \
	if [ -z "$$interp" ]; then \
	    echo "ERROR: no interpreter in [$(SPECGEN_PYTEST_CANDIDATES)] can import pytest; specgen drift gate cannot run" >&2; \
	    exit 1; \
	fi; \
	echo "specgen_pytest: using interpreter '$$interp'"; \
	cd specgen && $$interp -m pytest tests/ -q

# Unified DV run launcher. TB selects the testbench (topology; accepts a tb_ prefix).
# PATTERN selects one of the 4 spatial patterns, run directed (file_master +
# scoreboard). SEED unset -> a random 30-bit seed is drawn and printed so any
# run is replayable.
# BUILD_ROOT/PYTHON3/VERILATOR/FILELIST_F are NOT passed here -- they flow from
# root local.mk through sim/build_config.mk (see the local.mk note above).
TB      ?= mesh_4x4_vc1
PATTERN ?= neighbor
_TOPO   := $(TB:tb_%=%)
_VALID_PATTERNS := neighbor transpose uniform_random hotspot
ifeq ($(filter $(PATTERN),$(_VALID_PATTERNS)),)
$(error PATTERN must be one of: $(_VALID_PATTERNS) (got '$(PATTERN)'))
endif
# RANDOM is 0..32767; RANDOM*32768+RANDOM draws a uniform 30-bit seed (< 2**30),
# staying under Verilator's +verilator+seed+ int32 ceiling (< 2147483648). The old
# $RANDOM$RANDOM string-concat could reach 10 digits (~3.3e9) and abort the run.
_SEED   := $(if $(SEED),$(SEED),$(shell bash -c 'echo $$(( RANDOM * 32768 + RANDOM ))'))

# Forwarded only when set, so sim/verilator/Makefile's own defaults apply otherwise.
# INJECTION_COUNT's default depends on INJECTION_MODE and is computed there.
_INJ_ARGS := \
    $(if $(INJECTION_MODE),INJECTION_MODE=$(INJECTION_MODE)) \
    $(if $(INJECTION_RATE),INJECTION_RATE=$(INJECTION_RATE)) \
    $(if $(INJECTION_COUNT),INJECTION_COUNT=$(INJECTION_COUNT)) \
    $(if $(MAX_UNIQUE_IDS),MAX_UNIQUE_IDS=$(MAX_UNIQUE_IDS)) \
    $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING)) \
    $(if $(B_ROB_DEPTH),B_ROB_DEPTH=$(B_ROB_DEPTH)) \
    $(if $(R_ROB_DEPTH),R_ROB_DEPTH=$(R_ROB_DEPTH)) \
    $(if $(MAX_TXNS_PER_ID),MAX_TXNS_PER_ID=$(MAX_TXNS_PER_ID)) \
    $(if $(BURST_LEN),BURST_LEN=$(BURST_LEN))

.PHONY: sim
sim:
	@echo ">>> sim TB=$(_TOPO) PATTERN=$(PATTERN) SEED=$(_SEED)"
	$(MAKE) -C sim/verilator run-directed TOPOLOGY=$(_TOPO) RUN_CLASS=directed \
	    PATTERN=$(PATTERN) SEED=$(_SEED) $(_INJ_ARGS) $(if $(HOTSPOT),HOTSPOT=$(HOTSPOT))

# Injection-rate sweep: four VC configs x nine rates, one point per make sim.
# MAX_UNIQUE_IDS and MAX_OUTSTANDING are inherited, not forced. Both are shipped
# NI parameters, and the figure's subject is the machine as built. The bring-up
# step measures what other settings would buy, and reports it as a number.
# Heavy: rebuilds Verilator once per VC config. Run on WSL.
SWEEP_RATES ?= 0.05 0.1 0.2 0.3 0.4 0.5 0.7 0.85 1.0
SWEEP_VCS   ?= 1 2 4 8

.PHONY: sim-injection-sweep
sim-injection-sweep:
	@for vc in $(SWEEP_VCS); do \
	    for r in $(SWEEP_RATES); do \
	        echo ">>> sweep vc$$vc rate $$r"; \
	        $(MAKE) sim TB=tb_mesh_4x4_vc$${vc}_rob PATTERN=$(PATTERN) SEED=$(_SEED) \
	            INJECTION_MODE=1 INJECTION_RATE=$$r \
	            $(if $(MAX_UNIQUE_IDS),MAX_UNIQUE_IDS=$(MAX_UNIQUE_IDS)) \
	            $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING)) || exit 1; \
	    done; \
	done
	$(PYTHON3) sim/tools/plot_injection_sweep.py $(PATTERN)

# --- clean ---

clean: clean-cmodel clean-verilator clean-vcs clean-specgen-cache
	rm -rf $(BUILD_ROOT)
	rm -rf $(COSIM_VERILATOR)/../test_patterns/stim_*
	rm -f master_wrap_read_dump*.txt

clean-cmodel:
	rm -rf $(CMODEL_BUILD)

clean-verilator:
	$(MAKE) -C $(COSIM_VERILATOR) clean

clean-vcs:
	$(MAKE) -C sim/vcs clean

clean-specgen-cache:
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +
