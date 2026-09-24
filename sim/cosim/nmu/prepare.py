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


def prepare(rtl_stage, out, profile_path=None, extra_catalog=None):
    rtl_stage, out = Path(rtl_stage).resolve(), Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    profile = yaml.safe_load(Path(profile_path or ROOT / "sim/cosim/nmu/profile.yml").read_text())
    noc_id_width = profile.get("noc_id_width", 3)
    # Wrapper records use the generated AXI width; NMU external width can be overridden separately.
    pattern_id_width = profile.get("axi_id_width", noc_id_width)
    if type(noc_id_width) is not int or not 1 <= noc_id_width <= 8:
        raise ValueError("noc_id_width must be in [1, 8]")
    if type(pattern_id_width) is not int or not 1 <= pattern_id_width <= 8:
        raise ValueError("axi_id_width must be in [1, 8]")
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
                     "sim/dv/common_cells-1.37.0/src/delta_counter.sv",
                     "sim/dv/common_cells-1.37.0/src/counter.sv",
                     "sim/dv/common_cells-1.37.0/src/stream_delay.sv",
                     "sim/dv/axi-0.39.7/src/axi_delayer.sv",
                     "sim/dv/axi-0.39.7/src/axi_sim_mem.sv",
                     "sim/cosim/nmu/tb_nmu_cosim.sv"):
        copy(ROOT / relative, "repo/" + relative)
        source_list.append("repo/" + relative)
    for relative in (
        "rtl/nmu/ordering/tb_ordering.sv",
        "rtl/nmu/request_packetize/request_inject_tb_dut.sv",
        "rtl/nmu/request_packetize/tb_request_packetize.sv",
        "rtl/nmu/request_packetize/tb_request_packetize_stall.sv",
        "rtl/nmu/request_packetize/tb_request_packetize_stress.sv",
        "rtl/nmu/response_depacketize/tb_response_depacketize.sv",
    ):
        copy(ROOT / relative, "repo/" + relative)
    topo = ROOT / "sim/cosim/nmu/topology.yml"
    (out / "topology_pkg.sv").write_text(emit_sam_pkg(yaml.safe_load(topo.read_text())))
    (out / "files.f").write_text("\n".join(source_list) + "\n")
    patterns = ROOT / f"sim/test_patterns/cosim/generated/i{pattern_id_width}"
    cases = generate(patterns, topo, id_width=pattern_id_width, profile="cosim")
    cases += generate(patterns, topo, id_width=pattern_id_width, profile="cosim",
                      catalog=ROOT / "sim/test_patterns/cosim/cases.json")
    if extra_catalog:
        cases += generate(patterns, topo, id_width=pattern_id_width, profile="cosim", catalog=extra_catalog)
    (patterns / "cases.list").write_text("\n".join(cases) + "\n")
    (out / "pattern.txt").write_text("\n".join(cases) + "\n")
    for mode in ("control", "data", "rand"):
        mode_cases = generate(patterns / mode, topo, id_width=pattern_id_width, profile="cosim", mode=mode)
        mode_cases += generate(patterns / mode, topo, id_width=pattern_id_width, profile="cosim", mode=mode,
                               catalog=ROOT / "sim/test_patterns/cosim/cases.json")
        (patterns / mode / "cases.list").write_text("\n".join(mode_cases) + "\n")
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
    sys.path.insert(0, str(ROOT / "specgen"))
    constants = yaml.safe_load((ROOT / "specgen/source/constants.yaml").read_text())
    depth = profile["dat_credit_depth"]
    if not isinstance(depth, int) or depth < 2 or depth & (depth - 1):
        raise ValueError("DAT credit depth must be a power of two and at least 2")
    for key in ("ROUTER_VC_DEPTH", "NI_DAT_RX_VC_DEPTH"):
        constants["noc"][key]["default"] = profile["dat_credit_depth"]
    constants["axi"]["AXI_ID_WIDTH"]["default"] = noc_id_width
    constants["nsu"]["AXI_ID_WIDTH"]["default"] = noc_id_width
    constants["nsu"]["MAX_ACTIVE_IDS"]["default"] = 1 << noc_id_width
    from tools.elaborate.profile import emit as emit_profile
    emit_profile(ROOT, out, constants, noc_id_width)
    (out / "profile.yml").write_text(yaml.safe_dump(profile, sort_keys=False))
    (out / "profile.mk").write_text(f"AXI_ID_WIDTH ?= {pattern_id_width}\n")
    copy(ROOT / "sim/cosim/nmu/script/Makefile", "Makefile")
    copy(ROOT / "sim/cosim/nmu/script/run.py", "run.py")
    copy(ROOT / "sim/cosim/nmu/script/test_pipeline.py", "test_pipeline.py")
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
    parser.add_argument("--profile")
    parser.add_argument("--extra-catalog")
    args = parser.parse_args()
    prepare(args.rtl_stage, args.out, args.profile, args.extra_catalog)
