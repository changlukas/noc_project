#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/noc-nmu-shell-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT

task_sources=(
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
    "$task_root/rtl/common/taxi_axi_if.sv"
    "$task_root/rtl/nmu/nmu.sv"
    "$task_root/rtl/nmu/tests/tb_nmu_elaborate.sv"
)

task_verilator=(
    verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-TIMESCALEMOD
    --top-module tb_nmu_elaborate
)

"${task_verilator[@]}" --lint-only "${task_sources[@]}"
"${task_verilator[@]}" --binary --Mdir "$task_tmp/obj_dir" -o nmu_elaborate_tb "${task_sources[@]}"
"$task_tmp/obj_dir/nmu_elaborate_tb"
