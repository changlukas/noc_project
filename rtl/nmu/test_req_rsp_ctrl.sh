#!/usr/bin/env bash
set -euo pipefail

task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-req-rsp-ctrl-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT

task_sources=(
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
    "$task_root/rtl/common/nmu_req_rsp_ctrl_if.sv"
    "$task_root/rtl/nmu/nmu_req_rsp_ctrl_wrap.sv"
    "$task_root/rtl/nmu/tests/tb_nmu_req_rsp_ctrl.sv"
)

verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME \
    --top-module tb_nmu_req_rsp_ctrl --lint-only "${task_sources[@]}"
for task_id_width in 1 3 8; do
    verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME \
        --top-module tb_nmu_req_rsp_ctrl --lint-only \
        -GAXI_ID_WIDTH="$task_id_width" "${task_sources[@]}"
done
verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME \
    --top-module tb_nmu_req_rsp_ctrl --binary --Mdir "$task_tmp/obj_dir" \
    -o nmu_req_rsp_ctrl_tb "${task_sources[@]}"
"$task_tmp/obj_dir/nmu_req_rsp_ctrl_tb"
