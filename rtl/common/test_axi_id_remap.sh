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
    "$root_dir/rtl/nmu/request_path/id_remap.sv"
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
    nmu_lint)
        verilator --lint-only --timing --assert -Wno-TIMESCALEMOD -Wno-UNOPTFLAT \
            --top-module tb_axi_id_remap -GNMU_REMAP=1 \
            "$root_dir/rtl/nmu/top/nmu_lint.vlt" \
            "${include_args[@]}" "${common_sources[@]}" \
            "$root_dir/rtl/common/tests/tb_axi_id_remap.sv"
        ;;
    illegal)
        verilator --binary --timing -Wno-TIMESCALEMOD --top-module tb_axi_id_remap_illegal \
            "$root_dir/rtl/common/tests/tb_axi_id_remap_illegal.sv" \
            -Mdir "$tmp_dir/obj"
        if "$tmp_dir/obj/Vtb_axi_id_remap_illegal"; then
            echo "illegal AXI ID-width test unexpectedly passed" >&2
            exit 1
        fi
        ;;
    *)
        echo "usage: $0 {test|nmu_lint|illegal}" >&2
        exit 2
        ;;
esac
