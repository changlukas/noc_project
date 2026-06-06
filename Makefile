# Top-level orchestration: clean all build artifacts across the project.
#
# Removes:
#   - c_model/build/                       (CMake build dir, ~230 MB)
#   - cosim2/verilator/obj_dir/            (Verilator output, ~22 MB)
#   - specgen pycache                      (pytest bytecode cache)
#   - master_shell_read_dump.txt           (Vtb_top runtime artifact, any cwd)
#
# Preserves:
#   - All source files
#   - All checked-in regen output under specgen/generated/

.PHONY: clean

clean:
	rm -rf c_model/build
	rm -rf cosim2/verilator/obj_dir
	find specgen -type d -name __pycache__ -prune -exec rm -rf {} +
	rm -f master_shell_read_dump.txt
	rm -f cosim2/verilator/master_shell_read_dump.txt
	rm -f c_model/build/master_shell_read_dump.txt
	@echo "clean done."
