VERILATOR     := verilator
TOP           := tb_top
PROJ_ROOT     := $(realpath ../..)
COSIM_ROOT    := $(realpath ..)
CMODEL_INC    := $(PROJ_ROOT)/c_model/include
CMODEL_TESTS  := $(PROJ_ROOT)/c_model/tests
SPECGEN_INC   := $(PROJ_ROOT)/specgen/generated/cpp
SPECGEN_SV_INC := $(PROJ_ROOT)/specgen/generated/sv
YAMLCPP_INC   := $(PROJ_ROOT)/c_model/build/_deps/yaml-cpp-src/include
YAMLCPP_LIB   := $(PROJ_ROOT)/c_model/build/_deps/yaml-cpp-build/libyaml-cpp.a

SV_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(COSIM_ROOT)/sv/axi_master_wrap.sv \
    $(COSIM_ROOT)/sv/nmu_wrap.sv \
    $(COSIM_ROOT)/sv/loopback_noc_wrap.sv \
    $(COSIM_ROOT)/sv/nsu_wrap.sv \
    $(COSIM_ROOT)/sv/axi_slave_wrap.sv \
    $(COSIM_ROOT)/sv/wb2axip/faxi_wstrb.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_master.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_slave.v \
    $(COSIM_ROOT)/sv/tb_top.sv

C_SRC := \
    $(COSIM_ROOT)/c/cmodel_dpi.cpp \
    $(COSIM_ROOT)/verilator/main.cpp

# c_model is header-only: no -l flag needed; include paths suffice.
# yaml-cpp is a static archive built by CMake FetchContent; link via full path.
VERILATOR_FLAGS := \
    --cc --exe \
    --top-module $(TOP) \
    --no-timing \
    --assert \
    -I$(COSIM_ROOT)/sv \
    -I$(COSIM_ROOT)/sv/wb2axip \
    -I$(SPECGEN_SV_INC) \
    -CFLAGS "-std=c++17" \
    -CFLAGS "-DYAML_CPP_STATIC_DEFINE" \
    -CFLAGS "-g" \
    -CFLAGS "-O0" \
    -CFLAGS "-I$(COSIM_ROOT)/c" \
    -CFLAGS "-I$(CMODEL_INC)" \
    -CFLAGS "-I$(CMODEL_TESTS)" \
    -CFLAGS "-I$(SPECGEN_INC)" \
    -CFLAGS "-I$(YAMLCPP_INC)" \
    -CFLAGS "-Wno-attributes" \
    -LDFLAGS "$(YAMLCPP_LIB)" \
    +define+assume=assert \
    -Wno-fatal \
    -Wno-NULLPORT \
    -Wno-REDEFMACRO \
    -Wno-WIDTHEXPAND \
    -Wno-WIDTHTRUNC

# Optional strict-mode flag injection for release gates.
# Override with: make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic"
VERILATOR_EXTRA_FLAGS ?=
VERILATOR_FLAGS += $(VERILATOR_EXTRA_FLAGS)

all: obj_dir/V$(TOP)

# Python3 executable.  Prefer the MSYS2/system python3 if it resolves to a
# real interpreter; fall back to the full Windows CPython path on this host.
PYTHON3 ?= /c/Users/USER/AppData/Local/Programs/Python/Python313/python.exe

# Two-phase build: Verilator generates C++ in obj_dir, then make compiles it.
# PYTHON3 override: verilator_includer needs a real Python3 interpreter.
# LDFLAGS_EXTRA: set PE stack reserve to 64 MiB so MinGW pthread initialises
#   correctly on Windows (avoids STATUS_STACK_OVERFLOW 0xc00000fd).
obj_dir/V$(TOP): $(SV_SRC) $(C_SRC)
	$(VERILATOR) $(VERILATOR_FLAGS) $(SV_SRC) $(C_SRC)
	$(MAKE) -C obj_dir -f V$(TOP).mk \
	    PYTHON3="$(PYTHON3)" \
	    "USER_LDFLAGS=-Wl,--stack,67108864"

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
