"""One approved pipeline boundary overlapping another request's KV restore."""

import argparse
import csv
import hashlib
import json
import re
import shlex
import subprocess
from pathlib import Path

import address_map
import axi_transaction_metrics
import control_traffic as control
import gen_tb_top
import gen_test_patterns as patterns
import packet_metrics


# The existing four-stage pipeline uses 1 MiB per sequence shard.
ACTIVATION_BYTES = 1024 * 1024
STAGE_PAIRS = ((2, 0), (3, 1), (6, 4), (7, 5))


def generate(directory, case):
    if case not in ("activation", "restore", "overlap"):
        raise ValueError("unknown shared-link comparison")
    plan = control.generate_kv(directory, "idle" if case == "activation" else "restore")
    directory = Path(directory)
    for job in plan["jobs"]:
        job.update(flow="restore", read=True, destination=16)
    if case != "restore":
        topology = gen_tb_top.load_topology(plan["config"])
        nodes, _, _ = gen_tb_top._nodes(topology)
        _, entries = address_map.pack_config(topology)
        bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "memory"}
        for source, target in STAGE_PAIRS:
            base = bases[nodes[target][3]]
            lines = []
            for offset in range(0, ACTIVATION_BYTES, plan["burst_bytes"]):
                address = base + offset
                lines += patterns._ax_fields(0, address,
                    plan["burst_bytes"] // plan["bus_bytes"] - 1,
                    plan["bus_bytes"].bit_length() - 1, True)
                for beat in range(address, address + plan["burst_bytes"], plan["bus_bytes"]):
                    data = bytes((a ^ (a >> 8) ^ (a >> 16) ^ 0xa5) & 255
                                 for a in range(beat, beat + plan["bus_bytes"]))
                    lines.append(f'0x{int.from_bytes(data, "little"):x} '
                                 f'0x{(1 << plan["bus_bytes"])-1:x} 0')
                plan["jobs"].append(dict(node=source, address=address,
                    bytes=plan["burst_bytes"], flow="activation", read=False, destination=target))
            (directory / f"node{source}" / "write.txt").write_text("\n".join(lines) + "\n")
            (directory / f"node{target}" / "kv_sink.txt").write_text(f"{base:x} {ACTIVATION_BYTES}\n")
    plan.update(case=case, activation_shard_bytes=ACTIVATION_BYTES,
                stage_pairs=STAGE_PAIRS, shared_dat_link="dat_1to0",
                source_policy="one outstanding transaction per source, both flows initially ready")
    (directory / "control_plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    return plan


def review(directory):
    directory = Path(directory)
    plan = json.loads((directory / "control_plan.json").read_text())
    log = (directory / "run.log").read_text()
    if re.search(r"%Error|Unexpected RData|Assertion failed|\$fatal", log):
        raise ValueError("simulation failed")
    jobs = plan["jobs"]
    endpoints = re.findall(r"\[kv_endpoint_done\] node=(\d+) writes=(\d+) reads=(\d+) sink_bytes=(\d+) cycle=(\d+)", log)
    if len(endpoints) != 20 or {int(e[0]) for e in endpoints} != set(range(20)):
        raise ValueError("missing endpoint completion")
    for node, writes, reads, sink, _ in endpoints:
        node = int(node)
        expected = (sum(j["node"] == node and not j["read"] for j in jobs),
                    sum(j["node"] == node and j["read"] for j in jobs),
                    sum(j["bytes"] for j in jobs if not j["read"] and j["destination"] == node))
        if (int(writes), int(reads), int(sink)) != expected:
            raise ValueError("transaction or destination byte count mismatch")
    issues = re.findall(r"\[kv_issue\] node=(\d+) flow=\s*kv address=([0-9a-f]+) cycle=(\d+)", log)
    if sorted((int(n), int(a, 16)) for n, a, _ in issues) != sorted((j["node"], j["address"]) for j in jobs):
        raise ValueError("source address/count mismatch")
    def events(name):
        rows = re.findall(r"\[kv_" + name + r"\] node=(\d+) cycle=(\d+)", log)
        if len(rows) != len({n for n, _ in rows}):
            raise ValueError("duplicate source event")
        return {int(n): int(c) for n, c in rows}
    start, done = events("start"), events("done")
    if start.keys() != done.keys() or start.keys() != {j["node"] for j in jobs}:
        raise ValueError("source timing coverage mismatch")
    perf = json.loads((directory / "perf.json").read_text())
    topology = gen_tb_top.load_topology(plan["config"])
    nodes, _, _ = gen_tb_top._nodes(topology)
    peripherals = gen_tb_top._peripherals(topology)
    endpoints = {i: (cid, port) for i, _, _, cid, port in gen_tb_top._endpoints(nodes, peripherals)}
    with (directory / "perf.json.packets.csv").open() as stream:
        packets = packet_metrics.measure(csv.DictReader(stream), endpoints, perf["window"])
    packet_metrics.check_link_counts(packets, perf["noc"]["links"], nodes, peripherals)
    if packets["incomplete_packets"] or packets["missing_deliveries"]:
        raise ValueError("incomplete packets")
    result = dict(case=plan["case"], status="PASS", flows={}, window=perf["window"])
    for flow in sorted({j["flow"] for j in jobs}):
        selected = [j for j in jobs if j["flow"] == flow]
        sources = {j["node"] for j in selected}
        first, last = min(start[n] for n in sources), max(done[n] for n in sources)
        deliveries = [p for p in packets["deliveries"] if p["plane"] == "DAT" and
                      ((flow == "restore" and p["source"] == 16 and p["destination"] in sources) or
                       (flow == "activation" and (p["source"], p["destination"]) in STAGE_PAIRS))]
        expected_samples = (sum(j["bytes"] for j in selected) // plan["bus_bytes"]
                            if flow == "restore" else len(selected))
        if len(deliveries) != expected_samples or any(not p["in_window"] for p in deliveries):
            raise ValueError("per-flow packet coverage mismatch")
        values = [p["latency_cycles"] for p in deliveries]
        result["flows"][flow] = dict(start_cycle=first, end_cycle=last,
            transfer_completion_cycles=last-first+1, delivered_payload_bytes=sum(j["bytes"] for j in selected),
            packet_latency=dict(mean_cycles=sum(values)/len(values), max_cycles=max(values), samples=len(values)))
    counts = {link["name"]: link["flit_count"] for link in perf["noc"]["links"]}
    read_jobs = sum(j["read"] for j in jobs)
    expected_injection = dict(req=read_jobs, rsp=len(jobs)-read_jobs,
        dat=sum(j["bytes"] for j in jobs)//plan["bus_bytes"] + len(jobs)-read_jobs)
    for plane, expected in expected_injection.items():
        injected = sum(value for name, value in counts.items() if
                       re.fullmatch(plane+r"_inject_\d+", name) or
                       re.fullmatch(plane+r"_node\d+\.y_to_node\d+\.router", name))
        if injected != expected:
            raise ValueError(f"unexpected {plane} injection count")
    # XY: C0 to D0 and M0 to A0/A2 share 1 to 0, in the same direction.
    expected_shared = (ACTIVATION_BYTES // plan["bus_bytes"] + ACTIVATION_BYTES // plan["burst_bytes"]
                       if plan["case"] != "restore" else 0)
    expected_shared += 2 * control.KV_SHARD_BYTES // plan["bus_bytes"] if plan["case"] != "activation" else 0
    if counts[plan["shared_dat_link"]] != expected_shared:
        raise ValueError("shared link traffic does not match mapped flows")
    # Directed-mode window metadata starts at zero, before measurement enable.
    # The switch counter counts actual measured clock slots, excluding reset.
    shared_switch = next(s for s in perf["noc"]["router_dat_switches"]
                         if s["router"] == "router_1" and s["port"] == "WEST")
    duration = shared_switch["samples"]
    if "router_dat_allocations" in perf["noc"]:
        axi = axi_transaction_metrics.read_run(directory, topology, perf["window"], duration)
        if any(t["status"] != "complete" for t in axi["transactions"]):
            raise ValueError("incomplete AXI request")
        for flow, metrics in result["flows"].items():
            sources = {j["node"] for j in jobs if j["flow"] == flow}
            selected = [t for t in axi["transactions"] if t["node"] in sources]
            direction = "read" if flow == "restore" else "write"
            metrics["admission_latency"] = axi_transaction_metrics.summarize(selected)["metrics"][direction + "_admission"]
        for wait in perf["noc"]["router_dat_allocations"]:
            total, occupied, full, both = (wait[k] for k in
                ("waiting_cycles", "occupied_cycles", "input_full_cycles", "occupied_input_full_cycles"))
            if not (0 <= both <= min(occupied, full) <= max(occupied, full) <= total <= duration
                    and occupied + full - both <= total):
                raise ValueError("invalid VC allocation wait subsets")
        (directory / "axi_transactions.json").write_text(json.dumps(axi, indent=2) + "\n")
    result["shared_link"] = dict(name=plan["shared_dat_link"], flits=expected_shared,
        window_cycles=duration, utilization_percent=100*expected_shared/duration,
        arbitration_wait_vc_cycles=shared_switch["arbitration_wait_vc_cycles"])
    if plan["case"] == "overlap":
        intervals = list(result["flows"].values())
        if max(f["start_cycle"] for f in intervals) >= min(f["end_cycle"] for f in intervals):
            raise ValueError("flows did not overlap")
    (directory / "packet_latencies.json").write_text(json.dumps(packets, indent=2) + "\n")
    return result


def run(directory, binary, cases=("activation", "restore", "overlap")):
    directory, binary = directory.resolve(), binary.resolve()
    manifest = Path(__file__).with_name("emit_result_manifest.py")
    results = {}
    for case in cases:
        target = directory / case
        generate(target, case)
        command = [str(binary), f"+stim_dir={target}", "+kv_overlap", "+control_offset=0",
            "+injection_mode=0", "+verilator+seed+1", "+timeout_cycles=2000000", "+packet_trace",
            f"+sam_config=/workspace/sim/configs/{control.KV_CONFIG}.yml",
            f"+perf_out={target / 'perf.json'}", f"+perf_scenario=activation_kv_{case}"]
        subprocess.run(["python3", str(manifest), "--repo-root", "/workspace",
                        "--write-source-patch", "--out", str(target / "source.patch")], check=True)
        print(f"Running {case}", flush=True)
        with (target / "run.log").open("w") as output:
            subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=600)
        result = review(target)
        result["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
        subprocess.run(["python3", str(manifest), "--repo-root", "/workspace",
            "--out", str(target / "manifest.json"), "--config", f"sim/configs/{control.KV_CONFIG}.yml",
            "--simulator", "verilator", "--seed", "1", "--command", shlex.join(command),
            "--source-patch", str(target / "source.patch")], check=True)
        (target / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        results[case] = result
        print(json.dumps(result), flush=True)
    (directory / "summary.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--review-only", action="store_true")
    parser.add_argument("--case", choices=("activation", "restore", "overlap"))
    args = parser.parse_args()
    if args.review_only:
        print(json.dumps(review(args.directory), indent=2))
    elif args.binary:
        run(args.directory, args.binary, (args.case,) if args.case else ("activation", "restore", "overlap"))
    else:
        parser.error("provide --binary or --review-only")
