#!/usr/bin/env bash
set -euo pipefail

task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_revision=9ca8a7655f741e7dd5736669a20a301325194c28
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-response-path-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT

if [[ -n "${COMMON_CELLS_DIR:-}" ]]; then
    task_common_cells=$COMMON_CELLS_DIR
else
    task_common_cells="$task_tmp/common_cells"
    git clone --quiet https://github.com/pulp-platform/common_cells.git "$task_common_cells"
    git -C "$task_common_cells" checkout --quiet "$task_revision"
fi

task_sources=(
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
    "$task_common_cells/src/binary_to_gray.sv"
    "$task_common_cells/src/gray_to_binary.sv"
    "$task_common_cells/src/spill_register_flushable.sv"
    "$task_common_cells/src/spill_register.sv"
    "$task_common_cells/src/stream_register.sv"
    "$task_common_cells/src/sync.sv"
    "$task_common_cells/src/cdc_fifo_gray.sv"
    "$task_root/rtl/common/axi_async_fifo.sv"
    "$task_root/rtl/nmu/response_fifo/nmu_response_fifo.sv"
    "$task_root/rtl/nmu/response_path/nmu_response_path.sv"
)
task_verilator=(verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME
    -Wno-TIMESCALEMOD -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-SYNCASYNCNET
    -Wno-PINCONNECTEMPTY)

"${task_verilator[@]}" --lint-only --top-module tb_nmu_response_path \
    "${task_sources[@]}" "$task_root/rtl/nmu/response_path/tb_nmu_response_path.sv"
if [[ "${1:-test}" == test ]]; then
    "${task_verilator[@]}" --binary --top-module tb_nmu_response_path \
        --Mdir "$task_tmp/obj_dir" -o nmu_response_path_tb \
        "${task_sources[@]}" "$task_root/rtl/nmu/response_path/tb_nmu_response_path.sv"
    "$task_tmp/obj_dir/nmu_response_path_tb"
    task_log="$task_tmp/guard.log"
    "${task_verilator[@]}" --binary --top-module tb_nmu_response_fifo_guards \
        --Mdir "$task_tmp/obj_guard" -o nmu_response_fifo_guard_tb \
        "${task_sources[@]}" "$task_root/rtl/nmu/response_fifo/tb_nmu_response_fifo_guards.sv"
    if "$task_tmp/obj_guard/nmu_response_fifo_guard_tb" >"$task_log" 2>&1; then
        echo "response FIFO invalid depth did not fail" >&2
        exit 1
    fi
    grep -Fq "AXI_FIFO_DEPTH must be a power of two and at least 2" "$task_log"
fi
