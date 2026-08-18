from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def test_nmu_req_model_strobe_holds_until_rtl_handshake(tmp_path: Path) -> None:
    verilator = shutil.which("verilator")
    assert verilator is not None, "Verilator is required for the model-egress regression"

    obj_dir = tmp_path / "obj_dir"
    sources = [
        ROOT / "specgen/generated/sv/ni_params_pkg.sv",
        ROOT / "specgen/generated/sv/ni_signals_pkg.sv",
        ROOT / "sim/dv/common_cells-1.37.0/src/spill_register_flushable.sv",
        ROOT / "sim/dv/common_cells-1.37.0/src/spill_register.sv",
        ROOT / "ref_model/top/nmu_wrap.sv",
        ROOT / "sim/tests/model_egress/tb_nmu_req_hold.sv",
        ROOT / "sim/tests/model_egress/fake_nmu_dpi.cpp",
    ]
    command = [
        verilator,
        "--binary",
        "--timing",
        "--assert",
        "--Wno-fatal",
        "--top-module",
        "tb_nmu_req_hold",
        "--Mdir",
        str(obj_dir),
        f"-I{ROOT / 'sim/dv/common_cells-1.37.0/include'}",
        "-CFLAGS",
        f"-I{ROOT / 'ref_model/dpi'}",
        *map(str, sources),
    ]

    build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    assert build.returncode == 0, build.stdout + build.stderr

    run = subprocess.run(
        [str(obj_dir / "Vtb_nmu_req_hold")], cwd=ROOT, text=True, capture_output=True
    )
    assert run.returncode == 0, run.stdout + run.stderr
    assert (
        "PASS: model REQ strobes held through directed and randomized RTL-side stalls"
        in run.stdout
    )
