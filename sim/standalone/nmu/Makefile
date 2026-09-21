# Run inside the repository's Linux development environment.
.DEFAULT_GOAL := help
SHELL := /bin/bash
.NOTPARALLEL:
root := $(abspath ../../..)
include config.mk
output := $(CURDIR)/output/i$(ID_WIDTH)_n$(NOC_HALF_PERIOD)_b$(BUFFER_DEPTH)_r$(READ_ROB_ENABLED)
stage := $(output)/stage
SIMULATOR ?= verilator
CASE ?= ctrl_write_single
.PHONY: help prepare compile run sim regress legacy_regress run_wave run_wave_view nWave
help:
	@echo 'make run CASE=ctrl_write_single SIMULATOR=verilator'
	@echo 'make regress  (all standalone cases; compile once)'
	@echo 'make sim CASE=ctrl_read_burst  (reuse binary)'
	@echo 'make legacy_regress  (retained traffic/mixed regressions)'
prepare:
	NMU_TEST_OUTPUT="$(output)" NMU_ID_WIDTH=$(ID_WIDTH) NMU_NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) NMU_BUFFER_DEPTH=$(BUFFER_DEPTH) NMU_READ_ROB=$(READ_ROB_ENABLED) bash "$(root)/rtl/nmu/top/test_nmu.sh" prepare
compile run run_wave run_wave_view nWave: prepare
	$(MAKE) -C "$(stage)/script" $@ SIMULATOR=$(SIMULATOR) CASE=$(CASE) ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) READ_ROB_ENABLED=$(READ_ROB_ENABLED)
sim:
	$(MAKE) -C "$(stage)/script" sim SIMULATOR=$(SIMULATOR) CASE=$(CASE) ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) READ_ROB_ENABLED=$(READ_ROB_ENABLED)
regress: prepare
	$(MAKE) -C "$(stage)/script" block_regress SIMULATOR=$(SIMULATOR) ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) READ_ROB_ENABLED=$(READ_ROB_ENABLED)
legacy_regress: prepare
	$(MAKE) -C "$(stage)/script" legacy_regress SIMULATOR=$(SIMULATOR) CASE= ID_WIDTH=$(ID_WIDTH) NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) BUFFER_DEPTH=$(BUFFER_DEPTH) READ_ROB_ENABLED=$(READ_ROB_ENABLED)
