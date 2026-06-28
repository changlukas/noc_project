# Top-level Makefile — BUILD ONLY. Run all targets from repo root.
#
# Convention: the root builds (c_model + Verilator) and runs lint/test
# gates; SIMULATION runs from each simulator's own directory:
#   cd sim/verilator && make run-tb-top   (Windows + Linux)
#   cd sim/vcs       && make run-tb-top   (Linux workstation)
# Run logs land in sim/<sim>/output/<scenario>/run.log.
#
# All build artifacts live under the top-level build/ tree (gitignored):
#   build/cmodel/    CMake (c_model tests + FetchContent deps)
#   build/verilator/ obj_dir (tb_top)
#   build/vcs/       simv_* + csrc_* (workstation)

CMODEL_DIR      := c_model
BUILD_ROOT      := build
CMODEL_BUILD    := $(BUILD_ROOT)/cmodel
COSIM_VERILATOR := sim/verilator
COSIM_VCS       := sim/vcs

.PHONY: help build build-cmodel build-verilator test check lint_scenarios lint_docs \
        specgen_pytest sim sim-regress \
        clean clean-cmodel clean-verilator clean-vcs clean-specgen-cache

help:
	@echo "Build (from repo root):"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only -> build/cmodel/"
	@echo "  make build-verilator  Verilator binaries -> build/verilator/"
	@echo ""
	@echo "Simulate (from each simulator's directory):"
	@echo "  cd sim/verilator && make run-tb-top                   default scenario"
	@echo "  cd sim/verilator && make run-tb-top SCENARIO=<ax4-id> specific scenario"
	@echo "  cd sim/vcs       && make run-tb-top                   VCS (Linux workstation)"
	@echo ""
	@echo "Simulate:"
	@echo "  make sim TB=<topo> PATTERN=<p>            build + run benchmark (default TB=mesh_4x4_vc1)"
	@echo "  make sim TB=mesh_4x4_vc8 PATTERN=neighbor PYTHON3=python3"
	@echo "  Vars: TXN= SEED= HOTSPOT= BASE=<base.yaml>"
	@echo "  make sim-regress [BUILD=<build>]    run the co-sim regression (one build, or all)"
	@echo ""
	@echo "Test:"
	@echo "  make test             run c_model ctest suite"
	@echo "  make check            lint + build + full ctest + neighbor smoke"
	@echo ""
	@echo "Clean:"
	@echo "  make clean                  everything (build/ + per-sim output/)"
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

$(CMODEL_BUILD)/CMakeCache.txt:
	@$(TOOLPATH) $(CMAKE) -S $(CMODEL_DIR) -B $(CMODEL_BUILD) $(CMAKE_DEPS_FLAGS) $(CMAKE_EXTRA)

# Default topology for standalone build-verilator / make check.
# make sim overrides this by passing TOPOLOGY=$(TB) explicitly.
TOPOLOGY ?= mesh_4x4_vc1

build-verilator: build-cmodel
	@$(TOOLPATH) $(MAKE) -C $(COSIM_VERILATOR) TOPOLOGY=$(TOPOLOGY)

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

lint_scenarios:
	$(PYTHON3) tools/lint_scenarios.py --require-nonempty

# Mandatory ASCII byte check on maintained docs (spec sec 3.2).
# Excludes archive, normative spec, sub-project docs, legal, generated.
MAINTAINED_DOCS = \
    README.md \
    docs/architecture.md \
    docs/development.md \
    docs/internal/_archive/README.md \
    sim/test_patterns/README.md

lint_docs:
	$(PYTHON3) tools/lint_docs.py $(MAINTAINED_DOCS)

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

check: lint_scenarios lint_docs specgen_pytest build-cmodel build-verilator
	@$(TOOLPATH) sh -c '$(CTEST_CMD)'
	$(PYTHON3) sim/tools/run_benchmark.py --topology mesh_4x4_vc1 --pattern neighbor

# Unified sim target. TB selects topology (forwarded to build-verilator explicitly
# so non-default topologies like mesh_4x4_vc8 are built correctly).
# PATTERN, TXN, SEED, HOTSPOT, BASE are optional forwarded vars.
TB      ?= mesh_4x4_vc1
PATTERN ?= neighbor
BUILD   ?=

sim:
	$(MAKE) build-verilator TOPOLOGY=$(TB) PYTHON3=$(PYTHON3)
	$(PYTHON3) sim/tools/run_benchmark.py --topology $(TB) --pattern $(PATTERN) \
	  $(if $(TXN),--transactions-per-node $(TXN)) $(if $(SEED),--seed $(SEED)) \
	  $(if $(HOTSPOT),--hotspot $(HOTSPOT)) $(if $(BASE),--from $(BASE))

sim-regress:
	$(TOOLPATH) $(PYTHON3) sim/regress/run_regress.py $(if $(BUILD),--build $(BUILD))

# --- clean ---

clean: clean-cmodel clean-verilator clean-vcs clean-specgen-cache
	rm -rf $(BUILD_ROOT)

clean-cmodel:
	rm -rf $(CMODEL_BUILD)

clean-verilator:
	$(MAKE) -C $(COSIM_VERILATOR) clean

clean-vcs:
	$(MAKE) -C sim/vcs clean

clean-specgen-cache:
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +
