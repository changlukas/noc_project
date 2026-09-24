.DEFAULT_GOAL := help
SHELL := /bin/bash
.NOTPARALLEL:
ROOT := $(CURDIR)
CASE ?= ctrl_write_single
WAVE ?= 0
MODE ?= auto
RSP_DELAY ?= 2
REG_TYPE ?= 0
IO_FIFO_DEPTH ?= 32
-include profile.mk
AXI_ID_WIDTH ?= $(shell python3 -c 'import re; print(re.search(r"AXI_ID_WIDTH\s*=\s*(\d+)", open("repo/specgen/generated/sv/ni_params_pkg.sv").read()).group(1))')
MAX_ACTIVE_IDS ?= $(shell python3 -c 'import re; print(1 << min(int("$(AXI_ID_WIDTH)"), int(re.search(r"NOC_ID_WIDTH\s*=\s*(\d+)", open("repo/specgen/generated/sv/ni_params_pkg.sv").read()).group(1))))')
MAX_OUTSTANDING_PER_ID ?= 32
WAVE_RC ?= $(ROOT)/signals.rc
CXX := /cadtools/centos/spe21.1/tools.lnx86/cdsgcc/gcc/9.3/bin/g++
VCS ?= vcs
VCS_HOME ?= /home/mingwei/.toolsmgr/vcs
VERDI_HOME ?= /cadtools/synopsys/verdi/M-2017.03-SP1
PLI_DIR ?= $(VERDI_HOME)/share/PLI/VCS/linux64
export LD_LIBRARY_PATH := $(PLI_DIR)$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
export VCS_ARCH_OVERRIDE ?= linux
BUILD := $(ROOT)/build
REPORT := $(BUILD)/report_wave$(WAVE)$(if $(filter-out auto,$(MODE)),/$(MODE),)
INCLUDES := -Irepo/ref_model/dpi -Irepo/ref_model/c_model/include -Irepo/ref_model/c_model/tests/common -Irepo/specgen/generated/cpp -Ideps/yaml-cpp/include -Ideps/yaml-cpp/src -I$(VCS_HOME)/include
CPPFLAGS := -std=c++17 -O1 -fPIC -DYAML_CPP_STATIC_DEFINE $(INCLUDES)
YAML_SRC := $(wildcard deps/yaml-cpp/src/*.cpp)
YAML_HEADERS := $(shell find deps/yaml-cpp/include deps/yaml-cpp/src -name '*.h' -o -name '*.hpp')
ifeq ($(WAVE),1)
WAVE_FLAGS := +define+DUMP_WAVE -P $(PLI_DIR)/novas.tab $(PLI_DIR)/pli.a
endif
VCS_FLAGS := -full64 -sverilog -assert svaext -override_timescale=1ns/1ps -debug_access+all -f files.f -top tb_nmu_cosim -pvalue+tb_nmu_cosim.RSP_DELAY_CYCLES=$(RSP_DELAY) -pvalue+tb_nmu_cosim.OUTPUT_REG_TYPE=$(REG_TYPE) -pvalue+tb_nmu_cosim.IO_FIFO_DEPTH=$(IO_FIFO_DEPTH) -pvalue+tb_nmu_cosim.AXI_ID_WIDTH=$(AXI_ID_WIDTH) -pvalue+tb_nmu_cosim.MAX_ACTIVE_IDS=$(MAX_ACTIVE_IDS) -pvalue+tb_nmu_cosim.MAX_OUTSTANDING_PER_ID=$(MAX_OUTSTANDING_PER_ID)
DPI := $(BUILD)/dpi_$(shell python3 build_key.py cpp "$(CPPFLAGS) $(CXX)")
YAML_BUILD := $(BUILD)/yaml_$(shell python3 build_key.py yaml "$(CPPFLAGS) $(CXX)")
SIM := $(BUILD)/vcs_wave$(WAVE)_$(shell python3 build_key.py sv "$(VCS_FLAGS) $(WAVE_FLAGS) $(DPI)")
YAML_OBJ := $(patsubst deps/yaml-cpp/src/%.cpp,$(YAML_BUILD)/%.o,$(YAML_SRC))
.PHONY: help compile run sim regress run_wave run_wave_view corrupt nWave
help:
	@echo 'make run CASE=ctrl_write_single | make regress | make run_wave CASE=data_write_burst'
$(YAML_BUILD)/%.o: deps/yaml-cpp/src/%.cpp $(YAML_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -c $< -o $@
# Project sources are already in the directory hash. Their synchronized mtimes
# must not invalidate a previously compiled profile. Vendor headers retain the
# normal dependency because the DPI key does not hash them.
$(DPI)/cmodel_dpi.o: $(YAML_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -c repo/ref_model/dpi/cmodel_dpi.cpp -o $@
$(DPI)/libnmu_cmodel.so: $(DPI)/cmodel_dpi.o $(YAML_OBJ)
	$(CXX) -shared -static-libstdc++ -static-libgcc -Wl,-Bsymbolic -o $@ $^ -pthread
$(SIM)/simv: $(DPI)/libnmu_cmodel.so
	@mkdir -p $(SIM) $(REPORT)
	@cp SHA256SUMS $(REPORT)/source-SHA256SUMS
	$(VCS) $(VCS_FLAGS) -Mdir=$(SIM)/csrc -o $@ -LDFLAGS "-L$(DPI) -Wl,-rpath,$(DPI) -lnmu_cmodel" $(WAVE_FLAGS) -l $(REPORT)/compile.log
compile: $(SIM)/simv
run: compile sim
sim:
	python3 run.py --binary "$(SIM)/simv" --case "$(CASE)" --mode "$(MODE)" --report "$(REPORT)" $(if $(filter 1,$(WAVE)),--wave,)
regress: compile
	@set -e; while read name; do $(MAKE) --no-print-directory sim CASE=$$name; done < patterns/cases.list
corrupt: compile
	python3 run.py --binary "$(SIM)/simv" --case data_write_single --report "$(REPORT)" --corrupt
run_wave_view: nWave

nWave: run_wave
	@sed 's|@FSDB@|$(BUILD)/report_wave1/$(if $(filter-out auto,$(MODE)),$(MODE)/,)$(CASE).fsdb|g' "$(WAVE_RC)" > "$(BUILD)/report_wave1/$(if $(filter-out auto,$(MODE)),$(MODE)/,)$(CASE).rc"
	$(VERDI_HOME)/bin/nWave -ssf "$(BUILD)/report_wave1/$(if $(filter-out auto,$(MODE)),$(MODE)/,)$(CASE).fsdb" -sswr "$(BUILD)/report_wave1/$(if $(filter-out auto,$(MODE)),$(MODE)/,)$(CASE).rc"
run_wave:
	$(MAKE) run WAVE=1 CASE=$(CASE)

.PHONY: print-build-dirs
print-build-dirs:
	@echo $(DPI)
	@echo $(YAML_BUILD)
	@echo $(SIM)
