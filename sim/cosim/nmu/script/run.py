#!/usr/bin/env python3
"""Fail closed on model, protocol and existing scoreboard diagnostics."""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--binary", required=True)
p.add_argument("--case", required=True)
p.add_argument("--report", required=True)
p.add_argument("--wave", action="store_true")
p.add_argument("--corrupt", action="store_true")
a = p.parse_args()
patterns = Path("patterns")
cases = (patterns / "cases.list").read_text().split()
if a.case not in cases:
    p.error("Unsupported co-simulation CASE '{}'. Use make list TESTBENCH=cosim "
            "from nmu-standalone/. Available cases: {}".format(a.case, ", ".join(cases)))
stim = patterns / a.case
for name in ("schedule.txt", "read.txt", "write.txt"):
    if not (stim / name).is_file():
        p.error("Incomplete co-simulation pattern '{}': missing {}. "
                "Prepare and synchronize the co-simulation environment again.".format(a.case, stim / name))
report = Path(a.report)
report.mkdir(parents=True, exist_ok=True)
args = [str(Path(a.binary).resolve()), "+stim_dir=" + str(stim.resolve())]
args += (stim / "schedule.txt").read_text().split()
if a.wave:
    args += ["+wave_file=" + str((report / (a.case + ".fsdb")).resolve())]
if a.corrupt:
    args += ["+corrupt_rsp"]
r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
log = r.stdout.decode(errors="replace")
log_path = report / (a.case + ("_corrupt" if a.corrupt else "") + ".log")
diagnostics = re.sub(r"Warning: [^\n]*\nMacro 'FFARN' is deprecated\. Use 'FF' instead\.\n", "", log)
failed = r.returncode != 0 or bool(re.search(r"(?im)^(?:Warning:|Error:|Fatal:)|\b(?:mismatch|does not match|Assertion failed)\b", diagnostics))
passed = "NMU_COSIM_COUNTS" in log and not failed
if a.corrupt:
    passed = (r.returncode == 0 and "Unexpected RData" in log and
              "NMU_COSIM_COUNTS" in log and not re.search(r"(?m)^(?:Fatal:|Error:)", log))
if passed:
    log += "NMU_COSIM_CORRUPTION_DETECTED\n" if a.corrupt else "NMU_COSIM_PASS\n"
log_path.write_text(log)
print(log)
if not passed:
    raise SystemExit("Co-simulation acceptance failed")
