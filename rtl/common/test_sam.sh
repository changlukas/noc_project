#!/usr/bin/env bash
set -euo pipefail

task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
task_manifest="$task_root/rtl/Bender.yml"
task_revision=63b7c50d43e462b59506f69d341ff1e40202866d
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/noc-ni-sam-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT

if [[ -n "${COMMON_CELLS_DIR:-}" ]]; then
    task_common_cells=$COMMON_CELLS_DIR
else
    task_common_cells="$task_tmp/common_cells"
    git clone --quiet https://github.com/pulp-platform/common_cells.git "$task_common_cells"
    git -C "$task_common_cells" checkout --quiet "$task_revision"
fi

[[ $(git -C "$task_common_cells" rev-parse HEAD) == "$task_revision" ]]
[[ $(git -C "$task_common_cells" describe --tags --exact-match) == "v2.0.0-beta.3" ]]
grep -Fq "$task_revision" "$task_manifest"

task_generated="$task_tmp/generated/mesh_2x2/topology_pkg.sv"
mkdir -p "$(dirname "$task_generated")"
python3 "$task_root/sim/tools/gen_tb_top.py" --topology mesh_2x2 \
    --emit-topology-pkg --out "$task_generated"

task_sources=(
    "$task_root/specgen/generated/sv/ni_params_pkg.sv"
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
    "$task_generated"
    "$task_common_cells/src/cc_pkg.sv"
    "$task_common_cells/src/cc_addr_decode_dync.sv"
    "$task_common_cells/src/cc_addr_decode.sv"
    "$task_root/rtl/common/ni_sam.sv"
    "$task_root/rtl/common/tests/tb_ni_sam.sv"
)

task_verilator=(
    verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-TIMESCALEMOD
    -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -I"$task_common_cells/include"
    --top-module tb_ni_sam
)

case "${1:-test}" in
    lint)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"
        ;;
    test)
        "${task_verilator[@]}" --lint-only "${task_sources[@]}"

        task_obj_dir="$task_tmp/obj_dir"
        "${task_verilator[@]}" --binary --Mdir "$task_obj_dir" -o ni_sam_tb "${task_sources[@]}"
        "$task_obj_dir/ni_sam_tb" +disable_assert_final_checks
        ;;
    *)
        echo "usage: $0 [lint|test]" >&2
        exit 2
        ;;
esac
