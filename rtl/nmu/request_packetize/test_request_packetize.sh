#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-request-packetize-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
task_sources=(
  "$task_root/specgen/generated/sv/ni_params_pkg.sv"
  "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
  "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
  "$task_root/rtl/common/ni_child_types_pkg.sv"
  "$task_root/rtl/nmu/request_packetize/nmu_request_packetize.sv"
  "$task_root/rtl/nmu/request_packetize/tb_nmu_request_packetize.sv"
)
task_verilator=(verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME
  -Wno-TIMESCALEMOD -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-WIDTHTRUNC
  -Wno-WIDTHEXPAND -Wno-PROCASSINIT)
"${task_verilator[@]}" --lint-only --top-module tb_nmu_request_packetize "${task_sources[@]}"
if [[ "${1:-test}" == test ]]; then
  "${task_verilator[@]}" --binary --top-module tb_nmu_request_packetize \
    --Mdir "$task_tmp/obj_dir" -o nmu_request_packetize_tb "${task_sources[@]}"
  "$task_tmp/obj_dir/nmu_request_packetize_tb"
fi
