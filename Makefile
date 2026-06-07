# Top-level Makefile. Run all targets from repo root.
#
# Convention: top-level only orchestrates; each subdir Makefile owns its own
# build + clean. Recursive `$(MAKE) -C` keeps the responsibility split clean.

CMODEL_DIR      := c_model
CMODEL_BUILD    := $(CMODEL_DIR)/build
COSIM_VERILATOR := cosim2/verilator
SCENARIO        ?= debug_multi1

.PHONY: help build build-cmodel build-verilator sim test \
        clean clean-cmodel clean-verilator clean-specgen-cache

help:
	@echo "Build:"
	@echo "  make build            c_model + Verilator (correct dep order)"
	@echo "  make build-cmodel     c_model only"
	@echo "  make build-verilator  Verilator co-sim binary (needs c_model first)"
	@echo ""
	@echo "Run/test:"
	@echo "  make sim                          run Vtb_top on debug_multi1.yaml"
	@echo "  make sim SCENARIO=<name>          run a different fixture (no .yaml suffix)"
	@echo "  make test                         run c_model ctest suite"
	@echo ""
	@echo "Clean:"
	@echo "  make clean                  everything"
	@echo "  make clean-cmodel           c_model/build/"
	@echo "  make clean-verilator        cosim2/verilator/obj_dir/ + its dump"
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

sim: build-verilator
	cd $(COSIM_VERILATOR) && ./obj_dir/Vtb_top "+scenario=../tests/fixtures/$(SCENARIO).yaml"

test: build-cmodel
	cd $(CMODEL_BUILD) && ctest --output-on-failure

# --- clean ---

clean: clean-cmodel clean-verilator clean-specgen-cache

clean-cmodel:
	rm -rf $(CMODEL_BUILD)

clean-verilator:
	$(MAKE) -C $(COSIM_VERILATOR) clean

clean-specgen-cache:
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +
