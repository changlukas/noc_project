#!/usr/bin/env bash
set -euo pipefail

task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_revision=63b7c50d43e462b59506f69d341ff1e40202866d
task_tech_revision=3a3de73632a06826b1bd9c65a0a2e92b32016845
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-top-XXXXXX")
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
[[ $(git -C "$task_tech_cells" rev-parse HEAD) == "$task_tech_revision" ]]

task_generated="$task_tmp/topology_pkg.sv"
"${PYTHON3:-python3}" "$task_root/sim/tools/gen_tb_top.py" --topology mesh_2x2 \
    --emit-topology-pkg --out "$task_generated"

task_sources=(
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
    "$task_generated"
    "$task_root/rtl/common/ni_child_types_pkg.sv"
    "$task_common_cells/src/cc_pkg.sv"
    "$task_common_cells/src/cc_binary_to_gray.sv"
    "$task_common_cells/src/cc_gray_to_binary.sv"
    "$task_common_cells/src/cc_spill_register_flushable.sv"
    "$task_common_cells/src/cc_spill_register.sv"
    "$task_common_cells/src/cc_stream_register.sv"
    "$task_tech_cells/src/rtl/tc_sync.sv"
    "$task_common_cells/src/cc_cdc_fifo_gray.sv"
    "$task_common_cells/src/cc_addr_decode_dync.sv"
    "$task_common_cells/src/cc_addr_decode.sv"
    "$task_root/rtl/common/axi_async_fifo.sv"
    "$task_root/rtl/common/ni_sam.sv"
    "$task_root/rtl/common/stream_register.sv"
    "$task_root/rtl/nmu/request_fifo/nmu_request_fifo.sv"
    "$task_root/rtl/nmu/sam/nmu_sam.sv"
    "$task_root/sim/dv/common_cells-1.37.0/src/cf_math_pkg.sv"
    "$task_root/sim/dv/common_cells-1.37.0/src/lzc.sv"
    "$task_root/sim/dv/common_cells-1.37.0/src/rr_arb_tree.sv"
    "$task_root/sim/dv/axi-0.39.7/src/axi_pkg.sv"
    "$task_root/sim/dv/axi-0.39.7/src/axi_id_remap.sv"
    "$task_common_cells/src/cc_credit_counter.sv"
    "$task_common_cells/src/cc_fifo.sv"
    "$task_root/rtl/common/axi_if.sv"
    "$task_root/rtl/nmu/ordering/nmu_reorder_storage.sv"
    "$task_root/rtl/nmu/ordering/nmu_ordering.sv"
    "$task_root/rtl/nmu/channel_assign/nmu_channel_assign.sv"
    "$task_root/rtl/nmu/request_packetize/nmu_request_packetize.sv"
    "$task_root/rtl/nmu/response_depacketize/nmu_response_buffer.sv"
    "$task_root/rtl/nmu/response_depacketize/nmu_response_depacketize.sv"
    "$task_root/rtl/nmu/response_fifo/nmu_response_fifo.sv"
    "$task_root/rtl/nmu/response_path/nmu_response_path.sv"
    "$task_root/rtl/nmu/request_path/nmu_request_path.sv"
    "$task_root/rtl/nmu/top/nmu.sv"
    "${NMU_PATH_TB:-$task_root/rtl/nmu/top/tb_nmu_elaborate.sv}"
)

task_verilator=(verilator --timing --assert -Wall -Wno-fatal
    -Werror-WIDTHEXPAND -Werror-WIDTHTRUNC -Werror-LATCH
    "$task_root/rtl/nmu/top/nmu_lint.vlt" -Wno-DECLFILENAME
    -Wno-TIMESCALEMOD -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-SYNCASYNCNET
    -Wno-PINCONNECTEMPTY -I"$task_common_cells/include"
    -I"$task_root/sim/dv/axi-0.39.7/include" -I"$task_root/sim/dv/common_cells-1.37.0/include"
    --top-module "${NMU_PATH_TOP:-tb_nmu_elaborate}")
if [[ "${1:-test}" == standalone || "${1:-test}" == prepare ]]; then
    task_sources+=(
        "$task_tech_cells/src/rtl/tc_clk.sv"
        "$task_common_cells/src/cc_rstgen_bypass.sv"
        "$task_root/sim/dv/common_verification-0.2.5/src/rand_id_queue.sv"
        "$task_root/sim/dv/axi-0.39.7/src/axi_intf.sv"
        "$task_root/sim/dv/axi-0.39.7/src/axi_test.sv"
        "$task_root/sim/standalone/nmu/tb_nmu_standalone.sv"
    )
    task_output=${NMU_TEST_OUTPUT:-$task_tmp/standalone}
    mkdir -p "$task_output"
    cp "$task_generated" "$task_output/topology_pkg.sv"
    task_sources[3]="$task_output/topology_pkg.sv"
    printf '%s\n' "+incdir+$task_common_cells/include" \
        "+incdir+$task_root/sim/dv/axi-0.39.7/include" \
        "+incdir+$task_root/sim/dv/common_cells-1.37.0/include" \
        "${task_sources[@]}" > "$task_output/files.f"
    if [[ ${1:-test} != prepare ]]; then
    "${task_verilator[@]}" --top-module tb_nmu_standalone --binary -j 1 \
        -GID_WIDTH="${NMU_ID_WIDTH:-8}" -GNOC_HALF_PERIOD="${NMU_NOC_HALF_PERIOD:-5}" \
        -GBUFFER_DEPTH="${NMU_BUFFER_DEPTH:-128}" -GREAD_ROB_ENABLED="${NMU_READ_ROB:-1}" \
        --Mdir "$task_output/obj" "${task_sources[@]}"
    fi
    for task_pattern in neighbor uniform_random hotspot; do
        python3 "$task_root/sim/tools/gen_test_patterns.py" --pattern "$task_pattern" \
            --topology mesh_2x2 --space config --size 3 --len 3 --transactions-per-node 16 \
            --ids-per-initiator 1 --hotspot 1 --seed 17 --out "$task_output/$task_pattern"
        if [[ ${1:-test} != prepare ]]; then
            "$task_output/obj/Vtb_nmu_standalone" +stim_dir="$task_output/$task_pattern/node0"
        fi
    done
    python3 "$task_root/sim/tools/gen_nmu_standalone_patterns.py" \
        --out "$task_output/directed" --topology "$task_root/sim/configs/mesh_2x2.yml" \
        --id-width "${NMU_ID_WIDTH:-8}"
    task_block_patterns="$task_root/sim/test_patterns/standalone/generated/i${NMU_ID_WIDTH:-8}"
    python3 "$task_root/sim/tools/gen_standalone_patterns.py" --out "$task_block_patterns" \
        --id-width "${NMU_ID_WIDTH:-8}"
    if [[ ${1:-test} == prepare ]]; then
        python3 "$task_root/sim/tools/package_nmu_standalone.py" --run-dir "$task_output" \
            --block-patterns "$task_block_patterns" --output-dir "$task_output/stage"
        exit
    fi
    task_directed_args=(+require_reorder)
    if [[ ${NMU_BUFFER_DEPTH:-128} == 8 && ${NMU_READ_ROB:-1} == 1 ]]; then
        task_directed_args+=(+require_pressure)
    fi
    "$task_output/obj/Vtb_nmu_standalone" +stim_dir="$task_output/directed" "${task_directed_args[@]}"
    if "$task_output/obj/Vtb_nmu_standalone" +stim_dir="$task_output/directed" \
        +corrupt_rsp > "$task_output/corrupt.log" 2>&1; then
        echo "corrupt response unexpectedly passed" >&2
        exit 1
    fi
    grep -q 'R data/lane/order/last mismatch' "$task_output/corrupt.log"
    exit
fi
"${task_verilator[@]}" --lint-only "${task_sources[@]}"
if [[ "${1:-test}" == test ]]; then
    "${task_verilator[@]}" --binary -j 1 --Mdir "$task_tmp/obj" "${task_sources[@]}"
    "$task_tmp/obj/V${NMU_PATH_TOP:-tb_nmu_elaborate}"
fi
