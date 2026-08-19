# Top-level Makefile — build and test gates. Run these targets from repo root.
#
# Simulation is not here: `make sim CONFIG=... PATTERN=...`, see
# sim/verilator/Makefile.
# Run logs land in sim/verilator/output/<scenario>/run.log.
#
# All build artifacts live under the top-level build/ tree (gitignored):
#   build/cmodel/    CMake (c_model tests + FetchContent deps)
#   build/verilator/ obj_dir (tb_top)
#   build/vcs/       simv_* + csrc_* (workstation)

CMODEL_DIR      := ref_model/c_model
BUILD_ROOT      := build
# Recursive (=) not immediate (:=): BUILD_ROOT can be overridden by `-include
# local.mk` below (e.g. $(HOME)/noc_build on WSL), and CMODEL_BUILD must pick up
# that override rather than freezing the pre-include `build` value.
CMODEL_BUILD     = $(BUILD_ROOT)/cmodel
SIM_VERILATOR := sim/verilator
SIM_VCS       := sim/vcs

.PHONY: help build build-cmodel build-yamlcpp build-verilator test rtl-common-lint rtl-common-test \
        pytest check docker-build docker-shell docker-test docker-pytest docker-sim-setup docker-sim-smoke docker-sim-tier2 \
        clean clean-cmodel clean-verilator clean-vcs clean-generated

help:
	@echo "Build (from repo root):"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only -> build/cmodel/"
	@echo "  make build-verilator  Verilator binaries -> build/verilator/"
	@echo ""
	@echo "Simulate:"
	@echo "  make sim-gen CONFIG=<config> PATTERN=<p>         stimulus only; a run needs this first"
	@echo "  make sim CONFIG=<config> PATTERN=<p> [SEED=<n>]  build, then run"
	@echo "  make sim-build CONFIG=<config>                   build only, no run"
	@echo "  make sim-run CONFIG=<config> PATTERN=<p>         run only; errors if not built"
	@echo "  CONFIG is a sim/configs/*.yml basename: mesh_2x2 mesh_2x2_periph mesh_4x4 mesh_4x4_periph4"
	@echo "  make -C sim/verilator help                       every simulation variable"
	@echo ""
	@echo "Test:"
	@echo "  make test             run c_model ctest suite"
	@echo "  make rtl-common-lint  lint the production common primitive adapters"
	@echo "  make rtl-common-test  lint and behavior-test the common primitive adapters"
	@echo "  make pytest           specgen + sim/tools suites, golden drift gate"
	@echo "  make check            both of the above -- run this before committing"
	@echo ""
	@echo "Docker:"
	@echo "  make docker-build     build noc-dev Docker image"
	@echo "  make docker-shell     shell in noc-dev image with this repo mounted"
	@echo "  make docker-test      run full c_model ctest suite inside noc-dev image"
	@echo "  make docker-pytest    run specgen + sim/tools pytest suites inside noc-dev image"
	@echo "  make docker-sim-setup prepare sim-only deps inside noc-dev image, ctest bypassed"
	@echo "  make docker-sim-smoke run 2x2 verify inside noc-dev image, ctest bypassed"
	@echo "  make docker-sim-tier2 legacy alias for docker-sim-smoke (2x2 verify)"
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
# Then `make sim CONFIG=mesh_4x4 PATTERN=hotspot` needs no path/tool args
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
# internal compiler error. Docker/WSL also produced corrupt test executables at
# JOBS=2 on this host. Override only on a Linux host verified under parallel load.
JOBS ?= 1

build-cmodel: $(CMODEL_BUILD)/CMakeCache.txt
	@$(CMAKE) --build $(CMODEL_BUILD) -j $(JOBS)

# Sim needs only the yaml-cpp static lib (+ c_model/yaml-cpp headers via -I).
# Build that one target, not the whole c_model tree: the sim flow has no use for
# the unit-test executables.
build-yamlcpp: $(CMODEL_BUILD)/CMakeCache.txt
	@$(CMAKE) --build $(CMODEL_BUILD) --target yaml-cpp -j $(JOBS)

$(CMODEL_BUILD)/CMakeCache.txt:
	@$(CMAKE) -S $(CMODEL_DIR) -B $(CMODEL_BUILD) $(CMAKE_DEPS_FLAGS) $(CMAKE_EXTRA)

# Default configuration for standalone build-verilator. `build` and not the
# default goal: sim/verilator/Makefile defaults to running.
CONFIG    ?= mesh_4x4
RUN_CLASS ?= directed

build-verilator: build-yamlcpp
	@$(MAKE) -C $(SIM_VERILATOR) build CONFIG=$(CONFIG) RUN_CLASS=$(RUN_CLASS)

# --- test ---

test: build-cmodel
	@cd $(CMODEL_BUILD) && ctest --output-on-failure

rtl-common-lint:
	@bash rtl/common/test.sh lint

rtl-common-test:
	@bash rtl/common/test.sh test

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
	git add --refresh -- specgen/generated || status=1; \
	exit $$status

# Run before committing. The SAM parity proof lives half in each suite: ctest
# holds nmu/sam_yaml.hpp to sim/configs/sam_rules.golden and pytest holds
# sim/tools/address_map.py to the same file, so the two readers are only
# compared when both have run. A change to a helper they share (address_map.py's
# dst_id(), say) moves both sides of the Python-side cross-format check
# together, leaves pytest green, and shows up in ctest alone.
check: test pytest

# --- docker ---

DOCKER ?= docker
DOCKER_IMAGE ?= noc-dev:verilator-5.048
DOCKER_BUILD_VOLUME ?= noc-dev-build-cache
DOCKER_SIM_VOLUME ?= noc-dev-sim-cache
DOCKER_CCACHE_VOLUME ?= noc-dev-ccache
DOCKER_SIM_BUILD_ROOT ?= /home/agent/noc_sim_build
DOCKER_PROJECT_DIR ?= $(shell cygpath -m "$(CURDIR)" 2>/dev/null || printf '%s\n' "$(CURDIR)")
DOCKER_NO_PATHCONV ?= MSYS_NO_PATHCONV=1
DOCKER_RUN = $(DOCKER_NO_PATHCONV) $(DOCKER) run --rm -v "$(DOCKER_PROJECT_DIR):/workspace" -v "$(DOCKER_BUILD_VOLUME):/home/agent/noc_build" -v "$(DOCKER_CCACHE_VOLUME):/home/agent/.cache/ccache" -w /workspace $(DOCKER_IMAGE)
DOCKER_SIM_RUN = $(DOCKER_NO_PATHCONV) $(DOCKER) run --rm -v "$(DOCKER_PROJECT_DIR):/workspace" -v "$(DOCKER_SIM_VOLUME):$(DOCKER_SIM_BUILD_ROOT)" -v "$(DOCKER_CCACHE_VOLUME):/home/agent/.cache/ccache" -w /workspace -e BUILD_ROOT=$(DOCKER_SIM_BUILD_ROOT) $(DOCKER_IMAGE)

docker-build:
	$(DOCKER) build -f docker/noc-dev/Dockerfile -t $(DOCKER_IMAGE) .

docker-shell:
	$(DOCKER_NO_PATHCONV) $(DOCKER) run --rm -it -v "$(DOCKER_PROJECT_DIR):/workspace" -v "$(DOCKER_BUILD_VOLUME):/home/agent/noc_build" -v "$(DOCKER_CCACHE_VOLUME):/home/agent/.cache/ccache" -w /workspace $(DOCKER_IMAGE) bash

docker-test:
	$(DOCKER_RUN) bash -lc 'make test'

docker-pytest:
	$(DOCKER_RUN) bash -lc 'make pytest'

docker-sim-setup:
	$(DOCKER_SIM_RUN) bash -lc 'make build-yamlcpp BUILD_ROOT=$$BUILD_ROOT CMAKE_EXTRA=-DBUILD_TESTING=OFF'

docker-sim-smoke:
	$(DOCKER_SIM_RUN) bash -lc 'set -euo pipefail; make build-yamlcpp BUILD_ROOT=$$BUILD_ROOT CMAKE_EXTRA=-DBUILD_TESTING=OFF; rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv; rm -rf "$$BUILD_ROOT"/verilator/obj_dir_*; make sim BUILD_ROOT=$$BUILD_ROOT CONFIG=mesh_2x2 PATTERN=neighbor'

docker-sim-tier2: docker-sim-smoke

# Simulation runs from sim/verilator, whose Makefile reaches output/ by relative
# path, so these forward with -C rather than include. CONFIG, PATTERN and the
# rest reach the sub-make on their own: make passes command-line variables down
# through MAKEFLAGS. .PHONY is what stops `sim` matching the directory of the
# same name and answering "'sim' is up to date" without running anything.
# The VCS flow will land beside these as sim-vcs-* against sim/vcs.
.PHONY: sim sim-build sim-run sim-gen sim-injection-sweep
sim sim-injection-sweep:
	@$(MAKE) -C $(SIM_VERILATOR) $@

sim-build:
	@$(MAKE) -C $(SIM_VERILATOR) build

sim-run:
	@$(MAKE) -C $(SIM_VERILATOR) run

sim-gen:
	@$(MAKE) -C $(SIM_VERILATOR) gen

# --- clean ---

# After clean the tree must hold nothing but what git tracks, apart from the
# three files that are deliberately per-host and gitignored: local.mk,
# docs/backlog.md and .vscode/. Anything else surviving is a bug in here.
#
# The generated SV matters most. tb_top_<topo>.sv and
# filelist_<topo>.f used to survive clean, which is why every co-sim script
# carried its own `rm -f` preamble: a stale filelist holding paths from another
# host, or a tb_top from another topology, silently builds the wrong thing.
clean: clean-cmodel clean-verilator clean-vcs clean-generated
	rm -rf $(BUILD_ROOT)
	rm -f master_wrap_read_dump*.txt
	rm -f core core.*

clean-cmodel:
	rm -rf $(CMODEL_BUILD)
	rm -rf Testing

# Generated sources and stimulus, plus every __pycache__ the generators leave.
clean-generated:
	rm -f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv sim/filelist_*.f
	rm -f sim/tb/test/topology_pkg.sv
	rm -f sim/tools/injection_sweep.csv sim/tools/injection_sweep.png
	rm -f sim/verilator/hs_trace_node*.log
	find . -type d -name __pycache__ -prune -exec rm -rf {} +
	find . -type d -name .pytest_cache -prune -exec rm -rf {} +

clean-verilator:
	$(MAKE) -C $(SIM_VERILATOR) clean

clean-vcs:
	$(MAKE) -C $(SIM_VCS) clean
