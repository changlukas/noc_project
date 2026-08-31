#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/nmu-ordering-XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
task_sources=(
  "$task_root/specgen/generated/sv/ni_params_pkg.sv"
  "$task_root/specgen/generated/sv/ni_signals_pkg.sv"
  "$task_root/specgen/generated/sv/ni_flit_pkg.sv"
  "$task_root/rtl/common/ni_child_types_pkg.sv"
  "$task_root/rtl/nmu/ordering/nmu_reorder_storage.sv"
  "$task_root/rtl/nmu/ordering/nmu_ordering.sv"
)
task_verilator=(verilator --timing --assert -Wall -Wno-fatal -Wno-DECLFILENAME
  -Wno-TIMESCALEMOD -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-WIDTHTRUNC
  -Wno-WIDTHEXPAND -Wno-PROCASSINIT)
"${task_verilator[@]}" --lint-only --top-module tb_nmu_ordering \
  "${task_sources[@]}" "$task_root/rtl/nmu/ordering/tb_nmu_ordering.sv"
"${task_verilator[@]}" --lint-only --top-module tb_nmu_ordering_robless \
  "${task_sources[@]}" "$task_root/rtl/nmu/ordering/tb_nmu_ordering_robless.sv"
if [[ "${1:-test}" == test ]]; then
  "${task_verilator[@]}" --binary --top-module tb_nmu_ordering \
    --Mdir "$task_tmp/obj_dir" -o nmu_ordering_tb \
    "${task_sources[@]}" "$task_root/rtl/nmu/ordering/tb_nmu_ordering.sv"
  "$task_tmp/obj_dir/nmu_ordering_tb"
  "${task_verilator[@]}" --binary --top-module tb_nmu_ordering_robless \
    --Mdir "$task_tmp/obj_dir_robless" -o nmu_ordering_robless_tb \
    "${task_sources[@]}" "$task_root/rtl/nmu/ordering/tb_nmu_ordering_robless.sv"
  "$task_tmp/obj_dir_robless/nmu_ordering_robless_tb"

  task_tag_w=$(sed -nE 's/.*ORDERING_TAG_WIDTH[[:space:]]*=[[:space:]]*([0-9]+);/\1/p' \
    "$task_root/specgen/generated/sv/ni_flit_pkg.sv")
  test -n "$task_tag_w"
  task_invalid_limit=$(( (1 << task_tag_w) + 1 ))
  task_guard_values=(
    "-GNMU_ROB_B_DEPTH=$task_invalid_limit"
    "-GNMU_ROB_R_DEPTH=$task_invalid_limit"
    "-GNMU_MAX_TXNS_PER_ID=$task_invalid_limit"
  )
  task_guard_messages=(
    "NMU_ROB_B_DEPTH must be in [1, TAG_SPACE]"
    "NMU_ROB_R_DEPTH must be in [1, TAG_SPACE]"
    "NMU_MAX_TXNS_PER_ID must be in [1, TAG_SPACE]"
  )
  for task_case in 0 1 2; do
    task_log="$task_tmp/guard_$task_case.log"
    task_guard_obj="$task_tmp/obj_dir_guard_$task_case"
    "${task_verilator[@]}" --binary --top-module nmu_ordering \
      --Mdir "$task_guard_obj" -o nmu_ordering_guard_tb \
      ${task_guard_values[$task_case]} "${task_sources[@]}"
    if "$task_guard_obj/nmu_ordering_guard_tb" >"$task_log" 2>&1; then
      echo "ordering parameter guard $task_case did not fail" >&2
      exit 1
    fi
    grep -Fq "${task_guard_messages[$task_case]}" "$task_log"
  done
fi
