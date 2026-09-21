#!/usr/bin/env python3
"""Package an already generated standalone run for an offline VCS workstation."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import tempfile


def package(run_dir, output):
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
        (root / "run_vcs.sh").write_text(r'''#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build logs
vcs -full64 -sverilog -timescale=1ns/1ps -top tb_nmu_standalone \
    -f files.f -Mdir=build/csrc -o build/simv -l logs/compile.log
for pattern in neighbor uniform_random hotspot directed; do
    args=()
    if [[ $pattern == directed ]]; then args+=(+require_reorder); fi
    ./build/simv +stim_dir="cases/$pattern" "${args[@]}" -l "logs/$pattern.log"
    grep -q 'PASS NMU standalone' "logs/$pattern.log"
done
if ./build/simv +stim_dir=cases/directed +corrupt_rsp -l logs/corrupt.log; then
    echo 'ERROR: corrupt response unexpectedly passed' >&2
    exit 1
fi
grep -q 'R data/lane/order/last mismatch' logs/corrupt.log
tar -czf nmu-vcs-results.tar.gz logs
echo 'PASS: logs packaged in nmu-vcs-results.tar.gz'
''')
        (root / "README.txt").write_text(
            "Issue #118 NMU control-plane standalone snapshot.\n"
            "Run: bash run_vcs.sh\n"
            "Requires an initialized VCS environment and Bash. No Git, Python or network needed.\n"
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
        with tarfile.open(output, "w:gz") as archive:
            archive.add(root, arcname=root.name)
        print(output)
        print("SHA256", hashlib.sha256(output.read_bytes()).hexdigest())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    package(args.run_dir, args.output)
