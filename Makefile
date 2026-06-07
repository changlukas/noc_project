# Top-level Makefile. Run all targets from repo root.
#
# Convention: top-level only orchestrates; each subdir Makefile owns its own
# build + clean. Recursive `$(MAKE) -C` keeps the responsibility split clean.

CMODEL_DIR      := c_model
CMODEL_BUILD    := $(CMODEL_DIR)/build
COSIM_VERILATOR := cosim/verilator
SCENARIO_TREE   := tests/scenarios
SIM_OUTPUT_DIR  := cosim/output
LAYER           ?= common
SCENARIO        ?= single_write_read_aligned

.PHONY: help build build-cmodel build-verilator sim test \
        clean clean-cmodel clean-verilator clean-specgen-cache

help:
	@echo "Build:"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only"
	@echo "  make build-verilator  Verilator co-sim binary (needs c_model first)"
	@echo ""
	@echo "Run/test:"
	@echo "  make sim                                       run common/single_write_read_aligned"
	@echo "  make sim SCENARIO=<name>                       run common/<name>"
	@echo "  make sim SCENARIO=<name> LAYER=<sub-dir>       run <sub-dir>/<name>"
	@echo "      LAYER ∈ {common, sv-cosim-only, cpp-adapter-only, c-model-only}"
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
build-cmodel: $(CMODEL_BUILD)/CMakeCache.txt
	cmake --build $(CMODEL_BUILD) -j

$(CMODEL_BUILD)/CMakeCache.txt:
	cmake -S $(CMODEL_DIR) -B $(CMODEL_BUILD)

build-verilator: build-cmodel
	$(MAKE) -C $(COSIM_VERILATOR)

# --- run / test ---

# Run Vtb_top with cwd at $(COSIM_VERILATOR) so the in-project read-dump file
# (master_shell_read_dump.txt) lands there, then move it into
# $(SIM_OUTPUT_DIR)/$(SCENARIO)/ alongside run.log. The +scenario= flag is an
# absolute path under tests/scenarios/$(LAYER)/$(SCENARIO)/scenario.yaml;
# scenario_parser resolves data_file: relative to the YAML's own dir so cwd
# doesn't matter for input resolution.
SIM_RUN_DIR := $(SIM_OUTPUT_DIR)/$(SCENARIO)
SIM_RUN_ABS := $(CURDIR)/$(SIM_RUN_DIR)
SCENARIO_ABS := $(abspath $(SCENARIO_TREE)/$(LAYER)/$(SCENARIO)/scenario.yaml)

sim: build-verilator
	@mkdir -p $(SIM_RUN_DIR)
	@echo "running scenario $(LAYER)/$(SCENARIO); output -> $(SIM_RUN_DIR)/"
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

test: build-cmodel
	cd $(CMODEL_BUILD) && ctest --output-on-failure

# --- clean ---

clean: clean-cmodel clean-verilator clean-specgen-cache

clean-cmodel:
	rm -rf $(CMODEL_BUILD)

clean-verilator:
	$(MAKE) -C $(COSIM_VERILATOR) clean
	rm -rf $(SIM_OUTPUT_DIR)

clean-specgen-cache:
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +
