"""Standalone clean removes generated products without touching simulation inputs."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_clean_preserves_inputs_and_external_symlink_target(tmp_path):
    stage = tmp_path / "standalone"
    script = stage / "script"
    script.mkdir(parents=True)
    shutil.copy2(ROOT / "sim/standalone/common/clean.sh", script / "clean.sh")
    shutil.copy2(ROOT / "sim/standalone/common/simulator.mk", script / "Makefile")
    shutil.copy2(ROOT / "sim/standalone/nmu/config.mk", script / "config.mk")
    keep = ["files.f", "repo/rtl/nmu.sv", "deps/source.sv", "cases/read.txt",
            "generated/topology_pkg.sv", "pattern_list.txt", "SHA256SUMS",
            "script/nWaveLog/signals.rc", "script/nWaveLog/signals.rc.before-hierarchy"]
    remove = ["build/vcs_wave1/simv", "build/vcs_wave0/report/run.log",
              "build/vcs_wave1/waves/run.fsdb", "build/verilator/csrc/a.o",
              "novas_dump.log", "ucli.key", "simv.daidir/data", "csrc/data",
              "script/novas.conf", "script/novas.rc", "script/nWaveLog/novas.rc",
              "script/nWaveLog/fsdb.log", "script/nWaveLog/nWave.cmd",
              "script/nWaveLog/pes.bat", "script/nWaveLog/turbo.log",
              "verdiLog/session.log", "nmu-vcs-results.tar.gz", "core.123"]
    for name in keep + remove:
        p = stage / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(name)
    outside = tmp_path / "external"
    outside.mkdir()
    (outside / "keep.log").write_text("external")
    (script / "VerdiLog").symlink_to(outside, target_is_directory=True)
    # clean does not use simulator configuration or follow run_dir overrides.
    for _ in range(2):
        subprocess.run(["make", "-C", str(script), "clean", f"run_dir={outside}"], check=True)
    assert all((stage / name).read_text() == name for name in keep)
    assert all(not (stage / name).exists() for name in remove)
    assert (outside / "keep.log").read_text() == "external"
    assert not (script / "VerdiLog").is_symlink()


def test_clean_refuses_non_standalone_tree(tmp_path):
    script = tmp_path / "script"
    script.mkdir()
    shutil.copy2(ROOT / "sim/standalone/common/clean.sh", script / "clean.sh")
    build = tmp_path / "build"
    build.mkdir()
    (build / "keep").touch()
    result = subprocess.run(["bash", str(script / "clean.sh")], capture_output=True)
    assert result.returncode != 0
    assert (build / "keep").exists()
