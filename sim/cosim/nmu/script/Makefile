.DEFAULT_GOAL := help
SHELL := /bin/bash
.NOTPARALLEL:
ROOT := $(CURDIR)
CASE ?= ctrl_write_single
WAVE ?= 0
MODE ?= auto
RSP_DELAY ?= 2
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
HEADERS := $(shell find repo/ref_model repo/specgen/generated/cpp deps/yaml-cpp/include -name '*.h' -o -name '*.hpp')
YAML_HEADERS := $(shell find deps/yaml-cpp/include deps/yaml-cpp/src -name '*.h' -o -name '*.hpp')
SV_SRC := $(shell find repo deps -name '*.sv' -o -name '*.svh') topology_pkg.sv
ifeq ($(WAVE),1)
WAVE_FLAGS := +define+DUMP_WAVE -P $(PLI_DIR)/novas.tab $(PLI_DIR)/pli.a
endif
VCS_FLAGS := -full64 -sverilog -assert svaext -override_timescale=1ns/1ps -debug_access+all -f files.f -top tb_nmu_cosim -pvalue+tb_nmu_cosim.RSP_DELAY_CYCLES=$(RSP_DELAY)
DPI := $(BUILD)/dpi_$(shell python3 build_key.py cpp "$(CPPFLAGS) $(CXX)")
YAML_BUILD := $(BUILD)/yaml_$(shell python3 build_key.py yaml "$(CPPFLAGS) $(CXX)")
SIM := $(BUILD)/vcs_wave$(WAVE)_$(shell python3 build_key.py sv "$(VCS_FLAGS) $(WAVE_FLAGS) $(DPI)")
YAML_OBJ := $(patsubst deps/yaml-cpp/src/%.cpp,$(YAML_BUILD)/%.o,$(YAML_SRC))
.PHONY: help compile run sim regress run_wave corrupt nWave
help:
	@echo 'make run CASE=ctrl_write_single | make regress | make run_wave CASE=data_write_burst'
$(YAML_BUILD)/%.o: deps/yaml-cpp/src/%.cpp $(YAML_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -c $< -o $@
$(DPI)/cmodel_dpi.o: repo/ref_model/dpi/cmodel_dpi.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) -c $< -o $@
$(DPI)/libnmu_cmodel.so: $(DPI)/cmodel_dpi.o $(YAML_OBJ)
	$(CXX) -shared -static-libstdc++ -static-libgcc -Wl,-Bsymbolic -o $@ $^ -pthread
$(SIM)/simv: $(SV_SRC) files.f $(DPI)/libnmu_cmodel.so Makefile
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
