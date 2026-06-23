#!/usr/bin/env bash
# Git Bash on Windows wrapper for the genamba testbench.
# Prepends MSYS2 mingw64/bin + usr/bin to PATH so Git Bash can find
# verilator + make + their dependencies, without polluting the parent
# shell's environment (PATH changes live only in this script's process).
#
# Usage:
#   ./cosim/verilator/run_genamba.sh                       # T1/T2 (no plusarg)
#   ./cosim/verilator/run_genamba.sh +scenario=PATH        # T3+ (cmodel_init)
#
# Any args passed to this script are forwarded to the simulator exe.

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Prepend MSYS2 paths for this script only. Adjust if your MSYS2 install
# is at a non-default location.
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
export LC_ALL=C

make -C "$SCRIPT_DIR" genamba PYTHON3=python3
exec "$SCRIPT_DIR/obj_genamba/Vtb_genamba.exe" "$@"
