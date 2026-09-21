#!/usr/bin/env python3
"""Prepare a generated standalone run for direct SSH synchronization or optional export."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


def package(run_dir, output, directory=False):
    run_dir = Path(run_dir).resolve()
    repo = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="nmu-vcs-package-") as temporary:
        root = Path(temporary) / "nmu-standalone"
        root.mkdir()
        copied = {}

        def copy_path(source):
            source = Path(source).resolve()
            if source in copied:
                return copied[source]
            if source.is_relative_to(repo):
                relative = Path("repo") / source.relative_to(repo)
            elif source.is_relative_to(run_dir):
                relative = Path("generated") / source.relative_to(run_dir)
            else:
                # Dependency checkouts keep their pinned directory names.
                parts = source.parts
                pivot = next(i for i, part in enumerate(parts)
                             if part.startswith(("common_cells", "tech_cells_generic")))
                relative = Path("deps", *parts[pivot:])
                dependency = Path(*parts[:pivot+1])
                for license_file in dependency.glob("LICENSE*"):
                    target = root / "deps" / dependency.name / license_file.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(license_file, target)
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target, dirs_exist_ok=True)
            else:
                shutil.copy2(source, target)
            copied[source] = relative.as_posix()
            return copied[source]

        lines = []
        for line in (run_dir / "files.f").read_text().splitlines():
            if line.startswith("+incdir+"):
                lines.append("+incdir+" + copy_path(line[len("+incdir+"):]))
            elif line.strip():
                lines.append(copy_path(line))
        (root / "files.f").write_text("\n".join(lines) + "\n")
        for license_file in (repo / "sim/dv").glob("*/LICENSE*"):
            copy_path(license_file)
        for name in ("neighbor", "uniform_random", "hotspot", "directed"):
            source = run_dir / name
            if name != "directed":
                source /= "node0"
            shutil.copytree(source, root / "cases" / name)
        copy_path(repo / "rtl/nmu/top/nmu_lint.vlt")
        (root / "script").mkdir()
        shutil.copy2(repo / "sim/vcs/nmu/Makefile", root / "script/Makefile")
        shutil.copy2(repo / "sim/vcs/nmu/config.mk", root / "script/config.mk")
        (root / "run_vcs.sh").write_text(r'''#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make -C script regress "$@"
''')
        (root / "run_verilator.sh").write_text(r'''#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make -C script regress SIMULATOR=verilator "$@"
''')
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
        (root / "VERSION.txt").write_text(
            "Issue: 118\nCheckout HEAD: " + revision + "\n"
            "Compare SHA256SUMS for exact packaged source/config/pattern bytes.\n")
        (root / "README.txt").write_text(
            "Issue #118 NMU control-plane synchronized simulation tree.\n"
            "Run all (VCS default): bash run_vcs.sh\n"
            "Same suite locally: make -C script regress SIMULATOR=verilator\n"
            "Shared configuration: script/config.mk\n"
            "Run one: make -C script run PATTERN=neighbor\n"
            "FSDB: make -C script run_wave PATTERN=directed\n"
            "Reuse binary: make -C script sim PATTERN=hotspot\n"
            "Open waveform: make -C script nWave PATTERN=directed\n"
            "Package logs: make -C script report\n"
            "Requires an initialized VCS environment, GNU Make and Bash. No Git, Python or network needed.\n"
            "Default: external ID width 8, AXI clock 10ns, NoC clock 14ns, B/R depth 128.\n"
            "No NSU, memory model or C++ DPI. DAT RX is not implemented in this stage.\n"
            "This package has not been validated with VCS until workstation results are returned.\n")
        checksums = []
        for file in sorted(root.rglob("*")):
            if file.is_file():
                checksums.append(hashlib.sha256(file.read_bytes()).hexdigest() + "  " + file.relative_to(root).as_posix())
        (root / "SHA256SUMS").write_text("\n".join(checksums) + "\n")
        output = Path(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        if directory:
            shutil.copytree(root, output, dirs_exist_ok=True)
            print(output)
            return
        with tarfile.open(output, "w:gz") as archive:
            archive.add(root, arcname=root.name)
        print(output)
        print("SHA256", hashlib.sha256(output.read_bytes()).hexdigest())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument("--output")
    destination.add_argument("--output-dir")
    args = parser.parse_args()
    package(args.run_dir, args.output_dir or args.output, directory=bool(args.output_dir))
