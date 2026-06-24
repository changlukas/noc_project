#!/usr/bin/env python3
"""num_vc=2 multi-VC co-sim driver (gate-3 of the wire-level multi-VC task).

NUM_VC is an elaboration-time SV localparam threaded from the topology config, so
it CANNOT be set by a runtime plusarg — a num_vc=2 run requires building tb_top
from a num_vc=2 topology. This driver:

  1. generates tb_top from sim/topologies/mesh_2x1_vc2.yaml into an ISOLATED
     variant .sv (build/verilator/tb_top_vc2.sv) so the committed num_vc=1
     tb_top.sv (and its gen_tb_top --check gate) is never touched;
  2. verilates+compiles it into an ISOLATED obj_dir_vc2 (OBJDIR_SUFFIX=_vc2) so
     the num_vc=1 Vtb_top used by sim-regress is never clobbered;
  3. runs the multi-VC driver scenario — AX4-BAS-004_conformity_write_read, a
     NON-burst Mode-A read/write-split (len=0; AW/W -> write_vc=0, AR -> read_vc=1
     at num_vc=2), so the VC stack is genuinely exercised and the result is not
     confounded by the known wire-level burst bug (SP2);
  4. asserts a non-vacuous PASS: exactly one scenario ran AND the tb PASS line
     (scoreboard clean + reads checked) appeared.

Run: python3 sim/run_vc2_driver.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM = ROOT / "sim"
TOPOLOGY = "mesh_2x1_vc2"
DRIVER_SCN = "AX4-BAS-004_conformity_write_read"  # non-burst Mode-A write/read split
PASS = "PASS: scenario complete, scoreboard clean"

VL_BUILD = ROOT / "build" / "verilator"
TB_TOP_VC2 = VL_BUILD / "tb_top_vc2.sv"
FILELIST_VC2 = VL_BUILD / "filelist_vc2.f"
OBJDIR_SUFFIX = "_vc2"
EXE = VL_BUILD / f"obj_dir{OBJDIR_SUFFIX}" / "Vtb_top.exe"
EXE_NIX = VL_BUILD / f"obj_dir{OBJDIR_SUFFIX}" / "Vtb_top"


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, **kw)


def main() -> int:
    VL_BUILD.mkdir(parents=True, exist_ok=True)

    # 1. Generate the num_vc=2 tb_top into the isolated variant path.
    r = run([sys.executable, str(SIM / "tools" / "gen_tb_top.py"),
             "--topology", TOPOLOGY, "--out", str(TB_TOP_VC2)])
    if r.returncode != 0:
        print("FAIL: vc2 tb_top generation failed")
        return 1

    # 2. Build via the verilator Makefile with variant overrides. TB_TOP_SV points
    #    the tb_top recipe at the variant .sv; the filelist swaps the default
    #    tb_top.sv entry for it; OBJDIR_SUFFIX isolates the obj_dir + EXE.
    make = ["make", "-C", str(SIM / "verilator"),
            f"OBJDIR_SUFFIX={OBJDIR_SUFFIX}",
            f"TOPOLOGY={TOPOLOGY}",
            f"TB_TOP_SV={TB_TOP_VC2}",
            f"FILELIST_F={FILELIST_VC2}",
            "PYTHON3=python3"]
    r = run(make)
    if r.returncode != 0:
        print("FAIL: vc2 verilator build failed")
        return 1

    exe = EXE if EXE.exists() else (EXE_NIX if EXE_NIX.exists() else None)
    if exe is None:
        print(f"FAIL: Vtb_top (vc2) not built at {EXE}")
        return 1

    # 3. Run the non-burst Mode-A driver scenario (node0/node1 coordinate variants).
    d = SIM / "test_patterns" / DRIVER_SCN
    n0 = d / "node0" / "scenario.yaml"
    n1 = d / "node1" / "scenario.yaml"
    if not (n0.exists() and n1.exists()):
        print(f"FAIL: driver scenario {DRIVER_SCN} missing node0/node1 variants")
        return 1

    out = VL_BUILD / "output_vc2" / DRIVER_SCN
    out.mkdir(parents=True, exist_ok=True)
    r = run([str(exe),
             f"+scenario_node0={n0}", f"+scenario_node1={n1}",
             f"+perf_out={out / 'perf.json'}", f"+perf_scenario={DRIVER_SCN}"],
            capture_output=True, text=True, cwd=str(VL_BUILD))

    # 4. Non-vacuous PASS assertion (run-count == 1, tb PASS line present).
    ran = 1
    ok = r.returncode == 0 and PASS in r.stdout
    print(f"{'PASS' if ok else 'FAIL'}  {DRIVER_SCN}  (num_vc=2)")
    if not ok:
        sys.stdout.write(r.stdout[-1200:] + r.stderr[-600:])
    print(f"\nvc2 driver: {1 if ok else 0}/{ran} passed (num_vc=2, scenario={DRIVER_SCN})")
    if ran != 1:
        print("FAIL: expected exactly 1 vc2 driver scenario to run")
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
