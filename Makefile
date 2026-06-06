# Top-level orchestration. Run all targets from repo root.
#
# Clean targets:
#   make clean                clean everything (c_model + verilator + specgen + dumps)
#   make clean-cmodel         clean c_model/build/ only (CMake, ~230 MB)
#   make clean-verilator      clean cosim2/verilator/obj_dir/ + its dump (~22 MB)
#   make clean-specgen-cache  clean specgen __pycache__/ dirs
#
# Build (informational — Makefile does not wrap them):
#   cmake -S c_model -B c_model/build && cmake --build c_model/build
#       Build c_model headers + GoogleTest harness. Run from repo root.
#   (cd cosim2/verilator && make)
#       Build the Verilator co-sim binary obj_dir/Vtb_top.
#
# Preserves: all source files + checked-in regen output under specgen/generated/.

.PHONY: clean clean-cmodel clean-verilator clean-specgen-cache help

clean: clean-cmodel clean-verilator clean-specgen-cache
	rm -f master_shell_read_dump.txt
	@echo "clean done."

clean-cmodel:
	rm -rf c_model/build

clean-verilator:
	rm -rf cosim2/verilator/obj_dir
	rm -f cosim2/verilator/master_shell_read_dump.txt

clean-specgen-cache:
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +

help:
	@echo "Targets (run from repo root):"
	@echo "  make clean              clean everything"
	@echo "  make clean-cmodel       clean c_model/build/ only"
	@echo "  make clean-verilator    clean cosim2/verilator/obj_dir/ + its dump"
	@echo "  make clean-specgen-cache clean specgen __pycache__/ dirs"
	@echo "  make help               this message"
	@echo ""
	@echo "Build (not wrapped here — run directly):"
	@echo "  cmake -S c_model -B c_model/build && cmake --build c_model/build  # 1) c_model FIRST"
	@echo "  (cd cosim2/verilator && make)                                     # 2) Verilator depends on yaml-cpp.a built in c_model/build/_deps/"
