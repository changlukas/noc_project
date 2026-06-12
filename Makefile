# Top-level Makefile — BUILD ONLY. Run all targets from repo root.
#
# Convention: the root builds (c_model + Verilator) and runs lint/test
# gates; SIMULATION runs from each simulator's own directory:
#   cd cosim/verilator && make run-genamba / run-tb-top   (Windows + Linux)
#   cd cosim/vcs       && make run-genamba / run-tb-top   (Linux workstation)
# Run logs land in cosim/<sim>/output/<scenario>/run.log.
#
# All build artifacts live under the top-level build/ tree (gitignored):
#   build/cmodel/    CMake (c_model tests + FetchContent deps)
#   build/verilator/ obj_dir (tb_top) + obj_genamba
#   build/vcs/       simv_* + csrc_* (workstation)

CMODEL_DIR      := c_model
BUILD_ROOT      := build
CMODEL_BUILD    := $(BUILD_ROOT)/cmodel
COSIM_VERILATOR := cosim/verilator
COSIM_VCS       := cosim/vcs

.PHONY: help build build-cmodel build-verilator test check lint_scenarios lint_docs \
        clean clean-cmodel clean-verilator clean-specgen-cache

help:
	@echo "Build (from repo root):"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only -> build/cmodel/"
	@echo "  make build-verilator  Verilator binaries -> build/verilator/"
	@echo ""
	@echo "Simulate (from each simulator's directory):"
	@echo "  cd cosim/verilator && make run-genamba                  gen_amba role-1 (Tasks A-G)"
	@echo "  cd cosim/verilator && make run-tb-top                   wb2axip cosim, default scenario"
	@echo "  cd cosim/verilator && make run-tb-top SCENARIO=<ax4-id> specific scenario"
	@echo "  cd cosim/vcs       && make run-genamba / run-tb-top     VCS (Linux workstation)"
	@echo ""
	@echo "Test:"
	@echo "  make test             run c_model ctest suite"
	@echo "  make check            lint + build + full ctest"
	@echo ""
	@echo "Clean:"
	@echo "  make clean                  everything (build/ + per-sim output/)"
	@echo "  make clean-cmodel           build/cmodel/"
	@echo "  make clean-verilator        build/verilator/ + cosim/verilator/output/"
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

build-cmodel: $(CMODEL_BUILD)/CMakeCache.txt
	@$(TOOLPATH) cmake --build $(CMODEL_BUILD) -j

$(CMODEL_BUILD)/CMakeCache.txt:
	@$(TOOLPATH) cmake -S $(CMODEL_DIR) -B $(CMODEL_BUILD)

build-verilator: build-cmodel
	@$(TOOLPATH) $(MAKE) -C $(COSIM_VERILATOR)

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
    docs/_archive/README.md \
    tests/scenarios/README.md

lint_docs:
	$(PYTHON3) tools/lint_docs.py $(MAINTAINED_DOCS)

check: lint_scenarios lint_docs build-cmodel build-verilator
	@$(TOOLPATH) sh -c '$(CTEST_CMD)'

# --- clean ---

clean: clean-cmodel clean-verilator clean-specgen-cache
	rm -rf $(BUILD_ROOT)

clean-cmodel:
	rm -rf $(CMODEL_BUILD)

clean-verilator:
	$(MAKE) -C $(COSIM_VERILATOR) clean

clean-specgen-cache:
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +
