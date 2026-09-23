#!/bin/bash
set -euo pipefail
package_dir=$1
run_dir=$2
python=$3
wave_ext=$4
order=${6:-in_order}
case "$order" in in_order|out_of_order) ;; *) exit 2 ;; esac
cd "$package_dir"
"$python" repo/sim/tools/gen_standalone_patterns.py \
    --out "$run_dir/${order}_patterns" --topology cases/topology.json \
    --catalog repo/sim/test_patterns/standalone/${order}_perf.json --id-width "$5"
while read -r name; do
    mapfile -t args < "$run_dir/${order}_patterns/$name/schedule.txt"
    "$run_dir/simv" +stim_dir="$run_dir/${order}_patterns/$name" "${args[@]}" \
        +perf_${order} +perf_trace="$run_dir/report/$name.csv" \
        +wave_file="$run_dir/waves/$name.$wave_ext" \
        2>&1 | tee "$run_dir/report/$name.log"
    grep -q 'PASS NMU standalone' "$run_dir/report/$name.log"
done < "$run_dir/${order}_patterns/cases.list"
