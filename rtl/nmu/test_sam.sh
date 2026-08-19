#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
task_manifest="$task_root/rtl/Bender.yml"
task_revision=63b7c50d43e462b59506f69d341ff1e40202866d
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/noc-nmu-sam-XXXXXX")
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

task_topology=${SAM_TOPOLOGY:-mesh_2x2}
task_generated="$task_tmp/generated/$task_topology/topology_pkg.sv"
mkdir -p "$(dirname "$task_generated")"
python3 "$task_root/sim/tools/gen_tb_top.py" --topology "$task_topology" \
    --emit-topology-pkg --out "$task_generated"

task_sources=(
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
    "$task_generated"
    "$task_root/rtl/common/ni_child_types_pkg.sv"
    "$task_common_cells/src/cc_pkg.sv"
    "$task_common_cells/src/cc_addr_decode_dync.sv"
    "$task_common_cells/src/cc_addr_decode.sv"
    "$task_common_cells/src/cc_stream_register.sv"
    "$task_common_cells/src/cc_spill_register_flushable.sv"
    "$task_common_cells/src/cc_spill_register.sv"
    "$task_root/rtl/common/ni_sam.sv"
    "$task_root/rtl/common/noc_reg_slice.sv"
    "$task_root/rtl/nmu/nmu_sam.sv"
    "$task_root/rtl/nmu/tests/tb_nmu_sam.sv"
)

task_verilator=(
    verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-TIMESCALEMOD
    -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-SYNCASYNCNET -I"$task_common_cells/include"
    --top-module tb_nmu_sam
)

case "${1:-test}" in
    lint)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"
        ;;
    test)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"

        task_obj_dir="$task_tmp/obj_dir"
        "${task_verilator[@]}" --binary --Mdir "$task_obj_dir" -o nmu_sam_tb "${task_sources[@]}"
        "$task_obj_dir/nmu_sam_tb"

        for task_topology in mesh_2x2 mesh_2x2_periph; do
            task_generated="$task_tmp/generated/$task_topology/topology_pkg.sv"
            mkdir -p "$(dirname "$task_generated")"
            python3 "$task_root/sim/tools/gen_tb_top.py" --topology "$task_topology" \
                --emit-topology-pkg --out "$task_generated"
            task_sources[3]="$task_generated"
            task_obj_dir="$task_tmp/obj_dir_vectors_$task_topology"
            "${task_verilator[@]}" --top-module tb_nmu_sam_vectors --binary \
                --Mdir "$task_obj_dir" -o nmu_sam_vectors_tb "${task_sources[@]}" \
                "$task_root/rtl/nmu/tests/tb_nmu_sam_vectors.sv"
            "$task_obj_dir/nmu_sam_vectors_tb"
        done

        task_guard_messages=(
            "AW_SAM_REG_TYPE must be 0, 1, or 2"
            "AR_SAM_REG_TYPE must be 0, 1, or 2"
            "AW burst footprint crosses a SAM region boundary"
            "AR burst footprint crosses a SAM region boundary"
            "invalid AW collective mapping"
            "invalid AW SAM mapping"
            "invalid AR SAM mapping"
            "invalid AW collective mapping"
        )
        for task_case in 0 1 2 3 4 5 6 7; do
            task_obj_dir="$task_tmp/obj_dir_guard_$task_case"
            task_log="$task_tmp/guard_$task_case.log"
            "${task_verilator[@]}" \
                --top-module tb_nmu_sam_guards \
                -GINVALID_CASE="$task_case" \
                --binary --Mdir "$task_obj_dir" -o nmu_sam_guards_tb \
                "${task_sources[@]}" "$task_root/rtl/nmu/tests/tb_nmu_sam_guards.sv"
            if "$task_obj_dir/nmu_sam_guards_tb" >"$task_log" 2>&1; then
                echo "nmu_sam parameter guard $task_case did not fail" >&2
                exit 1
            fi
            if ! grep -Fq "${task_guard_messages[$task_case]}" "$task_log"; then
                cat "$task_log" >&2
                echo "nmu_sam parameter guard $task_case failed for an unexpected reason" >&2
                exit 1
            fi
        done
        ;;
    *)
        echo "usage: $0 [lint|test]" >&2
        exit 2
        ;;
esac
