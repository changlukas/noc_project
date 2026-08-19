#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

task_mode=${1:-test}
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
task_manifest="$task_root/rtl/Bender.yml"
task_revision=63b7c50d43e462b59506f69d341ff1e40202866d
task_tech_revision=3a3de73632a06826b1bd9c65a0a2e92b32016845
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/noc-common-primitives-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT

if [[ -n "${COMMON_CELLS_DIR:-}" ]]; then
    task_common_cells=$COMMON_CELLS_DIR
else
    task_common_cells="$task_tmp/common_cells"
    git clone --quiet https://github.com/pulp-platform/common_cells.git "$task_common_cells"
    git -C "$task_common_cells" checkout --quiet "$task_revision"
fi

if [[ -n "${TECH_CELLS_GENERIC_DIR:-}" ]]; then
    task_tech_cells=$TECH_CELLS_GENERIC_DIR
else
    task_tech_cells="$task_tmp/tech_cells_generic"
    git clone --quiet https://github.com/pulp-platform/tech_cells_generic.git "$task_tech_cells"
    git -C "$task_tech_cells" checkout --quiet "$task_tech_revision"
fi

[[ $(git -C "$task_common_cells" rev-parse HEAD) == "$task_revision" ]]
[[ $(git -C "$task_common_cells" describe --tags --exact-match) == "v2.0.0-beta.3" ]]
[[ $(git -C "$task_tech_cells" rev-parse HEAD) == "$task_tech_revision" ]]
grep -Fq "$task_revision" "$task_manifest"

task_sources=(
    "$task_common_cells/src/cc_pkg.sv"
    "$task_common_cells/src/cc_binary_to_gray.sv"
    "$task_common_cells/src/cc_fifo.sv"
    "$task_common_cells/src/cc_gray_to_binary.sv"
    "$task_common_cells/src/cc_spill_register_flushable.sv"
    "$task_common_cells/src/cc_spill_register.sv"
    "$task_common_cells/src/cc_stream_register.sv"
    "$task_tech_cells/src/rtl/tc_sync.sv"
    "$task_common_cells/src/cc_cdc_fifo_gray.sv"
    "$task_root/rtl/common/noc_sync_fifo.sv"
    "$task_root/rtl/common/axi_async_fifo.sv"
    "$task_root/rtl/common/noc_reg_slice.sv"
    "$task_root/rtl/common/tests/tb_common_primitives.sv"
    "$task_root/rtl/common/tests/tb_common_primitive_guards.sv"
)

task_verilator=(
    verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-TIMESCALEMOD
    -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-SYNCASYNCNET
    -I"$task_common_cells/include" --top-module tb_common_primitives
)

task_guard_messages=(
    "NOC_FIFO_DEPTH must be a power of two"
    "AXI_FIFO_DEPTH must be a power of two and at least 2"
    "REG_TYPE must be 0, 1, or 2"
)

case "$task_mode" in
    lint)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"
        ;;
    test)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"
        for task_depth in 2 8 32; do
            task_obj_dir="$task_tmp/obj_dir_$task_depth"
            "${task_verilator[@]}" \
                -GNOC_FIFO_DEPTH="$task_depth" \
                -GAXI_FIFO_DEPTH="$task_depth" \
                --binary --Mdir "$task_obj_dir" -o common_primitives_tb \
                "${task_sources[@]}"
            "$task_obj_dir/common_primitives_tb"
        done
        for task_case in 0 1 2; do
            task_obj_dir="$task_tmp/obj_dir_invalid_$task_case"
            task_log="$task_tmp/guard_$task_case.log"
            "${task_verilator[@]}" \
                --top-module tb_common_primitive_guards \
                -GINVALID_CASE="$task_case" \
                --binary --Mdir "$task_obj_dir" -o common_primitive_guards_tb \
                "${task_sources[@]}"
            if "$task_obj_dir/common_primitive_guards_tb" >"$task_log" 2>&1; then
                echo "adapter parameter guard $task_case did not fail" >&2
                exit 1
            fi
            if ! grep -Fq "${task_guard_messages[$task_case]}" "$task_log"; then
                cat "$task_log" >&2
                echo "adapter parameter guard $task_case failed for an unexpected reason" >&2
                exit 1
            fi
        done
        ;;
    *)
        echo "usage: $0 [lint|test]" >&2
        exit 2
        ;;
esac
