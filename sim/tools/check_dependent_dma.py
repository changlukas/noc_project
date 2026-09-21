#!/usr/bin/env python3
"""Run data and dependency fault checks against a built All-Gather DMA TB."""

import argparse
import json
from pathlib import Path
import re
import subprocess

import dependent_dma_plan
import gen_tb_top


def run_checks(binary, topology, shard_bytes, out_root, direction, require_backpressure=False,
               operation="all_gather", timeout_seconds=60):
    if timeout_seconds <= 0:
        raise ValueError("timeout_seconds must be positive")
    topo = gen_tb_top.load_topology(topology)
    nodes, _, _ = gen_tb_top._nodes(topo)
    endpoint_count = len(nodes) + len(gen_tb_top._peripherals(topo))
    config = gen_tb_top.ROOT / "sim" / "configs" / f"{topology}.yml"
    cases = (direction, "early_forward", "wrong_source", "missing_dependency",
             "extra_dependency", "future_local_dependency")
    if operation == "kv_restore_consume":
        cases += ("early_consume",)
    if operation == "shared_fetch_multicast":
        cases += ("missing_user", "wrong_user", "wide_user", "extra_user")
    for case in cases:
        plan = dependent_dma_plan.build_plan(topo, shard_bytes, direction, operation)
        if case == "early_forward" and not any(t.prerequisite is not None for t in plan.transfers):
            continue
        directory = (Path(out_root) / case).resolve()
        dependent_dma_plan.emit(plan, directory, endpoint_count)
        first_node = plan.transfers[0].issuer
        dependencies = directory / f"node{first_node}" / "dependencies.txt"
        jobs = directory / f"node{first_node}" / "jobs.txt"
        expected_error = None
        if case in ("missing_user", "wrong_user", "wide_user", "extra_user"):
            transfer = next(t for t in plan.transfers if t.user)
            issued = [t for t in plan.transfers if t.issuer == transfer.issuer]
            path = directory / f"node{transfer.issuer}" / "users.txt"
            lines = path.read_text().splitlines()
            if case == "missing_user":
                path.unlink()
                expected_error = "cannot open"
            elif case == "extra_user":
                path.write_text("\n".join(lines + ["0"]) + "\n")
                expected_error = "excess USER records"
            else:
                lines[issued.index(transfer)] = "0" if case == "wrong_user" else str(1 << 63)
                path.write_text("\n".join(lines) + "\n")
                expected_error = "mismatch" if case == "wrong_user" else "exceeds interface width"
        elif case == "early_forward":
            for node in range(endpoint_count):
                path = directory / f"node{node}" / "dependencies.txt"
                path.write_text("".join("0\n" for _ in path.read_text().splitlines()))
            expected_error = "dependency violation"
        elif case == "early_consume":
            for node in range(endpoint_count):
                issued = [t for t in plan.transfers if t.issuer == node]
                path = directory / f"node{node}" / "dependencies.txt"
                lines = path.read_text().splitlines()
                for index, transfer in enumerate(issued):
                    if transfer.source.node == transfer.destination.node:
                        lines[index] = "0"
                path.write_text("\n".join(lines) + ("\n" if lines else ""))
            expected_error = "dependency violation"
        elif case == "wrong_source":
            fields = jobs.read_text().splitlines()
            fields[1] = hex(int(fields[1], 0) + 1)
            jobs.write_text("\n".join(fields) + "\n")
            expected_error = "mismatch"
        elif case == "missing_dependency":
            dependencies.unlink()
            expected_error = "cannot open"
        elif case == "extra_dependency":
            dependencies.write_text(dependencies.read_text() + "0\n")
            expected_error = "excess dependency records"
        elif case == "future_local_dependency":
            lines = dependencies.read_text().splitlines()
            lines[0] = f"1 {first_node} 1"
            dependencies.write_text("\n".join(lines) + "\n")
            expected_error = "unissued local job"
        command = [str(Path(binary).resolve()), f"+stim_dir={directory}",
                   "+dependent_jobs=1", f"+sam_config={config}", "+verilator+seed+1",
                   f"+perf_out={directory / 'perf.json'}", f"+perf_scenario={case}",
                   f"+operation_out={directory / 'operation.json'}",
                   "+timeout_cycles=100000"]
        if operation == "shared_fetch_multicast":
            command.append("+dma_job_users=1")
        operation_path = directory / "operation.json"
        operation_path.unlink(missing_ok=True)
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=timeout_seconds)
        except subprocess.TimeoutExpired as error:
            def decoded(value):
                return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
            (directory / "run.log").write_text(decoded(error.stdout) + decoded(error.stderr))
            raise RuntimeError(f"{case}: wall-clock timeout after {timeout_seconds}s, see {directory / 'run.log'}") from error
        output = result.stdout + result.stderr
        (directory / "run.log").write_text(output)
        if expected_error is None:
            expected = dependent_dma_plan.pass_marker(plan)
            if result.returncode != 0 or expected not in output:
                raise RuntimeError(f"{case}: expected byte-checked PASS, see {directory / 'run.log'}")
            operation_result = json.loads(operation_path.read_text())
            dependent_dma_plan.validate_result(operation_result, plan)
            dependent_dma_plan.validate_perf_window(operation_result, json.loads((directory / "perf.json").read_text()))
            if require_backpressure and not any(int(value) > 0 for value in re.findall(
                    r"R backpressure held (\d+) cycles", output)):
                raise RuntimeError(f"{case}: no observed R backpressure")
        elif result.returncode == 0 or expected_error not in output or "PASS:" in output:
            raise RuntimeError(f"{case}: expected rejection ({expected_error}), see {directory / 'run.log'}")
        elif operation_path.exists():
            raise RuntimeError(f"{case}: failed run produced an operation result")
        print(f"{case}: {'PASS' if expected_error is None else 'fault rejected'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--topology", default="mesh_4x4")
    parser.add_argument("--shard-bytes", required=True, type=int,
                        help="Must match the compiled TB's DMA_LENGTH")
    parser.add_argument("--out", required=True)
    parser.add_argument("--direction", choices=("read", "write"), required=True,
                        help="Must match the compiled TB's DMA_RW")
    parser.add_argument("--require-backpressure", action="store_true")
    parser.add_argument("--timeout-seconds", type=int, default=60,
                        help="Per-case wall-clock limit, independent of the simulation cycle limit")
    parser.add_argument("--operation", choices=dependent_dma_plan.OPERATIONS, default="all_gather")
    args = parser.parse_args()
    run_checks(args.binary, args.topology, args.shard_bytes, args.out,
               args.direction, args.require_backpressure, args.operation, args.timeout_seconds)


if __name__ == "__main__":
    main()
