#!/usr/bin/env python3
"""Elaborate each NMU path boundary against the canonical generated types."""

import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
VERILATOR = shutil.which("verilator")

COMMON_SOURCES = [
    ROOT / "specgen/generated/sv/ni_params_pkg.sv",
    ROOT / "specgen/generated/sv/ni_signals_pkg.sv",
    ROOT / "specgen/generated/sv/ni_flit_pkg.sv",
    ROOT / "rtl/common/ni_child_types_pkg.sv",
]
def elaborate(top_module: str, dut_source: pathlib.Path, testbench: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix=f"{top_module}_") as temp_dir:
        command = [
            VERILATOR,
            "--binary",
            "--top-module",
            top_module,
            "--Mdir",
            str(pathlib.Path(temp_dir) / "obj_dir"),
            *map(str, COMMON_SOURCES),
            str(dut_source),
            str(testbench),
        ]
        subprocess.run(command, check=True)


def main() -> None:
    if VERILATOR is None:
        raise SystemExit("verilator is required for the NMU path elaboration check")

    elaborate(
        "tb_nmu_request_path",
        ROOT / "rtl/nmu/request_path/nmu_request_path.sv",
        ROOT / "rtl/nmu/request_path/tb_nmu_request_path.sv",
    )
    elaborate(
        "tb_nmu_response_path",
        ROOT / "rtl/nmu/response_path/nmu_response_path.sv",
        ROOT / "rtl/nmu/response_path/tb_nmu_response_path.sv",
    )


if __name__ == "__main__":
    main()
