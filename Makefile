# Top-level Makefile — build and test gates. Run these targets from repo root.
#
# Simulation is not here: `make -C sim TB=... PATTERN=...`, see sim/Makefile.
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
        pytest \
        clean clean-cmodel clean-verilator clean-vcs clean-generated

help:
	@echo "Build (from repo root):"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only -> build/cmodel/"
	@echo "  make build-verilator  Verilator binaries -> build/verilator/"
	@echo ""
	@echo "Simulate (runs from sim/):"
	@echo "  make -C sim TB=<topo> PATTERN=<p> [SEED=<n>]"
	@echo "  make -C sim help      every simulation variable"
	@echo ""
	@echo "Test:"
	@echo "  make test             run c_model ctest suite"
	@echo ""
	@echo "Clean:"
	@echo "  make clean                  everything (build/ + per-sim output/ + generated stimulus)"
	@echo "  make clean-cmodel           build/cmodel/"
	@echo "  make clean-verilator        build/verilator/ + sim/verilator/output/"
	@echo "  make clean-vcs              build/vcs/ + sim/vcs/output/ + Verdi droppings"
	@echo "  make clean-generated        generated SV/stimulus + __pycache__"

# --- build ---

build: build-cmodel build-verilator

# CMake configure runs only when CMakeCache.txt is missing (first time or after
# clean-cmodel). Subsequent `make build-cmodel` is pure `cmake --build`, which
# avoids reconfigure triggering ninja to re-run side-effect custom targets
# (e.g. codegen_check) under a different subprocess env.

# Per-host overrides (gitignored). Lets a machine pin CMAKE / DEPS_SRC / etc.
# once so the command line stays identical everywhere. Optional — the
# auto-detection below covers the common distribution layouts with no file.
#
# Per-host WSL config: create a gitignored `local.mk` at the repo root with:
#   BUILD_ROOT := $(HOME)/noc_build   # native-Linux build dir (WSL rejects /mnt COFF)
#   PYTHON3    := python3
#   VERILATOR  := verilator
# Then `make -C sim TB=tb_mesh_4x4_vc1 PATTERN=hotspot` needs no path/tool args
# either: sim/build_config.mk reads this same file through PROJ_ROOT.
-include local.mk

# CMake binary — auto-detected so the same `make build` works on every host.
# Prefer `cmake3` when present (RHEL ships the modern 3.x under that name, while
# bare `cmake` may be an ancient one shadowed onto PATH by e.g. a vendor tool install);
# otherwise fall through to `cmake` resolved at recipe time. Override in
# local.mk or on the command line if neither is right. Build needs cmake >= 3.20
# (FetchContent_MakeAvailable + gtest 1.14).
CMAKE ?= $(shell command -v cmake3 2>/dev/null || echo cmake)

# Extra cmake configure flags (escape hatch for host quirks). Common need:
# pin the Python interpreter when an EDA tool puts a broken
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

# Bounded, not a bare -j. Unlimited parallelism starts one compiler per core --
# 28 on the WSL host against 15 GB of RAM -- and the gtest translation units are
# template-heavy enough that the machine goes to swap and g++ dies with an
# internal compiler error. That is the whole story behind the "host GCC ICE"
# this file used to route around. Override with JOBS= for a bigger machine.
JOBS ?= 6

build-cmodel: $(CMODEL_BUILD)/CMakeCache.txt
	@$(CMAKE) --build $(CMODEL_BUILD) -j $(JOBS)

# Sim needs only the yaml-cpp static lib (+ c_model/yaml-cpp headers via -I).
# Build that one target, not the whole c_model tree: the sim flow has no use for
# the unit-test executables.
build-yamlcpp: $(CMODEL_BUILD)/CMakeCache.txt
	@$(CMAKE) --build $(CMODEL_BUILD) --target yaml-cpp -j $(JOBS)

$(CMODEL_BUILD)/CMakeCache.txt:
	@$(CMAKE) -S $(CMODEL_DIR) -B $(CMODEL_BUILD) $(CMAKE_DEPS_FLAGS) $(CMAKE_EXTRA)

# Default topology for standalone build-verilator.
# sim/Makefile overrides this by passing TOPOLOGY=$(TB) explicitly.
TOPOLOGY  ?= mesh_4x4_vc1
RUN_CLASS ?= directed

build-verilator: build-yamlcpp
	@$(MAKE) -C $(COSIM_VERILATOR) TOPOLOGY=$(TOPOLOGY) RUN_CLASS=$(RUN_CLASS)

# --- test ---

test: build-cmodel
	@cd $(CMODEL_BUILD) && ctest --output-on-failure

PYTHON3 ?= python3

# Python suites: specgen (codegen/golden drift gate -- a stale golden, e.g. an
# un-regenerated SV package, must not pass silently) and sim/tools (the stimulus
# and testbench generators, including the Python half of the C++/Python address
# packing agreement). Both run in one target; each suite imports from its own
# directory, hence the two cd's. Both ALWAYS run -- the exit status is
# accumulated rather than chained, so a red specgen suite cannot hide whether
# sim/tools passed.
pytest:
	@status=0; \
	(cd specgen && $(PYTHON3) -m pytest tests/ -q) || status=1; \
	(cd sim/tools && $(PYTHON3) -m pytest . -q) || status=1; \
	exit $$status

# Simulation runs from sim/, not here. `sim` is also a directory name, so
# without these two make would answer the old command with "'sim' is up to
# date" and run nothing — a silent no-op is worse than an error.
.PHONY: sim sim-injection-sweep
sim sim-injection-sweep:
	@echo "Simulation moved to sim/. Use: make -C sim $(if $(filter sim-injection-sweep,$@),sim-injection-sweep )TB=<topo> PATTERN=<p>" >&2
	@echo "  make -C sim help   for every variable" >&2
	@false

# --- clean ---

# After clean the tree must hold nothing but what git tracks, apart from the
# three files that are deliberately per-host and gitignored: local.mk,
# docs/backlog.md and .vscode/. Anything else surviving is a bug in here.
#
# The generated SV matters most. tb_top_<topo>.sv, noc_fabric_<topo>.sv and
# filelist_<topo>.f used to survive clean, which is why every co-sim script
# carried its own `rm -f` preamble: a stale filelist holding paths from another
# host, or a tb_top from another topology, silently builds the wrong thing.
clean: clean-cmodel clean-verilator clean-vcs clean-generated
	rm -rf $(BUILD_ROOT)
	rm -f master_wrap_read_dump*.txt

clean-cmodel:
	rm -rf $(CMODEL_BUILD)
	rm -rf Testing

# Generated sources and stimulus, plus every __pycache__ the generators leave.
clean-generated:
	rm -f sim/tb/tb_top_*.sv src/sv/noc_fabric_*.sv sim/filelist_*.f
	rm -rf sim/test_patterns
	find . -type d -name __pycache__ -prune -exec rm -rf {} +
	find . -type d -name .pytest_cache -prune -exec rm -rf {} +

clean-verilator:
	$(MAKE) -C $(COSIM_VERILATOR) clean

clean-vcs:
	$(MAKE) -C sim/vcs clean

