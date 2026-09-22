# One source/config/pattern snapshot; only simulator and waveform backends differ.
.DEFAULT_GOAL := run
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.NOTPARALLEL:
script_dir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
package_dir := $(abspath $(script_dir)/..)
include $(script_dir)/config.mk
SIMULATOR ?= vcs
CASE ?= ctrl_write_single
VCS ?= vcs
VERILATOR ?= verilator
NWAVE ?= nWave
WAVE_RC ?= $(script_dir)/nWaveLog/signals.rc
VERDI_HOME ?= /cadtools/synopsys/verdi/M-2017.03-SP1
PLI_DIR ?= $(VERDI_HOME)/share/PLI/VCS/linux64
# Keep the caller environment intact; extend only child-process library lookup.
export LD_LIBRARY_PATH := $(PLI_DIR)$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
export VCS_ARCH_OVERRIDE ?= linux
VCS_EXTRA ?=
VERILATOR_EXTRA ?=
SIM_EXTRA ?=
top := tb_nmu_standalone
run_dir ?= $(package_dir)/build/$(SIMULATOR)_i$(ID_WIDTH)_n$(NOC_HALF_PERIOD)_b$(BUFFER_DEPTH)_r$(READ_ROB_ENABLED)_wave$(WAVE)
run_dir := $(abspath $(run_dir))
# Generated compilation files must use the workstation's local clock, not NFS mtime.
vcs_cache_root := /tmp/noc-vcs-$(shell id -u)-$(shell cd "$(package_dir)" && pwd -P | tr -d '\n' | cksum | cut -d' ' -f1)
vcs_work_dir := $(vcs_cache_root)/i$(ID_WIDTH)_n$(NOC_HALF_PERIOD)_b$(BUFFER_DEPTH)_r$(READ_ROB_ENABLED)_wave$(WAVE)
report_dir := $(run_dir)/report
wave_dir := $(run_dir)/waves
filelist := $(package_dir)/files.f
wave_ext := fsdb
ifeq ($(SIMULATOR),verilator)
wave_ext := fst
endif
case_label := $(if $(CASE),$(CASE),$(PATTERN))
wave_file ?= $(wave_dir)/$(case_label).$(wave_ext)

VCS_FLAGS := -full64 -sverilog -assert svaext -override_timescale=1ns/1ps -debug_access+all \
    +lint=TFIPC-L +lint=PCWM -Mdir=$(vcs_work_dir)/csrc -f $(filelist) -top $(top) \
    -pvalue+$(top).ID_WIDTH=$(ID_WIDTH) \
    -pvalue+$(top).NOC_HALF_PERIOD=$(NOC_HALF_PERIOD) \
    -pvalue+$(top).BUFFER_DEPTH=$(BUFFER_DEPTH) \
    -pvalue+$(top).READ_ROB_ENABLED=$(READ_ROB_ENABLED) -o $(vcs_work_dir)/simv
VERILATOR_FLAGS := --binary --timing --assert -j 1 -Wno-fatal \
    -Werror-WIDTHEXPAND -Werror-WIDTHTRUNC -Werror-LATCH \
    $(package_dir)/repo/rtl/nmu/top/nmu_lint.vlt \
    --top-module $(top) -f $(filelist) --Mdir $(run_dir)/csrc -o $(run_dir)/simv \
    -GID_WIDTH=$(ID_WIDTH) -GNOC_HALF_PERIOD=$(NOC_HALF_PERIOD) \
    -GBUFFER_DEPTH=$(BUFFER_DEPTH) -GREAD_ROB_ENABLED=$(READ_ROB_ENABLED)
ifeq ($(WAVE),1)
VCS_FLAGS += +define+DUMP_WAVE -P $(PLI_DIR)/novas.tab $(PLI_DIR)/pli.a
VERILATOR_FLAGS += +define+DUMP_WAVE --trace-fst
endif

.PHONY: help sanity_check compile run sim regress block_regress legacy_regress run_wave run_wave_view nWave view fault report clean
help:
	@printf '%s\n' \
	 'make run CASE=ctrl_write_single     Compile and run (VCS default)' \
	 'make sim CASE=ctrl_read_burst       Reuse existing executable' \
	 'make regress                       Compile once, all standalone cases and fault check' \
	 'make legacy_regress                Retained topology traffic and mixed tests' \
	 'make run_wave CASE=same_id_cross_dst_reorder' \
	 'make nWave CASE=same_id_cross_dst_reorder  (load signal groups)' \
	 'make run_wave_view CASE=ctrl_write_single (run then open nWave)' \
	 'make clean                         Remove all build/wave/log/GUI artifacts; retain signal RC files' \
	 'make regress SIMULATOR=verilator    Same sources, cases and configuration' \
	 'Shared overrides: ID_WIDTH, NOC_HALF_PERIOD, BUFFER_DEPTH, READ_ROB_ENABLED' \
	 'Patterns are generated for a specific ID_WIDTH; synchronize matching inputs before changing it.'

sanity_check:
	@if [[ -n "$(CASE)" ]]; then grep -Fxq "$(CASE)" "$(package_dir)/cases/standalone/cases.list" || { echo "Unknown CASE" >&2; exit 1; }; fi
	@test -f "$(filelist)" || { echo 'Use the synchronized simulation directory (files.f missing)' >&2; exit 1; }
	@case "$(SIMULATOR)" in vcs|verilator) ;; *) echo 'SIMULATOR must be vcs or verilator' >&2; exit 1;; esac
	@case "$(PATTERN)" in neighbor|uniform_random|hotspot|directed) ;; *) echo 'Invalid PATTERN' >&2; exit 1;; esac
	@case "$(WAVE)" in 0|1) ;; *) echo 'WAVE must be 0 or 1' >&2; exit 1;; esac
	@mkdir -p "$(report_dir)" "$(wave_dir)"

compile: sanity_check
	@printf '%s\n' 'SIMULATOR=$(SIMULATOR)' 'WAVE=$(WAVE)' 'ID_WIDTH=$(ID_WIDTH)' \
	 'NOC_HALF_PERIOD=$(NOC_HALF_PERIOD)' 'BUFFER_DEPTH=$(BUFFER_DEPTH)' \
	 'READ_ROB_ENABLED=$(READ_ROB_ENABLED)' 'VCS_FLAGS=$(VCS_FLAGS)' \
	 'VERILATOR_FLAGS=$(VERILATOR_FLAGS)' 'VCS_EXTRA=$(VCS_EXTRA)' \
	 'VERILATOR_EXTRA=$(VERILATOR_EXTRA)' > "$(report_dir)/build-config.txt"
	@cp "$(package_dir)/SHA256SUMS" "$(report_dir)/source-SHA256SUMS"
ifeq ($(SIMULATOR),vcs)
	@command -v "$(VCS)" >/dev/null || { echo 'Initialize the workstation VCS environment first' >&2; exit 1; }
ifeq ($(WAVE),1)
	@test -r "$(PLI_DIR)/novas.tab" -a -r "$(PLI_DIR)/pli.a" || { echo 'Set VERDI_HOME or PLI_DIR to the installed Verdi PLI directory' >&2; exit 1; }
endif
	@mkdir -m 700 "$(vcs_cache_root)" 2>/dev/null || \
	  [[ -d "$(vcs_cache_root)" && ! -L "$(vcs_cache_root)" && -O "$(vcs_cache_root)" ]]
	@test ! -L "$(vcs_work_dir)"
	@mkdir -p "$(vcs_work_dir)"
	cd "$(package_dir)" && $(VCS) $(VCS_FLAGS) $(VCS_EXTRA) -l "$(report_dir)/compile.log"
	@cp "$(vcs_work_dir)/simv" "$(run_dir)/simv"
	@mkdir -p "$(run_dir)/simv.daidir"
	@cp -a "$(vcs_work_dir)/simv.daidir/." "$(run_dir)/simv.daidir/"
else
	cd "$(package_dir)" && $(VERILATOR) $(VERILATOR_FLAGS) $(VERILATOR_EXTRA) 2>&1 | tee "$(report_dir)/compile.log"
endif

run: compile sim

sim: sanity_check
	@test -x "$(run_dir)/simv" || { echo 'No binary for this configuration. Run: make run CASE=$(CASE)'  >&2; exit 1; }
	@args=(); stim="$(package_dir)/cases/$(PATTERN)"; if [[ -n "$(CASE)" ]]; then \
	  stim="$(package_dir)/cases/standalone/$(CASE)"; mapfile -t args < "$$stim/schedule.txt"; \
	elif [[ "$(PATTERN)" == directed ]]; then \
	  args+=(+require_reorder); \
	  if [[ $(BUFFER_DEPTH) == 8 && $(READ_ROB_ENABLED) == 1 ]]; then args+=(+require_pressure); fi; \
	fi; cd "$(package_dir)"; "$(run_dir)/simv" \
	  +stim_dir="$$stim" +run_dir="$(run_dir)" \
	  +wave_file="$(wave_file)" "$${args[@]}" $(SIM_EXTRA) \
	  2>&1 | tee "$(report_dir)/$(case_label).log"
	@grep -q 'PASS NMU standalone' "$(report_dir)/$(case_label).log"
	@echo 'PASS: $(case_label)'

block_regress regress: compile
	@while read -r name; do \
	  $(MAKE) --no-print-directory -f "$(script_dir)/Makefile" sim CASE=$$name run_dir="$(run_dir)"; \
	done < "$(package_dir)/cases/standalone/cases.list"
	@$(MAKE) --no-print-directory -f "$(script_dir)/Makefile" fault run_dir="$(run_dir)"

legacy_regress: compile
	@for pattern in $(patterns); do \
	  $(MAKE) --no-print-directory -f "$(script_dir)/Makefile" sim CASE= PATTERN=$$pattern run_dir="$(run_dir)"; \
	done
	@$(MAKE) --no-print-directory -f "$(script_dir)/Makefile" fault run_dir="$(run_dir)"
	@$(MAKE) --no-print-directory -f "$(script_dir)/Makefile" report run_dir="$(run_dir)"

# Some simulators return zero after $fatal; require the expected checker diagnostic.
fault: sanity_check
	@test -x "$(run_dir)/simv"
	@cd "$(package_dir)"; "$(run_dir)/simv" \
	  +stim_dir="$(package_dir)/cases/directed" +corrupt_rsp \
	  +wave_file="$(wave_dir)/corrupt.$(wave_ext)" 2>&1 | tee "$(report_dir)/corrupt.log" || :
	@grep -q 'R data/lane/order/last mismatch' "$(report_dir)/corrupt.log"
	@echo 'PASS: expected corruption was detected'

run_wave:
	@$(MAKE) --no-print-directory -f "$(script_dir)/Makefile" run WAVE=1 PATTERN=$(PATTERN) CASE=$(CASE)

run_wave_view: run_wave nWave

nWave:
	@$(MAKE) --no-print-directory -f "$(script_dir)/Makefile" view WAVE=1 SIMULATOR=vcs PATTERN=$(PATTERN) CASE=$(CASE)

view:
	@test -f "$(wave_file)" || { echo 'Run make run_wave first' >&2; exit 1; }
	@test -f "$(WAVE_RC)" || { echo 'Waveform RC template missing: $(WAVE_RC)' >&2; exit 1; }
	@sed 's|@FSDB@|$(wave_file)|g' "$(WAVE_RC)" > "$(run_dir)/$(case_label).rc"
	@cd "$(script_dir)" && $(NWAVE) -ssf "$(wave_file)" -sswr "$(run_dir)/$(case_label).rc" &

report:
	@test -d "$(report_dir)" || { echo 'No report directory yet' >&2; exit 1; }
	tar -czf "$(package_dir)/nmu-$(SIMULATOR)-results.tar.gz" -C "$(run_dir)" report
	@echo 'Reports: $(package_dir)/nmu-$(SIMULATOR)-results.tar.gz'

clean:
	@bash "$(script_dir)/clean.sh"
