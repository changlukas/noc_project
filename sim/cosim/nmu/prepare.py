#!/usr/bin/env python3
"""Prepare co-simulation sources using the existing standalone RTL dependency list."""
import argparse
import hashlib
from pathlib import Path
import shutil
import json
import sys
import yaml

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "sim/tools"))
from gen_tb_top import emit_sam_pkg, num_vc
from gen_standalone_patterns import generate


def prepare(rtl_stage, out):
    rtl_stage, out = Path(rtl_stage).resolve(), Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_list = []
    def copy(source, relative):
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_bytes() != source.read_bytes():
            shutil.copyfile(source, target)
    for line in (rtl_stage / "files.f").read_text().splitlines():
        if line.startswith("+incdir+"):
            relative = line[len("+incdir+"):]
            source = ROOT / relative[5:] if relative.startswith("repo/") else rtl_stage / relative
            for path in source.rglob("*"):
                if path.is_file():
                    copy(path, str(Path(relative) / path.relative_to(source)))
            source_list.append(line)
            continue
        flag = "-v " if line.startswith("-v ") else ""
        relative = line[len(flag):]
        if Path(relative).name in ("tb_nmu_standalone.sv", "tb_nmu_elaborate.sv"):
            continue
        if Path(relative).name == "topology_pkg.sv":
            source_list.append("topology_pkg.sv")
            continue
        source = ROOT / relative[5:] if relative.startswith("repo/") else rtl_stage / relative
        copy(source, relative)
        source_list.append(flag + relative)
    for relative in (f"specgen/generated/sv/noc_types_pkg_vc{num_vc()}.sv",
                     "ref_model/top/router_wrap.sv", "ref_model/top/nsu_wrap.sv",
                     "sim/dv/axi-0.39.7/src/axi_sim_mem.sv",
                     "sim/cosim/nmu/tb_nmu_cosim.sv"):
        copy(ROOT / relative, "repo/" + relative)
        source_list.append("repo/" + relative)
    topo = ROOT / "sim/cosim/nmu/topology.yml"
    (out / "topology_pkg.sv").write_text(emit_sam_pkg(yaml.safe_load(topo.read_text())))
    (out / "files.f").write_text("\n".join(source_list) + "\n")
    patterns = ROOT / "sim/test_patterns/cosim/generated/i3"
    cases = generate(patterns, topo, id_width=3, profile="cosim")
    (out / "pattern.txt").write_text("\n".join(cases) + "\n")
    for path in patterns.rglob("*"):
        if path.is_file():
            copy(path, str(Path("patterns") / path.relative_to(patterns)))
    for path in (ROOT / "sim/cosim/nmu").glob("*"):
        if path.is_file():
            copy(path, path.name)
    for directory in ("ref_model/dpi", "ref_model/c_model/include", "ref_model/c_model/tests/common",
                      "specgen/generated/cpp"):
        for path in (ROOT / directory).rglob("*"):
            if path.is_file():
                copy(path, "repo/" + str(path.relative_to(ROOT)))
    yaml_source = ROOT / "build/cmodel/_deps/yaml-cpp-src"
    for directory in ("include", "src"):
        for path in (yaml_source / directory).rglob("*"):
            if path.is_file():
                copy(path, "deps/yaml-cpp/" + str(path.relative_to(yaml_source)))
    for path in yaml_source.glob("LICENSE*"):
        copy(path, "deps/yaml-cpp/" + path.name)
    # One profile drives both generated languages and every DAT receiver.
    sys.path.insert(0, str(ROOT / "specgen/tools"))
    from elaborate import cpp_params, sv_params
    constants = yaml.safe_load((ROOT / "specgen/source/constants.yaml").read_text())
    profile = yaml.safe_load((ROOT / "sim/cosim/nmu/profile.yml").read_text())
    depth = profile["dat_credit_depth"]
    if not isinstance(depth, int) or depth < 2 or depth & (depth - 1):
        raise ValueError("DAT credit depth must be a power of two and at least 2")
    for key in ("ROUTER_VC_DEPTH", "NI_DAT_RX_VC_DEPTH"):
        constants["noc"][key]["default"] = profile["dat_credit_depth"]
    constants_path = out / "constants.yml"
    constants_path.write_text(yaml.safe_dump(constants, sort_keys=False))
    for emitter, relative in ((cpp_params, "repo/specgen/generated/cpp/ni_params.h"),
                              (sv_params, "repo/specgen/generated/sv/ni_params_pkg.sv")):
        (out / relative).write_text("// Generated from co-simulation constants.yml\n" +
                                   emitter.emit(constants_path, "cosim"))
    copy(ROOT / "sim/cosim/nmu/script/Makefile", "Makefile")
    copy(ROOT / "sim/cosim/nmu/script/run.py", "run.py")
    copy(ROOT / "sim/cosim/nmu/script/build_key.py", "build_key.py")
    names = [path for path in out.rglob("*") if path.is_file() and
             path.name != "SHA256SUMS" and "build" not in path.relative_to(out).parts]
    (out / "SHA256SUMS").write_text("".join(
        hashlib.sha256(path.read_bytes()).hexdigest() + "  " +
        str(path.relative_to(out)) + "\n" for path in sorted(names)))
    print(out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--rtl-stage", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    prepare(args.rtl_stage, args.out)
