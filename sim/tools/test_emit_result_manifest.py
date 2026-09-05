import json
import hashlib
import pathlib
import subprocess
import sys


SCRIPT = pathlib.Path(__file__).with_name("emit_result_manifest.py")
ROOT = SCRIPT.parents[2]
PARAM_FILES = (
    pathlib.Path("specgen/generated/cpp/ni_params.h"),
    pathlib.Path("specgen/generated/sv/ni_params_pkg.sv"),
)


def _write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _repo(tmp_path):
    _write(tmp_path / ".gitignore", "sim/verilator/output/\n")
    _write(tmp_path / "sim/configs/mesh_4x4.yml", "name: mesh_4x4\n")
    for path in PARAM_FILES:
        _write(tmp_path / path, f"generated: {path.name}\n")
    _write(tmp_path / "specgen/generated/cpp/ni_flit_constants.h", "unrelated\n")
    _write(tmp_path / "ref_model/tracked.cpp", "int tracked = 1;\n")
    _write(tmp_path / "sim/tools/tracked.py", "TRACKED = 1\n")
    _write(tmp_path / "specgen/source/constants.yaml", "parameters: {}\n")
    simulator = tmp_path / "fake-verilator"
    _write(simulator, "#!/bin/sh\necho 'Verilator 5.test'\n")
    simulator.chmod(0o755)
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    subprocess.run(["git", "config", "user.email", "test@example.com"],
                   cwd=tmp_path, check=True)
    subprocess.run(["git", "config", "user.name", "Test"],
                   cwd=tmp_path, check=True)
    subprocess.run(["git", "add", "."], cwd=tmp_path, check=True)
    subprocess.run(["git", "commit", "-qm", "fixture"], cwd=tmp_path, check=True)
    return simulator


def _write_source_patch(tmp_path):
    output = tmp_path / "sim/verilator/output/source.patch"
    result = subprocess.run([
        sys.executable, str(SCRIPT), "--write-source-patch",
        "--out", str(output), "--repo-root", str(tmp_path),
    ], text=True, capture_output=True)
    return result, output


def _emit(tmp_path, simulator, source_patch):
    output = tmp_path / "run/manifest.json"
    result = subprocess.run([
        sys.executable, str(SCRIPT), "--out", str(output),
        "--repo-root", str(tmp_path), "--config", "sim/configs/mesh_4x4.yml",
        "--simulator", str(simulator), "--seed", "1",
        "--command", "make -C sim/verilator sim CONFIG=mesh_4x4 SEED=1",
        "--source-patch", str(source_patch),
    ], text=True, capture_output=True)
    return result, output


def test_manifest_records_reproducible_cell_metadata_and_exact_parameter_set(tmp_path):
    simulator = _repo(tmp_path)
    _write(tmp_path / "ref_model/tracked.cpp", "int tracked = 2;\n")
    patch_result, source_patch = _write_source_patch(tmp_path)
    assert patch_result.returncode == 0, patch_result.stderr
    result, output = _emit(tmp_path, simulator, source_patch)
    assert result.returncode == 0, result.stderr
    first = json.loads(output.read_text(encoding="utf-8"))
    assert set(first) == {
        "git_revision", "config_file_sha256", "generated_parameter_sha256",
        "simulator_version", "seed", "exact_command", "source_patch",
        "source_patch_sha256",
    }
    assert first["git_revision"] == subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=tmp_path, text=True).strip()
    assert first["simulator_version"] == "Verilator 5.test"
    assert first["seed"] == 1
    assert first["exact_command"].endswith("CONFIG=mesh_4x4 SEED=1")
    assert first["source_patch"] == "sim/verilator/output/source.patch"
    assert first["source_patch_sha256"] == hashlib.sha256(
        source_patch.read_bytes()).hexdigest()

    _write(tmp_path / "specgen/generated/cpp/ni_flit_constants.h", "changed\n")
    result, _ = _emit(tmp_path, simulator, source_patch)
    assert result.returncode == 0
    assert json.loads(output.read_text())["generated_parameter_sha256"] == \
        first["generated_parameter_sha256"]

    _write(tmp_path / PARAM_FILES[0], "changed parameter\n")
    result, _ = _emit(tmp_path, simulator, source_patch)
    assert result.returncode == 0
    assert json.loads(output.read_text())["generated_parameter_sha256"] != \
        first["generated_parameter_sha256"]


def test_manifest_fails_closed_when_a_generated_parameter_is_missing(tmp_path):
    simulator = _repo(tmp_path)
    _write(tmp_path / "ref_model/tracked.cpp", "int tracked = 2;\n")
    patch_result, source_patch = _write_source_patch(tmp_path)
    assert patch_result.returncode == 0, patch_result.stderr
    (tmp_path / PARAM_FILES[1]).unlink()
    result, output = _emit(tmp_path, simulator, source_patch)
    assert result.returncode != 0
    assert not output.exists()


def test_source_patch_is_deterministic_and_contains_only_reproduction_sources(tmp_path):
    _repo(tmp_path)
    _write(tmp_path / "ref_model/tracked.cpp", "int tracked = 2;\n")
    _write(tmp_path / "sim/tools/new_tool.py", "NEW = True\n")
    _write(tmp_path / "specgen/source/constants.yaml", "parameters:\n  changed: true\n")
    _write(tmp_path / "docs/report.md", "excluded\n")
    _write(tmp_path / "sim/verilator/output/result.csv", "excluded\n")
    _write(tmp_path / "sim/verilator/test_patterns/mesh/node0/read.txt", "excluded\n")

    first_result, source_patch = _write_source_patch(tmp_path)
    assert first_result.returncode == 0, first_result.stderr
    first = source_patch.read_bytes()
    second_result, _ = _write_source_patch(tmp_path)
    assert second_result.returncode == 0, second_result.stderr
    assert source_patch.read_bytes() == first

    text = first.decode("utf-8")
    assert "ref_model/tracked.cpp" in text
    assert "sim/tools/new_tool.py" in text
    assert "specgen/source/constants.yaml" in text
    assert "docs/report.md" not in text
    assert "sim/verilator/output/result.csv" not in text
    assert "sim/verilator/test_patterns" not in text

    checkout = tmp_path.parent / f"{tmp_path.name}-clean"
    subprocess.run(["git", "clone", "-q", str(tmp_path), str(checkout)],
                   check=True)
    subprocess.run(["git", "apply", "--binary", str(source_patch)],
                   cwd=checkout, check=True)
    assert (checkout / "ref_model/tracked.cpp").read_text() == "int tracked = 2;\n"
    assert (checkout / "sim/tools/new_tool.py").read_text() == "NEW = True\n"


def test_manifest_fails_closed_when_source_patch_does_not_apply_to_revision(tmp_path):
    simulator = _repo(tmp_path)
    _write(tmp_path / "ref_model/tracked.cpp", "int tracked = 2;\n")
    patch_result, source_patch = _write_source_patch(tmp_path)
    assert patch_result.returncode == 0, patch_result.stderr
    source_patch.write_text("not a unified diff\n", encoding="utf-8")

    result, output = _emit(tmp_path, simulator, source_patch)
    assert result.returncode != 0
    assert not output.exists()


def test_refresh_root_refuses_result_without_non_vacuous_checker_pass(tmp_path):
    simulator = _repo(tmp_path)
    _write(tmp_path / "ref_model/tracked.cpp", "int tracked = 2;\n")
    patch_result, source_patch = _write_source_patch(tmp_path)
    assert patch_result.returncode == 0, patch_result.stderr
    emit_result, old_manifest = _emit(tmp_path, simulator, source_patch)
    assert emit_result.returncode == 0, emit_result.stderr

    cell = tmp_path / "campaign/cell"
    cell.mkdir(parents=True)
    _write(cell / "result.csv", "checker_status\nPASS\n")
    _write(cell / "run.log", "checker did not finish\n")
    old_manifest.replace(cell / "manifest.json")

    result = subprocess.run([
        sys.executable, str(SCRIPT), "--refresh-root", str(tmp_path / "campaign"),
        "--repo-root", str(tmp_path), "--config", "sim/configs/mesh_4x4.yml",
        "--simulator", str(simulator), "--source-patch", str(source_patch),
        "--command", "new exact command",
    ], text=True, capture_output=True)

    assert result.returncode != 0


def test_refresh_root_refuses_a_different_valid_source_patch(tmp_path):
    simulator = _repo(tmp_path)
    _write(tmp_path / "ref_model/tracked.cpp", "int tracked = 2;\n")
    patch_result, source_patch = _write_source_patch(tmp_path)
    assert patch_result.returncode == 0, patch_result.stderr
    emit_result, old_manifest = _emit(tmp_path, simulator, source_patch)
    assert emit_result.returncode == 0, emit_result.stderr

    cell = tmp_path / "campaign/cell"
    cell.mkdir(parents=True)
    _write(cell / "result.csv", "checker_status\nPASS\n")
    _write(cell / "run.log", "PASS: all 16 nodes done, non-vacuous\n")
    old_manifest.replace(cell / "manifest.json")

    _write(tmp_path / "sim/tools/tracked.py", "TRACKED = 2\n")
    replacement_result, _ = _write_source_patch(tmp_path)
    assert replacement_result.returncode == 0, replacement_result.stderr
    result = subprocess.run([
        sys.executable, str(SCRIPT), "--refresh-root", str(tmp_path / "campaign"),
        "--repo-root", str(tmp_path), "--config", "sim/configs/mesh_4x4.yml",
        "--simulator", str(simulator), "--source-patch", str(source_patch),
        "--command", "new exact command",
    ], text=True, capture_output=True)

    assert result.returncode != 0


def test_verilator_run_emits_manifest_after_result_acceptance():
    result = subprocess.run([
        "make", "-n", "-C", "sim/verilator", "run",
        "CONFIG=mesh_2x2", "PATTERN=neighbor", "SEED=1",
        "REPRO_COMMAND=make -C sim/verilator sim CONFIG=mesh_2x2 PATTERN=neighbor SEED=1",
    ], cwd=ROOT, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.index("emit_result_csv.py") < \
        result.stdout.index("emit_result_manifest.py")
    assert "--source-patch" in result.stdout
