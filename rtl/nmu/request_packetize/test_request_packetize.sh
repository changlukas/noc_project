#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-request-packetize-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
task_revision=63b7c50d43e462b59506f69d341ff1e40202866d
if [[ -n "${COMMON_CELLS_DIR:-}" ]]; then
    task_common_cells=$COMMON_CELLS_DIR
else
    task_common_cells="$task_tmp/common_cells"
    git clone --quiet https://github.com/pulp-platform/common_cells.git "$task_common_cells"
    git -C "$task_common_cells" checkout --quiet "$task_revision"
fi
[[ $(git -C "$task_common_cells" rev-parse HEAD) == "$task_revision" ]]
task_sources=(
    "$task_common_cells/src/cc_pkg.sv"
    "$task_common_cells/src/cc_credit_counter.sv"
    "$task_common_cells/src/cc_fifo.sv"
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
    "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
    "$task_root/rtl/common/ni_child_types_pkg.sv"
    "$task_root/rtl/nmu/channel_assign/nmu_channel_assign.sv"
    "$task_root/rtl/nmu/request_packetize/nmu_request_packetize.sv"
    "$task_root/rtl/nmu/request_packetize/nmu_request_inject_tb_dut.sv"
)
task_verilator=(verilator "$task_root/rtl/nmu/top/nmu_lint.vlt" --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME
    -Wno-TIMESCALEMOD -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-PINCONNECTEMPTY
    -Werror-WIDTHTRUNC -Werror-WIDTHEXPAND -Werror-LATCH
    -I"$task_common_cells/include")
for task_top in tb_nmu_request_packetize tb_nmu_request_packetize_stall; do
    task_tb="$task_root/rtl/nmu/request_packetize/$task_top.sv"
    "${task_verilator[@]}" --lint-only --top-module "$task_top" "${task_sources[@]}" "$task_tb"
    if [[ "${1:-test}" == test ]]; then
        "${task_verilator[@]}" --binary -j 1 --top-module "$task_top" \
            --Mdir "$task_tmp/$task_top" -o packetize_tb "${task_sources[@]}" "$task_tb"
        "$task_tmp/$task_top/packetize_tb"
    fi
done

if [[ "${1:-test}" == test ]]; then
    task_top=tb_nmu_request_packetize_stress
    task_tb="$task_root/rtl/nmu/request_packetize/$task_top.sv"
    for task_config in 2:1:0 2:2:0 8:2:0 2:2:1 4:3:0 2:4:1 4:6:1 8:8:0; do
        IFS=: read -r task_depth task_vcs task_mode <<< "$task_config"
        task_obj="$task_tmp/stress_${task_depth}_${task_vcs}_${task_mode}"
        "${task_verilator[@]}" --binary -j 1 --top-module "$task_top" \
            -GFIFO_DEPTH="$task_depth" -GDAT_NUM_VC="$task_vcs" -GDAT_VC_MODE="$task_mode" \
            --Mdir "$task_obj" -o stress_tb "${task_sources[@]}" "$task_tb"
        "$task_obj/stress_tb"
    done
fi
