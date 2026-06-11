# Top-level Makefile. Run all targets from repo root.
#
# Convention: top-level only orchestrates; each subdir Makefile owns its own
# build + clean. Recursive `$(MAKE) -C` keeps the responsibility split clean.

CMODEL_DIR      := c_model
CMODEL_BUILD    := $(CMODEL_DIR)/build
COSIM_VERILATOR := cosim/verilator
SCENARIO_TREE   := tests/scenarios
SIM_OUTPUT_DIR  := cosim/output
SCENARIO        ?= AX4-BAS-003_single_write_read_aligned

.PHONY: help build build-cmodel build-verilator sim sim-genamba test check lint_scenarios lint_docs \
        clean clean-cmodel clean-verilator clean-specgen-cache

help:
	@echo "Build:"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only"
	@echo "  make build-verilator  Verilator co-sim binary (needs c_model first)"
	@echo ""
	@echo "Run/test:"
	@echo "  make sim                                       run AX4-BAS-003_single_write_read_aligned"
	@echo "  make sim SCENARIO=<ax4-id>                     run tests/scenarios/<ax4-id>/"
	@echo "      e.g. make sim SCENARIO=AX4-BUR-002_incr_8beat"
	@echo "  make sim-genamba                               build+run gen_amba role-1 testbench (Tasks A-G)"
	@echo "  make sim-genamba GENAMBA_SCENARIO=<ax4-id>     override default scenario"
	@echo "  make test                                      run c_model ctest suite"
	@echo ""
	@echo "Clean:"
	@echo "  make clean                  everything"
	@echo "  make clean-cmodel           c_model/build/"
	@echo "  make clean-verilator        cosim/verilator/obj_dir/ + cosim/output/"
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

# --- run / test ---

# Run Vtb_top with cwd at $(COSIM_VERILATOR) so the in-project read-dump file
# (master_shell_read_dump.txt) lands there, then move it into
# $(SIM_OUTPUT_DIR)/$(SCENARIO)/ alongside run.log. The +scenario= flag is an
# absolute path under tests/scenarios/$(SCENARIO)/scenario.yaml;
# scenario_parser resolves data_file: relative to the YAML's own dir so cwd
# doesn't matter for input resolution.
SIM_RUN_DIR := $(SIM_OUTPUT_DIR)/$(SCENARIO)
SIM_RUN_ABS := $(CURDIR)/$(SIM_RUN_DIR)
SCENARIO_ABS := $(abspath $(SCENARIO_TREE)/$(SCENARIO)/scenario.yaml)

sim: build-verilator
	@mkdir -p $(SIM_RUN_DIR)
	@echo "running scenario $(SCENARIO); output -> $(SIM_RUN_DIR)/"
	@cd $(COSIM_VERILATOR) && \
	    ./obj_dir/Vtb_top "+scenario=$(SCENARIO_ABS)" \
	    > $(SIM_RUN_ABS)/run.log 2>&1; \
	rc=$$?; \
	if [ -f master_shell_read_dump.txt ]; then \
	    mv master_shell_read_dump.txt $(SIM_RUN_ABS)/; \
	fi; \
	echo "--- run.log (tail) ---"; \
	tail -8 $(SIM_RUN_ABS)/run.log; \
	echo "--- artifacts ---"; \
	ls $(SIM_RUN_ABS)/; \
	exit $$rc

# gen_amba role-1 testbench: build+run delegated to cosim/verilator/Makefile.
# GENAMBA_SCENARIO override is forwarded; defaults to AX4-BAS-001 in the
# delegate. The genamba target is independent of build-verilator (its own
# objdir, top, source list) and does not depend on c_model build.
#
# On Windows the sub-make must find MSYS2 mingw64/bin (verilator + perl)
# and usr/bin (make + g++) on PATH; prepend in the recipe so this target
# works from any Git Bash login (matches run_genamba.sh's prepend). The
# prepended paths are harmless on Linux / macOS (they simply don't exist).
# LC_ALL silences the "Locale not supported" warning from MSYS2 perl on
# zh_TW. GNU make on MSYS2 does not inherit $(OS) from env, so the prefix
# is unconditional rather than guarded.
GENAMBA_SCENARIO ?=
# yaml-cpp header check: cmodel_dpi.cpp pulls in headers from
# c_model/build/_deps/yaml-cpp-src/include, populated by CMake FetchContent.
# Don't auto-depend on build-cmodel here: a full c_model build is slow and
# the yaml-cpp headers are stable once configured. Surface a clear hint
# if missing instead.
YAMLCPP_LIB := $(CMODEL_BUILD)/_deps/yaml-cpp-build/libyaml-cpp.a
sim-genamba:
	@if [ ! -f "$(YAMLCPP_LIB)" ]; then \
	    echo "ERROR: yaml-cpp static lib missing ($(YAMLCPP_LIB))."; \
	    echo "Run \`make build-cmodel\` once to populate c_model/build/_deps."; \
	    exit 1; \
	fi
	@$(TOOLPATH) $(MAKE) -C $(COSIM_VERILATOR) run-genamba \
	    $(if $(GENAMBA_SCENARIO),GENAMBA_SCENARIO=$(GENAMBA_SCENARIO),)

test: build-cmodel
	@$(TOOLPATH) sh -c 'cd $(CMODEL_BUILD) && ctest --output-on-failure'

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
	@$(TOOLPATH) sh -c 'cd $(CMODEL_BUILD) && ctest --output-on-failure'

# --- clean ---

clean: clean-cmodel clean-verilator clean-specgen-cache

clean-cmodel:
	rm -rf $(CMODEL_BUILD)

clean-verilator:
	$(MAKE) -C $(COSIM_VERILATOR) clean
	rm -rf $(SIM_OUTPUT_DIR)

clean-specgen-cache:
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +
