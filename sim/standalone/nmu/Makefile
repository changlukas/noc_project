# Run inside the repository's Linux development environment.
.DEFAULT_GOAL := help
SHELL := /bin/bash
.NOTPARALLEL:
root := $(abspath ../../..)
include config.mk
output := $(CURDIR)/output/i$(ID_WIDTH)_n$(NOC_HALF_PERIOD)_b$(BUFFER_DEPTH)_r$(R_ROB_EN)
stage := $(output)/stage
SIMULATOR ?= verilator
CASE ?= ctrl_write_single
.PHONY: help prepare compile run sim regress legacy_regress run_wave run_wave_view nWave clean
help:
	@echo 'make run CASE=ctrl_write_single SIMULATOR=verilator'
	@echo 'make regress  (all standalone cases; compile once)'
	@echo 'make sim CASE=same_id_outstanding MODE=rand SEED=7  (reuse binary)'
	@echo 'make legacy_regress  (retained traffic/mixed regressions)'
prepare:
	NMU_TEST_OUTPUT="$(output)" NMU_ID_WIDTH=$(ID_WIDTH) NMU_NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) NMU_BUFFER_DEPTH=$(BUFFER_DEPTH) NMU_READ_ROB=$(R_ROB_EN) bash "$(root)/rtl/nmu/top/test_nmu.sh" prepare
compile run run_wave run_wave_view nWave: prepare
	$(MAKE) -C "$(stage)/script" $@ SIMULATOR=$(SIMULATOR) CASE=$(CASE) ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) R_ROB_EN=$(R_ROB_EN)
sim:
	$(MAKE) -C "$(stage)/script" sim SIMULATOR=$(SIMULATOR) CASE=$(CASE) ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) R_ROB_EN=$(R_ROB_EN)
regress: prepare
	$(MAKE) -C "$(stage)/script" block_regress SIMULATOR=$(SIMULATOR) ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) R_ROB_EN=$(R_ROB_EN)
legacy_regress: prepare
	$(MAKE) -C "$(stage)/script" legacy_regress SIMULATOR=$(SIMULATOR) CASE= ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) R_ROB_EN=$(R_ROB_EN)

# Clean every prepared profile without regenerating sources or patterns.
clean:
	@set -e; for task_stage in "$(CURDIR)"/output/*/stage; do \
	  if [[ -f "$$task_stage/files.f" ]]; then \
	    cp "$(root)/sim/standalone/common/clean.sh" "$$task_stage/script/clean.sh"; \
	    bash "$$task_stage/script/clean.sh"; \
	  fi; \
	done

.PHONY: dat_regress
dat_regress: prepare
	$(MAKE) -C "$(stage)/script" dat_regress SIMULATOR=$(SIMULATOR)

.PHONY: in_order_perf
in_order_perf: prepare
	$(MAKE) -C "$(stage)/script" in_order_perf SIMULATOR=$(SIMULATOR)

.PHONY: out_of_order_perf
out_of_order_perf: prepare
	$(MAKE) -C "$(stage)/script" out_of_order_perf SIMULATOR=$(SIMULATOR)
