#!/usr/bin/env bash
set -euo pipefail

task_mode=${1:-test}
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
task_manifest="$task_root/rtl/Bender.yml"
task_revision=9ca8a7655f741e7dd5736669a20a301325194c28
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-request-fifo-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT

if [[ -n "${COMMON_CELLS_DIR:-}" ]]; then
    task_common_cells=$COMMON_CELLS_DIR
else
    task_common_cells="$task_tmp/common_cells"
    git clone --quiet https://github.com/pulp-platform/common_cells.git "$task_common_cells"
    git -C "$task_common_cells" checkout --quiet "$task_revision"
fi

[[ $(git -C "$task_common_cells" rev-parse HEAD) == "$task_revision" ]]
grep -Fq "$task_revision" "$task_manifest"

task_sources=(
    "$task_common_cells/src/binary_to_gray.sv"
    "$task_common_cells/src/gray_to_binary.sv"
    "$task_common_cells/src/spill_register_flushable.sv"
    "$task_common_cells/src/spill_register.sv"
    "$task_common_cells/src/stream_register.sv"
    "$task_common_cells/src/sync.sv"
    "$task_common_cells/src/cdc_fifo_gray.sv"
    "$task_root/rtl/common/axi_async_fifo.sv"
    "$task_root/rtl/nmu/nmu_request_fifo.sv"
    "$task_root/rtl/nmu/tests/tb_nmu_request_fifo.sv"
    "$task_root/rtl/nmu/tests/tb_nmu_request_fifo_guards.sv"
)

task_verilator=(
    verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-TIMESCALEMOD
    -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-SYNCASYNCNET -Wno-PINCONNECTEMPTY
    -I"$task_common_cells/include" --top-module tb_nmu_request_fifo
)

task_guard_messages=(
    "AXI_FIFO_DEPTH must be a power of two and at least 2"
    "AXI_ID_WIDTH must be between 1 and 8"
    "AXI_ID_WIDTH must be between 1 and 8"
)

case "$task_mode" in
    lint)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"
        ;;
    test)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"
        for task_depth in 2 8 32; do
            for task_id_width in 1 3 8; do
                task_obj_dir="$task_tmp/obj_dir_${task_depth}_${task_id_width}"
                "${task_verilator[@]}" \
                    -GAXI_FIFO_DEPTH="$task_depth" \
                    -GAXI_ID_WIDTH="$task_id_width" \
                    --binary --Mdir "$task_obj_dir" -o nmu_request_fifo_tb \
                    "${task_sources[@]}"
                "$task_obj_dir/nmu_request_fifo_tb"
            done
        done
        for task_case in 0 1 2; do
            task_obj_dir="$task_tmp/obj_dir_invalid_$task_case"
            task_log="$task_tmp/guard_$task_case.log"
            "${task_verilator[@]}" \
                --top-module tb_nmu_request_fifo_guards \
                -GINVALID_CASE="$task_case" \
                --binary --Mdir "$task_obj_dir" -o nmu_request_fifo_guards_tb \
                "${task_sources[@]}"
            if "$task_obj_dir/nmu_request_fifo_guards_tb" >"$task_log" 2>&1; then
                echo "request FIFO parameter guard $task_case did not fail" >&2
                exit 1
            fi
            if ! grep -Fq "${task_guard_messages[$task_case]}" "$task_log"; then
                cat "$task_log" >&2
                echo "request FIFO parameter guard $task_case failed for an unexpected reason" >&2
                exit 1
            fi
        done
        ;;
    *)
        echo "usage: $0 [lint|test]" >&2
        exit 2
        ;;
esac
