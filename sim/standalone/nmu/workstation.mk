.DEFAULT_GOAL := help
TESTBENCH ?= standalone
ifeq ($(TESTBENCH),cosim)
.PHONY: help compile run sim regress run_wave run_wave_view nWave corrupt list
help compile run sim regress run_wave run_wave_view nWave corrupt:
	$(MAKE) --no-print-directory -C cosim $@
list:
	@cat cosim/pattern.txt
else ifeq ($(TESTBENCH),standalone)
.PHONY: help compile run sim regress block_regress legacy_regress run_wave run_wave_view nWave view fault report clean dat_regress in_order_perf out_of_order_perf mixed_perf list
help compile run sim regress block_regress legacy_regress run_wave run_wave_view nWave view fault report clean dat_regress in_order_perf out_of_order_perf mixed_perf:
	$(MAKE) --no-print-directory -C script $@
list:
	@cat pattern_list.txt
else
$(error TESTBENCH must be standalone or cosim)
endif
