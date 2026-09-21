#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
export NMU_PATH_TOP=tb_nmu_request_path
export NMU_PATH_TB="$task_root/rtl/nmu/request_path/tb_nmu_request_path.sv"
exec bash "$task_root/rtl/nmu/top/test_nmu.sh" "${1:-test}"
