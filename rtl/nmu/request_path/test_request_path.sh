#!/usr/bin/env bash
set -euo pipefail

task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_revision=9ca8a7655f741e7dd5736669a20a301325194c28
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-request-path-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT

if [[ -n "${COMMON_CELLS_DIR:-}" ]]; then
    task_common_cells=$COMMON_CELLS_DIR
else
    task_common_cells="$task_tmp/common_cells"
    git clone --quiet https://github.com/pulp-platform/common_cells.git "$task_common_cells"
    git -C "$task_common_cells" checkout --quiet "$task_revision"
fi

task_generated="$task_tmp/topology_pkg.sv"
"${PYTHON3:-python3}" "$task_root/sim/tools/gen_tb_top.py" --topology mesh_2x2 \
    --emit-topology-pkg --out "$task_generated"

task_sources=(
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
    "$task_generated"
    "$task_root/rtl/common/ni_child_types_pkg.sv"
    "$task_common_cells/src/binary_to_gray.sv"
    "$task_common_cells/src/gray_to_binary.sv"
    "$task_common_cells/src/spill_register_flushable.sv"
    "$task_common_cells/src/spill_register.sv"
    "$task_common_cells/src/stream_register.sv"
    "$task_common_cells/src/sync.sv"
    "$task_common_cells/src/cdc_fifo_gray.sv"
    "$task_common_cells/src/cc_pkg.sv"
    "$task_common_cells/src/cc_addr_decode_dync.sv"
    "$task_common_cells/src/cc_addr_decode.sv"
    "$task_common_cells/src/cc_stream_register.sv"
    "$task_common_cells/src/cc_spill_register_flushable.sv"
    "$task_common_cells/src/cc_spill_register.sv"
    "$task_root/rtl/common/axi_async_fifo.sv"
    "$task_root/rtl/common/ni_sam.sv"
    "$task_root/rtl/common/stream_register.sv"
    "$task_root/rtl/nmu/request_fifo/nmu_request_fifo.sv"
    "$task_root/rtl/nmu/sam/nmu_sam.sv"
    "$task_root/rtl/nmu/request_path/nmu_request_path.sv"
    "$task_root/rtl/nmu/request_path/tb_nmu_request_path.sv"
)

task_verilator=(verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME
    -Wno-TIMESCALEMOD -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-SYNCASYNCNET
    -Wno-PINCONNECTEMPTY -I"$task_common_cells/include" --top-module tb_nmu_request_path)

"${task_verilator[@]}" --lint-only "${task_sources[@]}"
if [[ "${1:-test}" == test ]]; then
    "${task_verilator[@]}" --binary --Mdir "$task_tmp/obj_dir" \
        -o nmu_request_path_tb "${task_sources[@]}"
    "$task_tmp/obj_dir/nmu_request_path_tb"
fi
