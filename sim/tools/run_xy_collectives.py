#!/usr/bin/env python3
"""Run the approved matrix sequentially using the existing co-sim acceptance path."""
import argparse
import json
from pathlib import Path
import subprocess
import time
import perf_report
from xy_collective_model import calibrate, estimate

ROOT=Path(__file__).resolve().parents[2]
KINDS=("gather","all_gather","reduce_scatter","reduce","all_reduce")


def cases(variant="paper"):
    result=[]
    def add(category,op,length,bp=0):
        result.append(dict(category=category,operation=op,bytes=length,backpressure=bp,
                           tag=f"{category}_{op}_b{length}_bp{bp}"))
    prefix = "xy_direct_" if variant == "direct" else "xy_"
    for p in (4,16):
        for kind in KINDS:
            add("functional",f"{prefix}{kind}_p{p}",64)
    for kind in KINDS:
        add("backpressure",f"{prefix}{kind}_p16",16384,1)
    add("descriptor","xy_descriptor_chain",1048576,1)
    for src,dst in ((0,1),(1,0),(0,4),(4,0)):
        for length in (64,4096,65536):
            add("calibration",f"xy_cal_{src}_{dst}",length)
    for src,dst in ((0,2),(2,0),(0,8),(8,0)):
        for length in (64,65536):
            add("calibration",f"xy_cal_{src}_{dst}",length)
    for dst in (1,4):
        for length in (64,4096,65536):
            add("calibration",f"xy_duplex_0_{dst}",length)
    for src,dst in ((0,15),(15,0)):
        add("heldout",f"xy_cal_{src}_{dst}",4096)
    for p in (4,16):
        for kind in KINDS:
            for length in (256,16384,262144):
                add("measurement",f"{prefix}{kind}_p{p}",length)
    return result


def write_json(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(data,indent=2,default=str,allow_nan=False)+"\n")


def prepare_model(output, status, calibration_output=None):
    calibration_cases=[c for c in cases() if c["category"] in ("calibration","heldout")]
    accepted={r["tag"] for r in status if r["status"]=="passed"}
    calibration_output = calibration_output or output
    if calibration_output == output and any(c["tag"] not in accepted for c in calibration_cases):
        raise ValueError("complete independent calibration before primary measurements")
    rows=perf_report.collect_operations([calibration_output/c["tag"] for c in calibration_cases])
    model=calibrate(rows)
    for section in ("calibration","heldout"):
        for sample in model[section]:
            sample["directory"]=str(Path(sample["directory"]).resolve())
    write_json(output/"model.json",model)
    return model


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,default=ROOT/"sim/verilator/output/xy_direct_20260916")
    parser.add_argument("--variant", choices=("direct", "paper"), default="direct")
    parser.add_argument("--calibration-output", type=Path)
    parser.add_argument("--resume",action="store_true")
    parser.add_argument("--operation", help="Run only the named operation from the selected matrix")
    parser.add_argument("--smc-analysis-only", type=Path, metavar="DESTINATION",
                        help="Reconstruct NI-to-delivery SMC results from accepted --output traces")
    parser.add_argument("--expected-primary", type=int, default=30, help="Expected primary count for SMC analysis")
    parser.add_argument("--category",choices=("functional","backpressure","descriptor","calibration","heldout","measurement"))
    args=parser.parse_args()
    output=args.output.resolve()
    output.relative_to(ROOT/"sim/verilator/output")
    if args.smc_analysis_only:
        if args.variant != "direct":
            parser.error("SMC mapping is approved for direct XY only")
        import sys
        subprocess.run([sys.executable, str(ROOT/"sim/tools/xy_smc_analysis.py"),
                        str(output), "--output", str(args.smc_analysis_only),
                        "--expected-primary", str(args.expected_primary)], check=True)
        return
    output.mkdir(parents=True,exist_ok=True)
    matrix = cases(args.variant)
    calibration_output = args.calibration_output.resolve() if args.calibration_output else None
    run_matrix = [c for c in matrix if not calibration_output or c["category"] not in ("calibration", "heldout")]
    selected=[c for c in run_matrix if args.category is None or c["category"]==args.category]
    if args.operation:
        selected = [c for c in selected if c["operation"] == args.operation]
        if not selected:
            parser.error("operation does not match the selected matrix/category")
    status_path=output/"batch_status.json"
    status=json.loads(status_path.read_text()) if args.resume and status_path.exists() else []
    by_tag={r["tag"]:r for r in status}
    base=["make","-C","sim/verilator","CONFIG=mesh_4x4_dual_edge_large","DMA=1","DMA_DEPENDENT=1",
          "DMA_RW=write","SEED=1","BUILD_JOBS=1","SIM_OPT=-O2",
          "BUILD_ROOT=/home/agent/noc_sim_build","VL_BUILD=/home/agent/noc_sim_build/verilator",
          "STIM_BASE=test_patterns/xy_collectives",f"OUTPUT_ROOT={output}"]
    model=None
    for case in selected:
        if case["category"]=="measurement" and model is None:
            model=prepare_model(output,status,calibration_output)
            print("CALIBRATED",model["endpoint_mode"],flush=True)
        directory=output/case["tag"]
        if args.resume and by_tag.get(case["tag"],{}).get("status")=="passed":
            perf_report.collect_operations([directory])
            continue
        record={**case,"status":"running","start":time.time()}
        if case["tag"] in by_tag:
            status.remove(by_tag[case["tag"]])
        status.append(record)
        by_tag[case["tag"]]=record
        write_json(status_path,status)
        print("START",case["tag"],flush=True)
        command=base+[f"DMA_OPERATION={case['operation']}",f"DMA_LENGTH={case['bytes']}",
                      f"DMA_BACKPRESSURE={case['backpressure']}",f"SIM_TAG={case['tag']}"]
        try:
            with (output/(case["tag"]+"_build.log")).open("w") as log:
                for goal in ("gen","sim"):
                    subprocess.run(command+[goal],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
            row=perf_report.collect_operations([directory])[0]
            write_json(directory/"validated.json",row)
            record.update(status="passed",end=time.time(),cycles=row["duration_cycles"])
            print("PASS",case["tag"],row["duration_cycles"],flush=True)
        except Exception as exc:
            record.update(status="failed",end=time.time(),error=str(exc))
            write_json(status_path,status)
            raise
        write_json(status_path,status)
    accepted={r["tag"] for r in status if r["status"]=="passed"}
    if all(c["tag"] in accepted for c in run_matrix):
        rows=perf_report.collect_operations([output/c["tag"] for c in run_matrix])
        if model is None:
            model=prepare_model(output,status,calibration_output)
        primary=[]
        for case,row in zip(run_matrix,rows):
            if case["category"]!="measurement":
                continue
            predicted=estimate(row["contract"],model,model["endpoint_mode"])
            primary.append({**row,"directory":case["tag"],"estimated_time_cycles":predicted,
                            "slowdown":row["duration_cycles"]/predicted,"estimate_status":"calibrated_transport_extension",
                            "model_id":model["model_id"]})
        write_json(output/"measurements.json",dict(schema=1,records=primary))
        print("COMPLETE",len(rows),"accepted cases",len(primary),"primary results",flush=True)

if __name__=="__main__":
    main()
