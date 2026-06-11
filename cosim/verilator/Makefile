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
    $(COSIM_ROOT)/sv/channel_model_wrap.sv \
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
# Flags shared by every Verilator target in this Makefile (tb_top, genamba).
VERILATOR_COMMON_FLAGS := \
    --cc --exe \
    --assert \
    -I$(COSIM_ROOT)/sv \
    -I$(SPECGEN_SV_INC) \
    -CFLAGS "-std=c++17" \
    -CFLAGS "-DYAML_CPP_STATIC_DEFINE" \
    -CFLAGS "-I$(COSIM_ROOT)/c" \
    -CFLAGS "-I$(CMODEL_INC)" \
    -CFLAGS "-I$(CMODEL_TESTS)" \
    -CFLAGS "-I$(SPECGEN_INC)" \
    -CFLAGS "-I$(YAMLCPP_INC)" \
    -LDFLAGS "$(YAMLCPP_LIB)" \
    +define+assume=assert \
    -Wno-fatal \
    -Wno-NULLPORT \
    -Wno-REDEFMACRO \
    -Wno-WIDTHEXPAND \
    -Wno-WIDTHTRUNC \
    -CFLAGS "-Wno-attributes"
# -Wno-attributes (g++): verilator's own svdpi.h triggers "'dllimport'
# attribute ignored" under MinGW for every DPI-bearing translation unit —
# toolchain header noise, not project code.

VERILATOR_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --top-module $(TOP) \
    --no-timing \
    -I$(COSIM_ROOT)/sv/wb2axip \
    -CFLAGS "-g" \
    -CFLAGS "-O0"

# Optional strict-mode flag injection for release gates.
# Override with: make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic"
VERILATOR_EXTRA_FLAGS ?=
VERILATOR_FLAGS += $(VERILATOR_EXTRA_FLAGS)

# Linked binary name is platform-dependent: MinGW ld appends .exe, Linux
# does not. Naming the real artifact makes the source-dependency check
# fire correctly on both (a phony or wrong-name target rebuilds always).
EXEEXT := $(if $(findstring NT,$(shell uname -s)),.exe,)
TBTOP_EXE := obj_dir/V$(TOP)$(EXEEXT)

all: $(TBTOP_EXE)

# Python3 executable.  On Windows with MSYS2 use `py -3`; on Linux/macOS
# `python3` is standard.  Override at the command line if needed:
#   make build-verilator PYTHON3=py
PYTHON3 ?= python3

# Two-phase build: Verilator generates C++ in obj_dir, then make compiles it.
# PYTHON3 override: verilator_includer needs a real Python3 interpreter.
# LDFLAGS_EXTRA: set PE stack reserve to 64 MiB so MinGW pthread initialises
#   correctly on Windows (avoids STATUS_STACK_OVERFLOW 0xc00000fd).
$(TBTOP_EXE): $(SV_SRC) $(C_SRC)
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

clean: clean-genamba
	rm -rf obj_dir
	rm -f master_shell_read_dump.txt

# --- gen_amba role-1 testbench (Phase 1) ---
GENAMBA_OBJDIR := obj_genamba
GENAMBA_SRC := \
    $(SPECGEN_SV_INC)/ni_params_pkg.sv \
    $(SPECGEN_SV_INC)/ni_signals_pkg.sv \
    $(COSIM_ROOT)/sv/nmu_wrap.sv \
    $(COSIM_ROOT)/sv/nsu_wrap.sv \
    $(COSIM_ROOT)/sv/genamba/mem_axi.v \
    $(COSIM_ROOT)/sv/genamba_master_bfm.sv \
    $(COSIM_ROOT)/sv/tb_genamba.sv
GENAMBA_C_SRC := \
    $(COSIM_ROOT)/c/cmodel_dpi.cpp \
    $(COSIM_ROOT)/verilator/main_genamba.cpp
GENAMBA_FLAGS := \
    $(VERILATOR_COMMON_FLAGS) \
    --timing \
    --top-module tb_genamba \
    --Mdir $(GENAMBA_OBJDIR) \
    --output-split 0 \
    -I$(COSIM_ROOT)/sv/genamba \
    +define+AMBA_AXI4 +define+AMBA_QOS \
    +define+AMBA_AXI_CACHE +define+AMBA_AXI_PROT

# Strict-mode flag injection applies to genamba builds too (mirrors
# VERILATOR_FLAGS above).
GENAMBA_FLAGS += $(VERILATOR_EXTRA_FLAGS)

# Two-phase build mirrors tb_top pattern: Verilator generates C++ in
# $(GENAMBA_OBJDIR), then $(MAKE) compiles it.  --timing is used instead of
# --no-timing so the SV $time-based $display calls are accurate.
# --output-split 0: Verilator 5.036's default 20000-statement split can
# separate a coroutine's `__Vfork_N__sync` declaration from its use across
# generated .cpp chunks ("was not declared in this scope" C++ error) when
# fork-bearing tasks grow. Disabling the split keeps each coroutine whole;
# compile-time cost is acceptable at this design size.
# -LDFLAGS "-Wl,--stack,N" is intentionally omitted: Verilator 5.036 on
# Windows/MSYS2 escapes commas in generated .mk, breaking the link step.
#
# Incremental: the exe depends on SV + C sources, so an unchanged tree
# skips both Verilation and the C++ compile on repeat `make run-genamba`.
# `genamba` stays as the friendly alias. $(EXEEXT) defined near `all:`.
GENAMBA_EXE := $(GENAMBA_OBJDIR)/Vtb_genamba$(EXEEXT)
# sed pass between Verilation and C++ compile: Verilator 5.036 on Windows
# joins include-resolved source paths with backslashes and embeds them in
# generated C-string literals (fork-sync done() debug info). `\g` etc. are
# invalid escapes (2300+ g++ warnings) and `\t` silently corrupts to TAB.
# Rewrite only backslashes that follow a drive-letter path prefix —
# legitimate \n escapes in $display format strings never match.
$(GENAMBA_EXE): $(GENAMBA_SRC) $(GENAMBA_C_SRC)
	$(VERILATOR) $(GENAMBA_FLAGS) $(GENAMBA_SRC) $(GENAMBA_C_SRC)
	sed -i -E ':a; s|([A-Za-z]:/[A-Za-z0-9_./]*)\\([A-Za-z0-9_])|\1/\2|; ta' \
	    $(GENAMBA_OBJDIR)/*.cpp $(GENAMBA_OBJDIR)/*.h
	$(MAKE) -C $(GENAMBA_OBJDIR) -f Vtb_genamba.mk \
	    PYTHON3="$(PYTHON3)"

.PHONY: genamba
genamba: $(GENAMBA_EXE)

# Build + run in one shot. SCENARIO override follows the top-level Makefile
# convention: pass a scenario directory name under tests/scenarios/.
GENAMBA_SCENARIO ?= AX4-BAS-001_single_write_no_read
GENAMBA_SCENARIO_ABS := $(PROJ_ROOT)/tests/scenarios/$(GENAMBA_SCENARIO)/scenario.yaml
.PHONY: run-genamba
run-genamba: genamba
	./$(GENAMBA_EXE) "+scenario=$(GENAMBA_SCENARIO_ABS)"

.PHONY: clean-genamba
clean-genamba:
	rm -rf $(GENAMBA_OBJDIR)
