#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
task_out=${NMU_DAT_TEST_OUTPUT:-$task_root/sim/standalone/nmu/output/dat-ingress}
mkdir -p "$task_out"
if [[ -n "${NMU_DAT_STAGE:-}" ]]; then
    task_stage=$NMU_DAT_STAGE
else
    make -C "$task_root/sim/standalone/nmu" prepare ID_WIDTH=8 NOC_HALF_PERIOD=5 BUFFER_DEPTH=128 R_ROB_EN=1 > "$task_out/prepare.log" 2>&1
    task_stage="$task_root/sim/standalone/nmu/output/i8_n5_b128_r1/stage"
fi
cd "$task_stage"
task_sim=${SIMULATOR:-verilator}
ulimit -c 0
task_sources=(
    repo/specgen/generated/sv/ni_params_pkg.sv
    repo/specgen/generated/sv/ni_signals_pkg.sv
    repo/specgen/generated/sv/ni_flit_pkg.sv
    repo/rtl/common/ni_types_pkg.sv
    deps/common_cells/src/cc_pkg.sv
    deps/common_cells/src/cc_fifo.sv
    repo/sim/dv/common_cells-1.37.0/src/cf_math_pkg.sv
    repo/sim/dv/common_cells-1.37.0/src/lzc.sv
    repo/sim/dv/common_cells-1.37.0/src/rr_arb_tree.sv
    repo/rtl/nmu/response_depacketize/response_buffer.sv
    repo/rtl/nmu/response_depacketize/response_depacketize.sv
)
task_compile() {
    task_name="v${task_vcs}_m${task_mode}_d${task_depth}"
    task_wave=()
    if [[ "$task_sim" == verilator ]]; then
        if [[ ${WAVE:-0} == 1 ]]; then task_wave=(+define+DUMP_WAVE --trace-fst); fi
        verilator --binary --timing --assert -j 1 -Wno-fatal \
            -Werror-WIDTHEXPAND -Werror-WIDTHTRUNC -Werror-LATCH \
            "$task_root/rtl/nmu/top/nmu_lint.vlt" \
            --top-module tb_nmu_response_depacketize \
            -GNUM_DAT_VC="$task_vcs" -GDAT_VC_MODE="$task_mode" -GDAT_RX_VC_DEPTH="$task_depth" \
            -Ideps/common_cells/include "${task_sources[@]}" \
            "$task_root/rtl/nmu/response_depacketize/tb_response_depacketize.sv" \
            ${task_wave[@]+"${task_wave[@]}"} --Mdir "$task_out/$task_name" > "$task_out/$task_name.compile.log" 2>&1
        task_binary="$task_out/$task_name/Vtb_nmu_response_depacketize"
        task_ext=fst
    elif [[ "$task_sim" == vcs ]]; then
        task_key=$(pwd -P | tr -d '\n' | cksum | cut -d' ' -f1)
        task_cache="/tmp/noc-vcs-$(id -u)-$task_key"
        mkdir -m 700 "$task_cache" 2>/dev/null || [[ -d "$task_cache" && ! -L "$task_cache" && -O "$task_cache" ]]
        task_work="$task_cache/dat_${task_name}_wave${WAVE:-0}"
        [[ ! -L "$task_work" ]]; mkdir -p "$task_work"
        if [[ ${WAVE:-0} == 1 ]]; then
            task_pli=${PLI_DIR:-${VERDI_HOME:-/cadtools/synopsys/verdi/M-2017.03-SP1}/share/PLI/VCS/linux64}
            task_wave=(+define+DUMP_WAVE -P "$task_pli/novas.tab" "$task_pli/pli.a")
            export LD_LIBRARY_PATH="$task_pli${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        fi
        vcs -full64 -sverilog -assert svaext -override_timescale=1ns/1ps -debug_access+all \
            +incdir+deps/common_cells/include "${task_sources[@]}" \
            "$task_root/rtl/nmu/response_depacketize/tb_response_depacketize.sv" \
            -top tb_nmu_response_depacketize \
            -pvalue+tb_nmu_response_depacketize.NUM_DAT_VC="$task_vcs" \
            -pvalue+tb_nmu_response_depacketize.DAT_VC_MODE="$task_mode" \
            -pvalue+tb_nmu_response_depacketize.DAT_RX_VC_DEPTH="$task_depth" \
            ${task_wave[@]+"${task_wave[@]}"} -Mdir="$task_work/csrc" -o "$task_work/simv" \
            -l "$task_out/$task_name.compile.log" > "$task_out/$task_name.console.log" 2>&1
        task_binary="$task_work/simv"
        task_ext=fsdb
    else
        echo "Unsupported simulator: $task_sim" >&2; exit 1
    fi
}
task_fault() {
    "$task_binary" +fault="$1" > "$task_out/$task_name.fault$1.log" 2>&1 || true
    grep -Eq "$2" "$task_out/$task_name.fault$1.log"
}
for task_cfg in '1 0 2' '2 0 8' '6 1 2' '8 0 32' '8 1 8'; do
    read -r task_vcs task_mode task_depth <<< "$task_cfg"
    task_compile
    "$task_binary" +wave_file="$task_out/$task_name.$task_ext" | tee "$task_out/$task_name.log"
    grep -q 'PASS depacketize' "$task_out/$task_name.log"
    task_fault 1 'invalid channel or VC'
    task_fault 4 'credit overflow|Trying to push new data'
    if [[ $task_vcs -lt 8 ]]; then task_fault 2 'invalid channel or VC'; fi
    if [[ $task_mode == 1 ]]; then task_fault 3 'invalid channel or VC'; fi
done
for task_cfg in '2 0 0 depth' '2 0 1 depth' '2 0 3 depth' '1 1 2 mode'; do
    read -r task_vcs task_mode task_depth task_guard <<< "$task_cfg"
    task_compile
    "$task_binary" > "$task_out/$task_name.guard.log" 2>&1 || true
    if [[ $task_guard == depth ]]; then
        grep -q 'DAT_RX_VC_DEPTH must be a power of two and at least 2' "$task_out/$task_name.guard.log"
    else
        grep -q 'DAT_VC_MODE split requires a positive even VC count' "$task_out/$task_name.guard.log"
    fi
done
echo "PASS DAT ingress profiles, fault checks and parameter guards"
