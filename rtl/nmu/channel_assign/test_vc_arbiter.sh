#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_out=${NMU_VC_TEST_OUTPUT:-$task_root/sim/standalone/nmu/output/vc-arbiter}
mkdir -p "$task_out"
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT
task_cc=${COMMON_CELLS_DIR:-$task_tmp/common_cells}
if [[ -z ${COMMON_CELLS_DIR:-} ]]; then
    git clone -q https://github.com/pulp-platform/common_cells.git "$task_cc"
    git -C "$task_cc" checkout -q 63b7c50d43e462b59506f69d341ff1e40202866d
fi
[[ $(git -C "$task_cc" rev-parse HEAD) == 63b7c50d43e462b59506f69d341ff1e40202866d ]]
for task_n in 1 2 3 4 6 8; do
    for task_depth in 2 8; do
        task_name="n${task_n}_d${task_depth}"
        verilator --binary --timing --assert -j 1 -Wno-fatal             --top-module tb_floo_vc_arbiter -GN="$task_n" -GDEPTH="$task_depth"             -I"$task_cc/include" "$task_root/rtl/nmu/top/nmu_lint.vlt"             "$task_root/sim/dv/axi-0.39.7/src/axi_pkg.sv"             "$task_root/sim/dv/floonoc-c58f1bf/floo_pkg.sv"             "$task_cc/src/cc_pkg.sv" "$task_cc/src/cc_lzc.sv"             "$task_cc/src/cc_rr_arb_tree.sv" "$task_cc/src/cc_credit_counter.sv"             "$task_root/sim/dv/floonoc-c58f1bf/floo_vc_arbiter.sv"             "$task_root/rtl/nmu/channel_assign/tb_floo_vc_arbiter.sv"             --Mdir "$task_out/$task_name" > "$task_out/$task_name.compile.log" 2>&1
        "$task_out/$task_name/Vtb_floo_vc_arbiter" | tee "$task_out/$task_name.log"
    done
done
