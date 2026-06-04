VERILATOR     := verilator
TOP           := tb_axi_conformity
PROJ_ROOT     := $(realpath ../..)
COSIM_ROOT    := $(realpath ..)
CMODEL_INC    := $(PROJ_ROOT)/c_model/include
SPECGEN_INC   := $(PROJ_ROOT)/specgen/generated/cpp
YAMLCPP_INC   := $(PROJ_ROOT)/c_model/build/_deps/yaml-cpp-src/include
YAMLCPP_LIB   := $(PROJ_ROOT)/c_model/build/_deps/yaml-cpp-build/libyaml-cpp.a

SV_SRC := \
    $(COSIM_ROOT)/sv/axi_if.sv \
    $(COSIM_ROOT)/sv/nmu_cmodel_proxy.sv \
    $(COSIM_ROOT)/sv/nsu_cmodel_proxy.sv \
    $(COSIM_ROOT)/sv/wb2axip/faxi_master.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_slave.v \
    $(COSIM_ROOT)/sv/tb_axi_conformity.sv

C_SRC := \
    $(COSIM_ROOT)/c/axi_dpi.cpp \
    $(COSIM_ROOT)/verilator/main.cpp

# c_model is header-only: no -l flag needed; include paths suffice.
# yaml-cpp is a static archive built by CMake FetchContent; link via full path.
VERILATOR_FLAGS := \
    --cc --exe --build \
    --top-module $(TOP) \
    -I $(COSIM_ROOT)/sv \
    -I $(COSIM_ROOT)/sv/wb2axip \
    -CFLAGS "-std=c++17 -I$(COSIM_ROOT)/c -I$(CMODEL_INC) -I$(SPECGEN_INC) -I$(YAMLCPP_INC)" \
    -LDFLAGS "$(YAMLCPP_LIB)" \
    +define+assume=assert \
    -Wno-fatal

all: obj_dir/V$(TOP)

obj_dir/V$(TOP): $(SV_SRC) $(C_SRC)
	$(VERILATOR) $(VERILATOR_FLAGS) $(SV_SRC) $(C_SRC)

# Smoke-test the Verilator toolchain with a trivial module before attempting
# the full co-sim build.  Expected output: "TOOLCHAIN OK"
hello:
	echo "module empty_tb; initial \$$finish; endmodule" > /tmp/empty_tb.sv
	$(VERILATOR) --cc --exe --build --top-module empty_tb \
	    -CFLAGS "-std=c++17" /tmp/empty_tb.sv \
	    --Mdir /tmp/empty_tb_obj
	/tmp/empty_tb_obj/Vempty_tb && echo "TOOLCHAIN OK"

clean:
	rm -rf obj_dir
