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


def test_vcs_build_uses_local_cache_and_copies_runtime(tmp_path):
    stage = tmp_path / "standalone"
    script = stage / "script"
    script.mkdir(parents=True)
    for source, target in [("common/clean.sh", "clean.sh"),
                           ("common/simulator.mk", "Makefile"),
                           ("nmu/config.mk", "config.mk")]:
        shutil.copy2(ROOT / "sim/standalone" / source, script / target)
    (stage / "files.f").touch()
    (stage / "SHA256SUMS").touch()
    cases = stage / "cases/standalone"
    cases.mkdir(parents=True)
    (cases / "cases.list").write_text("ctrl_write_single\n")
    stub = tmp_path / "vcs"
    stub.write_text("#!/usr/bin/env python3\n"
                    "import pathlib, sys\n"
                    "out = pathlib.Path(sys.argv[sys.argv.index('-o')+1])\n"
                    "out.parent.mkdir(parents=True, exist_ok=True)\n"
                    "out.write_text(str(out))\n"
                    "out.chmod(0o755)\n"
                    "database = out.with_name('simv.daidir')\n"
                    "database.mkdir(exist_ok=True)\n"
                    "(database / 'runtime.so').write_text('runtime')\n")
    stub.chmod(0o755)
    try:
        subprocess.run(["make", "-C", str(script), "compile", f"VCS={stub}"], check=True)
        binary = next((stage / "build").glob("*/simv"))
        cached_binary = Path(binary.read_text())
        assert str(cached_binary).startswith("/tmp/noc-vcs-")
        assert cached_binary.exists()
        assert (binary.parent / "simv.daidir/runtime.so").read_text() == "runtime"
    finally:
        subprocess.run(["make", "-C", str(script), "clean"], check=True)
    assert not cached_binary.exists()


def test_fault_requires_checker_diagnostic_not_exit_status(tmp_path):
    stage = tmp_path / "standalone"
    script = stage / "script"
    script.mkdir(parents=True)
    shutil.copy2(ROOT / "sim/standalone/common/simulator.mk", script / "Makefile")
    shutil.copy2(ROOT / "sim/standalone/nmu/config.mk", script / "config.mk")
    (stage / "files.f").touch()
    cases = stage / "cases/standalone"
    cases.mkdir(parents=True)
    (cases / "cases.list").write_text("ctrl_write_single\n")
    run = stage / "build/test"
    run.mkdir(parents=True)
    binary = run / "simv"
    for diagnostic, status, passes in [
        ("R data/lane/order/last mismatch", 0, True),
        ("R data/lane/order/last mismatch", 1, True),
        ("PASS NMU standalone", 0, False),
        ("unrelated simulator error", 1, False),
    ]:
        binary.write_text(f"#!/bin/sh\necho '{diagnostic}'\nexit {status}\n")
        binary.chmod(0o755)
        result = subprocess.run(["make", "-C", str(script), "fault", f"run_dir={run}"],
                                capture_output=True)
        assert (result.returncode == 0) == passes
