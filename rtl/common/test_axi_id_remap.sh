#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
mode=${1:-test}
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

common_sources=(
    "$root_dir/sim/dv/common_cells-1.37.0/src/cf_math_pkg.sv"
    "$root_dir/sim/dv/common_cells-1.37.0/src/lzc.sv"
    "$root_dir/sim/dv/axi-0.39.7/src/axi_pkg.sv"
    "$root_dir/sim/dv/axi-0.39.7/src/axi_id_remap.sv"
)
include_args=(
    -I"$root_dir/sim/dv/common_cells-1.37.0/include"
    -I"$root_dir/sim/dv/axi-0.39.7/include"
)

case "$mode" in
    test)
        verilator --binary --timing -Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNOPTFLAT \
            --top-module tb_axi_id_remap \
            "${include_args[@]}" "${common_sources[@]}" \
            "$root_dir/rtl/common/tests/tb_axi_id_remap.sv" \
            -Mdir "$tmp_dir/obj"
        "$tmp_dir/obj/Vtb_axi_id_remap"
        ;;
    illegal)
        expected_messages=(
            "AXI_ID_WIDTH must be between 1 and 8"
            "AXI_ID_WIDTH must be between 1 and 8"
            "NOC_ID_WIDTH must be fixed at 3"
            "NOC_ID_WIDTH must be fixed at 3"
        )
        for invalid_case in 0 1 2 3; do
            obj_dir="$tmp_dir/obj_$invalid_case"
            log="$tmp_dir/illegal_$invalid_case.log"
            verilator --binary --timing -Wno-TIMESCALEMOD \
                --top-module tb_axi_id_remap_illegal -GINVALID_CASE="$invalid_case" \
                "$root_dir/rtl/common/tests/tb_axi_id_remap_illegal.sv" \
                -Mdir "$obj_dir"
            if "$obj_dir/Vtb_axi_id_remap_illegal" >"$log" 2>&1; then
                echo "illegal ID-width case $invalid_case unexpectedly passed" >&2
                exit 1
            fi
            if ! grep -Fq "${expected_messages[$invalid_case]}" "$log"; then
                cat "$log" >&2
                echo "illegal ID-width case $invalid_case failed for an unexpected reason" >&2
                exit 1
            fi
        done
        ;;
    *)
        echo "usage: $0 {test|illegal}" >&2
        exit 2
        ;;
esac
