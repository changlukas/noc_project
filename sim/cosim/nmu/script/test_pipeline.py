#!/usr/bin/env python3
"""Run the existing focused RTL testbenches with VCS, without rebuilding DPI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--mixed-only", action="store_true")
options = parser.parse_args()
os.environ.setdefault("VCS_ARCH_OVERRIDE", "linux")
root = Path.cwd()
out = root / "build/buffer-pipeline/focused"
out.mkdir(parents=True, exist_ok=True)
lines = (root / "files.f").read_text().splitlines()
lines = [line for line in lines if not line.endswith(("tb_nmu_cosim.sv", "router_wrap.sv", "nsu_wrap.sv"))]
lines += ["repo/rtl/nmu/request_packetize/request_inject_tb_dut.sv"]
filelist = out / "files.f"
filelist.write_text("\n".join(lines) + "\n")
cases = [
    ("tb_nmu_request_packetize", {}),
    ("tb_nmu_request_packetize_stall", {}),
    ("tb_nmu_request_packetize_stress", {"REG_TYPE": 0, "NUM_DAT_VC": 2}),
    ("tb_nmu_request_packetize_stress", {"REG_TYPE": 1, "NUM_DAT_VC": 2}),
    ("tb_nmu_request_packetize_stress", {"REG_TYPE": 2, "NUM_DAT_VC": 6, "DAT_VC_MODE": 1}),
    ("tb_nmu_request_packetize_stress", {"REG_TYPE": 0, "NUM_DAT_VC": 1}),
    ("tb_nmu_request_packetize_stress", {"AW_REG_TYPE": 2, "W_REG_TYPE": 0, "AR_REG_TYPE": 1}),
    ("tb_nmu_request_packetize_stress", {"AW_REG_TYPE": 0, "W_REG_TYPE": 2, "AR_REG_TYPE": 1}),
    ("tb_nmu_response_depacketize", {"NUM_DAT_VC": 2, "DAT_RX_VC_DEPTH": 2}),
    ("tb_nmu_response_depacketize", {"NUM_DAT_VC": 6, "DAT_VC_MODE": 1, "DAT_RX_VC_DEPTH": 8}),
]
results = []
for top, params in cases:
    if options.mixed_only and "AW_REG_TYPE" not in params:
        continue
    group = "response_depacketize" if "response_depacketize" in top else "request_packetize"
    tb = "repo/rtl/nmu/{}/{}.sv".format(group, top.replace("tb_nmu_", "tb_"))
    key = hashlib.sha256((top + repr(params)).encode() + (root / tb).read_bytes()).hexdigest()[:8]
    work = out / (top + "_" + key)
    work.mkdir(exist_ok=True)
    args = ["vcs", "-full64", "-sverilog", "-assert", "svaext", "-override_timescale=1ns/1ps",
            "-f", str(filelist), tb, "-top", top, "-Mdir=" + str(work / "csrc"), "-o", str(work / "simv")]
    args += ["-pvalue+{}.{}={}".format(top, key, value) for key, value in params.items()]
    with (work / "compile.log").open("w") as log:
        result = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(str(work / "compile.log"))
    with (work / "sim.log").open("w") as log:
        result = subprocess.run([str(work / "simv")], stdout=log, stderr=subprocess.STDOUT, timeout=180)
    text = (work / "sim.log").read_text()
    passed = result.returncode == 0 and "PASS" in text and not any(x in text for x in ("Error:", "Fatal:", "ASSERT FAILED"))
    results.append({"top": top, "parameters": params, "pass": passed, "log": str(work / "sim.log")})
    (out / "results.json").write_text(json.dumps(results, indent=2))
    print(top, params, "PASS" if passed else "FAIL", flush=True)
    if not passed:
        print(text[-4000:], flush=True)
        raise SystemExit(1)
