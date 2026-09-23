.DEFAULT_GOAL := help
SHELL := /bin/bash
.NOTPARALLEL:
ROOT := $(abspath ../../..)
RTL_STAGE ?= $(ROOT)/sim/standalone/nmu/output/i8_n5_b128_r1/stage
CASE ?= ctrl_write_single
OUT := $(ROOT)/build/nmu-cosim/stage
.PHONY: help prepare lint sync compile run sim regress run_wave
help:
	@echo 'make prepare | make sync; on workstation: make run CASE=ctrl_write_single'
prepare:
	python3 prepare.py --rtl-stage "$(RTL_STAGE)" --out "$(OUT)"
lint: prepare
	cd "$(OUT)" && verilator --lint-only --timing --assert -Wno-fatal -f files.f --top-module tb_nmu_cosim
sync: prepare
	python3 "$(ROOT)/sim/tools/sync_nmu_workstation.py" --source "$(OUT)" --remote-dir /home/mingwei/noc_project/nmu-cosim

compile run sim regress run_wave corrupt nWave: prepare
	$(MAKE) -C "$(OUT)" $@ CASE=$(CASE)
