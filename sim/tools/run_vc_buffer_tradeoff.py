#!/usr/bin/env python3
"""Run the approved VC/router-depth Outstanding trade-off matrix."""

import argparse
import pathlib
import re
import shutil
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[2]
CONSTANTS = ROOT / "specgen" / "source" / "constants.yaml"
VERILATOR = ROOT / "sim" / "verilator"
MANIFEST_TOOL = ROOT / "sim" / "tools" / "emit_result_manifest.py"
SOURCE_PATCH = VERILATOR / "output" / "source.patch"
CONFIGS = (
    (1, 8), (4, 8), (8, 8),
    (1, 32), (2, 16), (2, 32), (4, 16),
)
RESULTS_PER_CONFIG = 5
SOURCE_OUTSTANDING_DEPTH = 32
MAX_TXNS_PER_ID = 32


def set_default(source, name, value):
    marker = f"  {name}:\n"
    if source.count(marker) != 1:
        raise ValueError(f"expected one {name} block")
    start = source.index(marker)
    tail_start = start + len(marker)
    next_block = re.search(r"\n  \S", source[tail_start:])
    end = tail_start + next_block.start() if next_block else len(source)
    block = source[start:end]
    block, count = re.subn(r"(^    default:\s*)\d+", rf"\g<1>{value}", block,
                           count=1, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"expected one default in {name}")
    return source[:start] + block + source[end:]


def configure_candidate(source, vc, depth):
    configured = set_default(source, "DAT_NUM_VC", vc)
    configured = set_default(configured, "ROUTER_VC_DEPTH", depth)
    return set_default(configured, "NI_DAT_RX_VC_DEPTH", depth)


def clear_verilator_objects(build_root):
    build_root = build_root.resolve()
    if build_root != pathlib.Path("/home/lucas/noc_build/verilator"):
        raise ValueError(f"refusing unexpected build root: {build_root}")
    for path in build_root.glob("obj_dir_*"):
        resolved = path.resolve()
        if resolved.parent != build_root:
            raise ValueError(f"refusing unexpected object directory: {resolved}")
        shutil.rmtree(resolved)


def run(command):
    subprocess.run(command, cwd=ROOT, check=True)


def regenerate_params():
    for target in ("cpp", "sv"):
        run(["python3", "specgen/tools/codegen.py", "--target", target,
             "--domain", "params"])


def candidate_repro_command(vc, depth, seed, build_root):
    tag = f"v{vc}_b{depth}"
    apply_command = (
        "python3 sim/tools/run_vc_buffer_tradeoff.py --apply-only "
        f"--vc {vc} --depth {depth} --build-root {build_root}"
    )
    run_command = (
        "make -C sim/verilator sim-outstanding-ai-tradeoff "
        f"CONFIG=mesh_4x4 SEED={seed} TRADEOFF_CONFIG_TAG={tag} "
        f"SOURCE_OUTSTANDING_DEPTH={SOURCE_OUTSTANDING_DEPTH} "
        f"MAX_TXNS_PER_ID={MAX_TXNS_PER_ID}"
    )
    return f"{apply_command} && {run_command}"


def tradeoff_make_command(vc, depth, seed, build_root):
    tag = f"v{vc}_b{depth}"
    repro_command = candidate_repro_command(vc, depth, seed, build_root)
    return [
        "make", "-C", "sim/verilator", "sim-outstanding-ai-tradeoff",
        "CONFIG=mesh_4x4", f"SEED={seed}", f"TRADEOFF_CONFIG_TAG={tag}",
        f"SOURCE_OUTSTANDING_DEPTH={SOURCE_OUTSTANDING_DEPTH}",
        f"MAX_TXNS_PER_ID={MAX_TXNS_PER_ID}",
        f"REPRO_COMMAND={repro_command}",
    ]


def candidate_manifest_refresh_command(vc, depth, seed, build_root):
    tag = f"v{vc}_b{depth}"
    return [
        "python3", str(MANIFEST_TOOL),
        "--refresh-root", str(VERILATOR / "output" / "tradeoff" / tag),
        "--repo-root", str(ROOT), "--config", "sim/configs/mesh_4x4.yml",
        "--simulator", "verilator", "--source-patch", str(SOURCE_PATCH),
        f"--command={candidate_repro_command(vc, depth, seed, build_root)}",
    ]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--build-root", type=pathlib.Path,
                        default=pathlib.Path("/home/lucas/noc_build/verilator"))
    parser.add_argument("--apply-only", action="store_true")
    parser.add_argument("--vc", type=int)
    parser.add_argument("--depth", type=int)
    args = parser.parse_args(argv)

    original = CONSTANTS.read_text(encoding="utf-8")
    if args.apply_only:
        if args.vc is None or args.depth is None:
            parser.error("--apply-only requires --vc and --depth")
        if (args.vc, args.depth) not in CONFIGS:
            parser.error("--vc/--depth must select an approved candidate")
        configured = configure_candidate(original, args.vc, args.depth)
        CONSTANTS.write_text(configured, encoding="utf-8", newline="\n")
        regenerate_params()
        clear_verilator_objects(args.build_root)
        print(f"[tradeoff] applied VC={args.vc}, depth={args.depth}", flush=True)
        return
    if args.vc is not None or args.depth is not None:
        parser.error("--vc and --depth are only valid with --apply-only")

    try:
        for vc, depth in CONFIGS:
            tag = f"v{vc}_b{depth}"
            output = VERILATOR / "output" / "tradeoff" / tag
            configured = configure_candidate(original, vc, depth)
            CONSTANTS.write_text(configured, encoding="utf-8", newline="\n")
            print(f"[tradeoff] configure VC={vc}, depth={depth}", flush=True)
            regenerate_params()
            if len(list(output.rglob("result.csv"))) == RESULTS_PER_CONFIG:
                run(candidate_manifest_refresh_command(
                    vc, depth, args.seed, args.build_root))
                print(f"[tradeoff] skip complete {tag}", flush=True)
                continue
            clear_verilator_objects(args.build_root)
            run(tradeoff_make_command(vc, depth, args.seed, args.build_root))
            count = len(list(output.rglob("result.csv")))
            if count != RESULTS_PER_CONFIG:
                raise RuntimeError(
                    f"{tag}: expected {RESULTS_PER_CONFIG} result rows, found {count}")
    finally:
        CONSTANTS.write_text(original, encoding="utf-8", newline="\n")
        regenerate_params()
        run(["python3", "specgen/tools/codegen.py", "--check"])
        print("[tradeoff] restored shipped VC=2, Router/NI RX depth=8", flush=True)


if __name__ == "__main__":
    main()
