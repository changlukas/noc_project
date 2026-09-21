#!/usr/bin/env python3
"""Write reproducibility metadata for one accepted simulation result."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tempfile


GENERATED_PARAMETER_FILES = (
    pathlib.Path("specgen/generated/cpp/ni_params.h"),
    pathlib.Path("specgen/generated/sv/ni_params_pkg.sv"),
)
SOURCE_PATHS = ("ref_model", "sim", "specgen")
SOURCE_EXCLUDES = (
    ":(exclude)sim/verilator/output/**",
    ":(exclude)sim/test_patterns/**",
    ":(exclude)sim/filelist_*.f",
    ":(exclude)sim/tb/test/tb_top_*.sv",
    ":(exclude)sim/tb/soc/tb_top_dma_*.sv",
)


def _sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _parameter_sha256(root):
    digest = hashlib.sha256()
    for relative in GENERATED_PARAMETER_FILES:
        path = root / relative
        data = path.read_bytes()
        digest.update(relative.as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(data)
        digest.update(b"\0")
    return digest.hexdigest()


def _temporary_index(root, revision):
    directory = tempfile.TemporaryDirectory()
    index = pathlib.Path(directory.name) / "index"
    env = os.environ.copy()
    env["GIT_INDEX_FILE"] = str(index)
    subprocess.run(["git", "read-tree", revision], cwd=root, env=env,
                   check=True, stdout=subprocess.DEVNULL)
    return directory, env


def _source_patch(root, revision):
    directory, env = _temporary_index(root, revision)
    try:
        pathspec = [*SOURCE_PATHS, *SOURCE_EXCLUDES]
        changed = subprocess.check_output([
            "git", "ls-files", "-z", "--modified", "--deleted", "--others",
            "--exclude-standard", "--", *pathspec,
        ], cwd=root).split(b"\0")
        changed = sorted(os.fsdecode(path) for path in changed if path)
        if changed:
            subprocess.run(["git", "add", "-A", "--", *changed], cwd=root,
                           env=env, check=True, stdout=subprocess.DEVNULL)
        return subprocess.check_output([
            "git", "diff", "--cached", "--binary", "--full-index",
            "--no-ext-diff", "--no-color", revision, "--", *pathspec,
        ], cwd=root, env=env)
    finally:
        directory.cleanup()


def _validate_source_patch(root, revision, patch):
    if not patch.read_bytes():
        raise ValueError("source patch must not be empty")
    directory, env = _temporary_index(root, revision)
    try:
        subprocess.run(["git", "apply", "--cached", "--check", "--binary",
                        str(patch)], cwd=root, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       text=True)
    finally:
        directory.cleanup()


def _require(value, option):
    if value is None:
        raise ValueError(f"{option} is required")
    return value


def _write_manifest(root, output, config_arg, simulator, seed, command,
                    source_patch_arg):
    if output.exists():
        output.unlink()
    config = (root / config_arg).resolve(strict=True)
    config.relative_to(root)
    source_patch = pathlib.Path(source_patch_arg)
    if not source_patch.is_absolute():
        source_patch = root / source_patch
    source_patch = source_patch.resolve(strict=True)
    patch_relative = source_patch.relative_to(root).as_posix()
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    simulator_version = subprocess.check_output(
        [simulator, "--version"], text=True).strip()
    if not revision or not simulator_version or not command.strip():
        raise ValueError("manifest metadata must not be empty")
    _validate_source_patch(root, revision, source_patch)

    manifest = {
        "git_revision": revision,
        "config_file_sha256": _sha256(config),
        "generated_parameter_sha256": _parameter_sha256(root),
        "simulator_version": simulator_version,
        "seed": seed,
        "exact_command": command,
        "source_patch": patch_relative,
        "source_patch_sha256": _sha256(source_patch),
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def _refresh_root(root, result_root, config, simulator, source_patch,
                  command_override):
    result_root = result_root.resolve(strict=True)
    result_root.relative_to(root)
    patch = pathlib.Path(source_patch)
    if not patch.is_absolute():
        patch = root / patch
    patch = patch.resolve(strict=True)
    patch_relative = patch.relative_to(root).as_posix()
    patch_sha256 = _sha256(patch)
    results = sorted(result_root.rglob("result.csv"))
    manifests = sorted(result_root.rglob("manifest.json"))
    expected = {path.with_name("manifest.json") for path in results}
    if not results or set(manifests) != expected:
        raise ValueError("refresh root must contain one manifest per result.csv")
    existing = []
    for manifest in manifests:
        run_log = manifest.with_name("run.log")
        if not run_log.is_file() or not re.search(
                r"^PASS: all \d+ nodes done, non-vacuous$",
                run_log.read_text(encoding="utf-8"), re.MULTILINE):
            raise ValueError(f"result lacks non-vacuous checker PASS: {manifest.parent}")
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        if not isinstance(payload.get("seed"), int) or not payload.get("exact_command"):
            raise ValueError(f"invalid existing manifest: {manifest}")
        recorded_patch = payload.get("source_patch")
        recorded_sha256 = payload.get("source_patch_sha256")
        if (recorded_patch is None) != (recorded_sha256 is None):
            raise ValueError(f"incomplete source patch metadata: {manifest}")
        if recorded_patch is not None and (
                recorded_patch != patch_relative or
                recorded_sha256 != patch_sha256):
            raise ValueError(f"source patch metadata mismatch: {manifest}")
        existing.append((manifest, payload))
    for manifest, payload in existing:
        _write_manifest(root, manifest, config, simulator, payload["seed"],
                        command_override or payload["exact_command"], source_patch)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--config", type=pathlib.Path)
    parser.add_argument("--simulator")
    parser.add_argument("--seed", type=int)
    parser.add_argument("--command")
    parser.add_argument("--source-patch", type=pathlib.Path)
    parser.add_argument("--write-source-patch", action="store_true")
    parser.add_argument("--refresh-root", type=pathlib.Path)
    args = parser.parse_args(argv)

    root = args.repo_root.resolve(strict=True)
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    if args.write_source_patch:
        output = _require(args.out, "--out")
        if args.refresh_root:
            raise ValueError("--write-source-patch and --refresh-root are exclusive")
        patch = _source_patch(root, revision)
        if not patch:
            raise ValueError("source patch must not be empty")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(patch)
        return

    config = _require(args.config, "--config")
    simulator = _require(args.simulator, "--simulator")
    source_patch = _require(args.source_patch, "--source-patch")
    if args.refresh_root:
        _refresh_root(root, args.refresh_root, config, simulator, source_patch,
                      args.command)
        return
    output = _require(args.out, "--out")
    seed = _require(args.seed, "--seed")
    command = _require(args.command, "--command")
    _write_manifest(root, output, config, simulator, seed, command, source_patch)


if __name__ == "__main__":
    main()
