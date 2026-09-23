#!/usr/bin/env python3
"""Prepare a generated standalone run for direct SSH synchronization or optional export."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from gen_standalone_patterns import generate as generate_standalone


def package(run_dir, output, directory=False, block_patterns=None, id_width=8):
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
                if relative.parts[0] == "deps" and source.name == "cc_fifo.sv":
                    # Size the wrap bound explicitly before comparison. Some older
                    # simulators mis-evaluate the parameter part-select subtraction.
                    text = source.read_text()
                    old = "FifoDepth[PtrWidth-1:0] - 1"
                    if text.count(old) != 2:
                        raise ValueError("Pinned cc_fifo wrap expressions changed; review compatibility patch")
                    target.write_text(text.replace(old, "PtrWidth'(FifoDepth - 1)"))
            copied[source] = relative.as_posix()
            return copied[source]

        lines = []
        for line in (run_dir / "files.f").read_text().splitlines():
            if line.startswith("+incdir+"):
                lines.append("+incdir+" + copy_path(line[len("+incdir+"):]))
            elif line.strip():
                relative = copy_path(line)
                # Resolve only interfaces actually used by this top from the library.
                lines.append(("-v " if Path(line).name == "axi_intf.sv" else "") + relative)
        (root / "files.f").write_text("\n".join(lines) + "\n")
        for license_file in (repo / "sim/dv").glob("*/LICENSE*"):
            copy_path(license_file)
        for name in ("neighbor", "uniform_random", "hotspot", "directed"):
            source = run_dir / name
            if name != "directed":
                source /= "node0"
            shutil.copytree(source, root / "cases" / name)
        if block_patterns:
            shutil.copytree(block_patterns, root / "cases/standalone")
        else:
            generate_standalone(root / "cases/standalone", repo / "sim/configs/mesh_2x2.yml", id_width)
        copy_path(repo / "sim/test_patterns/standalone/cases.json")
        for name in ("gen_standalone_patterns.py", "axi_file_format.py"):
            copy_path(repo / "sim/tools" / name)
        import yaml
        from address_map import pack_config
        _, entries = pack_config(yaml.safe_load((repo / "sim/configs/mesh_2x2.yml").read_text()))
        (root / "cases/topology.json").write_text(json.dumps(entries))

        copy_path(repo / "rtl/nmu/top/nmu_lint.vlt")
        copy_path(repo / "rtl/nmu/response_depacketize/tb_response_depacketize.sv")
        copy_path(repo / "rtl/nmu/response_depacketize/test_response_depacketize.sh")
        (root / "script").mkdir()
        shutil.copy2(repo / "sim/standalone/common/simulator.mk", root / "script/Makefile")
        shutil.copy2(repo / "sim/standalone/common/clean.sh", root / "script/clean.sh")
        shutil.copy2(repo / "sim/standalone/nmu/config.mk", root / "script/config.mk")
        (root / "script/nWaveLog").mkdir()
        shutil.copy2(repo / "sim/standalone/nmu/signals.rc", root / "script/nWaveLog/signals.rc")
        (root / "Makefile").write_text(
            ".DEFAULT_GOAL := help\n"
            ".PHONY: help compile run sim regress block_regress legacy_regress run_wave run_wave_view nWave view fault report clean dat_regress list\n"
            "help compile run sim regress block_regress legacy_regress run_wave run_wave_view nWave view fault report clean dat_regress:\n"
            "\t$(MAKE) --no-print-directory -C script $@\n"
            "list:\n\t@cat pattern_list.txt\n")
        descriptions = {
            "ctrl_write_single": "Single control write",
            "ctrl_read_single": "Single control read",
            "ctrl_write_burst": "Control write bursts: SIZE, FIXED/INCR/WRAP, WSTRB, LAST",
            "ctrl_read_burst": "Control read bursts: SIZE, FIXED/INCR/WRAP, lanes, LAST",
            "same_id_in_order": "Same ID, requests and responses in order",
            "same_id_outstanding": "Same ID, multiple outstanding transactions",
            "multi_id_outstanding": "Multiple IDs and outstanding transactions",
            "cross_id_out_of_order": "Out-of-order responses across IDs",
            "same_id_cross_dst_reorder": "Same ID across destinations, B/R reordering",
            "outstanding_full_recover": "Capacity pressure and recovery, including ID remap reuse",
            "backpressure": "REQ and AXI response stalls",
            "reset_inflight": "Coordinated reset with transactions in flight",
            "data_write_single": "Single data write",
            "data_read_single": "Single data read",
            "data_write_burst": "Data write bursts: SIZE, FIXED/INCR/WRAP, WSTRB, LAST",
            "data_read_burst": "Data read bursts: full data bus, beat count, LAST",
            "ctrl_rand": "Seeded control requests",
            "data_rand": "Seeded data requests",
            "request_rand": "Seeded mixed control/data requests",

        }
        names = (root / "cases/standalone/cases.list").read_text().splitlines()
        (root / "pattern_list.txt").write_text(
            "NMU standalone control/data/random patterns\n\n"
            "First run (compile + simulate): make run CASE=ctrl_write_burst\n"
            "After compilation (reuse binary): make sim CASE=ctrl_write_burst\n"
            "Shared scenarios (5-12): MODE=control|data|rand, default control\n"
            "Random example: make sim CASE=same_id_outstanding MODE=rand SEED=7\n"
            "Fixed ctrl_*/data_* cases select their own mode; request_rand mixes both.\n"
            "Full matrix: make regress\n"
            "Waveform: make run_wave CASE=ctrl_write_burst\n"
            "Display this list: make list\n\n"
            + "\n".join(f"{i:2d}. {name}\n    {descriptions[name]}" for i, name in enumerate(names, 1))
            + "\n\nDefault simulator: VCS. Local override: SIMULATOR=verilator.\n"
            "AW/W/AR use the original axi_file_master.run(); no TB outstanding cap.\n"
            "Changing CASE reuses the binary. Changing DUT settings or WAVE may need compilation.\n"
            "Pattern ID width must match the compiled DUT.\n"
            "Legacy traffic patterns: neighbor, uniform_random, hotspot, directed.\n"
            "Run legacy suite: make legacy_regress\n")
        (root / "run_vcs.sh").write_text(r'''#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make regress "$@"
''')
        (root / "run_verilator.sh").write_text(r'''#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make regress SIMULATOR=verilator "$@"
''')
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
        (root / "VERSION.txt").write_text(
            "Issue: 83\nCheckout HEAD: " + revision + "\n"
            "Compare SHA256SUMS for exact packaged source/config/pattern bytes.\n")
        (root / "README.txt").write_text(
            "Issue #83 NMU injection/ejection synchronized simulation tree.\n"
            "Run all (VCS default): bash run_vcs.sh\n"
            "Same suite locally: make regress SIMULATOR=verilator\n"
            "Shared configuration: script/config.mk\n"
            "Run one: make run CASE=ctrl_write_single\n"
            "FSDB: make run_wave CASE=same_id_cross_dst_reorder\n"
            "Reuse binary: make sim CASE=ctrl_read_burst\n"
            "Open waveform and signal groups: make nWave CASE=same_id_cross_dst_reorder\n"
            "Waveform template: script/nWaveLog/signals.rc (@FSDB@ is replaced for the selected CASE).\n"
            "Package logs: make report\n"
            "Clean all build/wave/log/GUI artifacts: make clean (retains signal RC files).\n"
            "Requires an initialized VCS environment, GNU Make and Bash. Python 3.6+ is used for seeded pattern generation. No Git or network needed.\n"
            "Default: external ID width 8, AXI clock 10ns, NoC clock 10ns, B/R depth 128.\n"
            "No NSU, memory model or C++ DPI. REQ/RSP and DAT use the same standalone checker.\n"
            "See the retrieved VCS reports for the verified case/mode/seed matrix.\n")
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
    parser.add_argument("--block-patterns")
    parser.add_argument("--id-width", type=int, choices=(1, 3, 8), default=8)
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument("--output")
    destination.add_argument("--output-dir")
    args = parser.parse_args()
    package(args.run_dir, args.output_dir or args.output, directory=bool(args.output_dir), block_patterns=args.block_patterns, id_width=args.id_width)
