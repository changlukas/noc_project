"""Approved 8-byte command, status and release stimulus for the file master."""

import argparse
import csv
import json
import re
import shlex
import subprocess
import hashlib
from pathlib import Path

import address_map
import gen_tb_top
import gen_test_patterns as patterns
import packet_metrics


KV_TILES = (8, 9, 12, 13)
KV_CONFIG = "mesh_4x4_dual_edge_large"
KV_SHARD_BYTES = 512 * 1024
AXI_PAGE_BYTES = 4096
AXI_MAX_BURST_BEATS = 256


def review_traces(directory, config, result):
    """Add passive observations when present, keeping legacy summaries readable."""
    packet_path = directory / "perf.json.packets.csv"
    link_path = directory / "perf.json.links.csv"
    if not packet_path.exists() and not link_path.exists():
        return
    topology = gen_tb_top.load_topology(config)
    nodes, _, _ = gen_tb_top._nodes(topology)
    peripherals = gen_tb_top._peripherals(topology)
    endpoints = {i: (cid, port) for i, _, _, cid, port
                 in gen_tb_top._endpoints(nodes, peripherals)}
    perf = json.loads((directory / "perf.json").read_text())
    with packet_path.open() as stream:
        packets = packet_metrics.measure(csv.DictReader(stream), endpoints, perf["window"])
    packet_metrics.check_link_counts(packets, perf["noc"]["links"], nodes, peripherals)
    if packets["incomplete_packets"] or packets["missing_deliveries"]:
        raise ValueError("incomplete control packet observations")
    # Narrow control writes have two REQ flits. Restore read requests have one.
    control = [p for p in packets["deliveries"]
               if p["plane"] == "REQ" and p["flit_count"] == 2 and p["in_window"]]
    expected = (sum(p["delivered_payload_bytes"] for p in result["phases"].values()) // 8
                if "phases" in result else result["delivered_control_bytes"] // 8)
    if len(control) != expected:
        raise ValueError("control request packet coverage mismatch")
    if control:
        values = [p["latency_cycles"] for p in control]
        result["control_request_packet_latency"] = dict(samples=len(values),
            mean_cycles=sum(values)/len(values), max_cycles=max(values))
    if "phases" in result:
        windows = {name: (p["start_cycle"], p["end_cycle"])
                   for name, p in result["phases"].items()}
    else:
        windows = {}
        if result["control_samples"]:
            windows["control"] = (min(p["ready_cycle"] for p in result["control_samples"]),
                max(p["acknowledgement_cycle"] for p in result["control_samples"]))
        if result["kv_start"]:
            windows["kv"] = (min(result["kv_start"].values()), max(result["kv_done"].values()))
    with link_path.open() as stream:
        result["link_windows"] = packet_metrics.measure_link_windows(
            csv.DictReader(stream), perf["noc"]["links"], windows)
    result["packet_latency_by_plane"] = packets["summary"]
    result["collective_response_observations"] = len(packets["aggregation_observations"])
    result["analysis_sources_sha256"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (Path(__file__), Path(packet_metrics.__file__))}
    (directory / "packet_latencies.json").write_text(json.dumps(packets, indent=2) + "\n")


def generate_kv(directory, case):
    """Emit the approved same-tile control/KV comparison, without compute."""
    if case not in ("idle", "control", "offload", "restore", "control_offload", "control_restore"):
        raise ValueError("unknown KV comparison case")
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if (directory / "run.log").exists():
        raise ValueError("refusing to overwrite a KV measurement")
    topo = gen_tb_top.load_topology(KV_CONFIG)
    nodes, _, _ = gen_tb_top._nodes(topo)
    _, entries = address_map.pack_config(topo)
    config_bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "config"}
    config = {node: config_bases[cid] for node, _, _, cid in nodes}
    memories = [e for e in entries if e["space"] == "peripheral"]
    backing = memories[0]["base"]
    bus_bytes = patterns.axi_widths()["data"] // 8
    burst_bytes = min(AXI_PAGE_BYTES, AXI_MAX_BURST_BEATS * bus_bytes)
    if len(nodes) != 16 or len(memories) != 4 or backing % AXI_PAGE_BYTES:
        raise ValueError("KV mapping does not match approved topology")
    if bus_bytes & (bus_bytes - 1) or KV_SHARD_BYTES % burst_bytes:
        raise ValueError("KV shard cannot use aligned full-width bursts")
    has_control = case.startswith("control")
    offload, restore = "offload" in case, "restore" in case
    messages = []
    if has_control:
        for tile in KV_TILES:
            messages.extend([
                dict(source=0, target=tile, stage="command", address=config[tile], value=0x434D4401),
                dict(source=tile, target=0, stage="ack", address=config[0]+tile*8, value=0x41430000+tile),
            ])
    jobs = []
    for node in range(len(nodes) + len(memories)):
        folder = directory / f"node{node}"
        folder.mkdir(exist_ok=True)
        writes, reads, receives, init, sink = [], [], [], [], []
        outgoing = [m for m in messages if m["source"] == node]
        for m in outgoing:
            writes += patterns._ax_fields(0, m["address"], 0, 3, True)
            lane = m["address"] % bus_bytes
            writes.append(f'0x{m["value"] << (lane*8):x} 0x{255 << lane:x} 0')
        for m in messages:
            if m["target"] == node:
                bit = KV_TILES.index(m["source"]) if node == 0 else 0
                receives.append(f'{m["address"]:x} {m["value"]:x} {bit} {m["stage"]} {m["source"]}')
        if node in KV_TILES and (offload or restore):
            base = backing + KV_TILES.index(node) * KV_SHARD_BYTES
            for offset in range(0, KV_SHARD_BYTES, burst_bytes):
                addr = base + offset
                fields = patterns._ax_fields(0, addr, burst_bytes // bus_bytes - 1,
                                             bus_bytes.bit_length()-1, offload)
                jobs.append(dict(node=node, address=addr, bytes=burst_bytes))
                if restore:
                    reads += fields
                else:
                    writes += fields
                    for beat in range(addr, addr + burst_bytes, bus_bytes):
                        data = bytes(((a ^ (a >> 8) ^ (a >> 16) ^ 0xa5) & 255)
                                     for a in range(beat, beat + bus_bytes))
                        writes.append(f'0x{int.from_bytes(data, "little"):x} 0x{(1 << bus_bytes)-1:x} 0')
        if node == len(nodes):
            for rank in range(len(KV_TILES)):
                base = backing + rank * KV_SHARD_BYTES
                if restore:
                    init.append(f"0x{base:x} {KV_SHARD_BYTES}")
                if offload:
                    sink.append(f"{base:x} {KV_SHARD_BYTES}")
        for name, lines in (("write.txt", writes), ("read.txt", reads),
                            ("control_receives.txt", receives), ("memory_init.txt", init),
                            ("kv_sink.txt", sink), ("kv_schedule.txt", [str(len(outgoing))])):
            (folder / name).write_text("\n".join(lines) + ("\n" if lines else ""))
    plan = dict(case=case, config=KV_CONFIG, host=0, tiles=KV_TILES, memory_endpoint=16,
                shard_bytes=KV_SHARD_BYTES, bus_bytes=bus_bytes, burst_bytes=burst_bytes,
                messages=messages, jobs=jobs, read=restore, control=has_control,
                source_policy="one outstanding per direction, acknowledgement at next write opportunity")
    (directory / "control_plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    return plan


def review_kv(directory):
    directory = Path(directory)
    plan = json.loads((directory / "control_plan.json").read_text())
    log = (directory / "run.log").read_text()
    if re.search(r"%Error|Unexpected RData|Assertion failed|\$fatal", log) or "[kv_endpoint_done]" not in log:
        raise ValueError("KV simulation failed or is incomplete")
    def events(pattern):
        rows = re.findall(pattern, log)
        if len({row[0] for row in rows}) != len(rows):
            raise ValueError("duplicate endpoint event")
        return {int(node): int(cycle) for node, cycle in rows}
    start = events(r"\[kv_start\] node=(\d+) cycle=(\d+)")
    done = events(r"\[kv_done\] node=(\d+) cycle=(\d+)")
    ready = events(r"\[kv_control_ready\] node=(\d+) cycle=(\d+)")
    expected_nodes = set(plan["tiles"]) if plan["jobs"] else set()
    if start.keys() != expected_nodes or done.keys() != expected_nodes:
        raise ValueError("missing KV completion")
    endpoints = re.findall(r"\[kv_endpoint_done\] node=(\d+) writes=(\d+) reads=(\d+) sink_bytes=(\d+) cycle=(\d+)", log)
    if len(endpoints) != 20 or {int(r[0]) for r in endpoints} != set(range(20)):
        raise ValueError("incomplete endpoint retirement")
    for node, writes, reads, sink, _ in endpoints:
        n = int(node)
        controls = sum(m["source"] == n for m in plan["messages"])
        bulk = sum(j["node"] == n for j in plan["jobs"])
        if int(writes) != controls + (0 if plan["read"] else bulk) or int(reads) != (bulk if plan["read"] else 0):
            raise ValueError("endpoint transaction count mismatch")
        expected_sink = sum(j["bytes"] for j in plan["jobs"]) if n == 16 and not plan["read"] else 0
        if int(sink) != expected_sink:
            raise ValueError("KV sink byte count mismatch")
    deliveries = {}
    for stage, source, dest, cycle in re.findall(r"\[control_receive\] stage=(\w+) source=(\d+) destination=(\d+) cycle=(\d+)", log):
        key = stage, int(source), int(dest)
        if key in deliveries:
            raise ValueError("duplicate control delivery")
        deliveries[key] = int(cycle)
    if deliveries.keys() != {(m["stage"], m["source"], m["target"]) for m in plan["messages"]}:
        raise ValueError("incorrect control recipient coverage")
    if ready.keys() != ({0, *plan["tiles"]} if plan["control"] else set()):
        raise ValueError("incorrect control readiness coverage")
    issues = re.findall(r"\[kv_issue\] node=(\d+) flow=\s*(\w+) address=([0-9a-f]+) cycle=(\d+)", log)
    actual_jobs = [(int(n), int(a, 16)) for n, flow, a, _ in issues if flow == "kv"]
    if sorted(actual_jobs) != sorted((j["node"], j["address"]) for j in plan["jobs"]):
        raise ValueError("KV source transfer mismatch")
    control_rows = [(int(n), int(a, 16), int(c)) for n, flow, a, c in issues if flow == "control"]
    control_issues = {(n, a): c for n, a, c in control_rows}
    expected_controls = {(m["source"], m["address"]) for m in plan["messages"]}
    if len(control_rows) != len(expected_controls) or control_issues.keys() != expected_controls:
        raise ValueError("control source transfer mismatch")
    samples = []
    for tile in plan["tiles"] if plan["control"] else []:
        command = deliveries["command", 0, tile]
        ack = deliveries["ack", tile, 0]
        if not ready[0] <= command == ready[tile] <= ack:
            raise ValueError("control dependency or readiness violation")
        if plan["jobs"] and not start[tile] <= ready[0] <= ack <= done[tile]:
            raise ValueError("control exchange is not fully inside tile KV transfer")
        ack_message = next(m for m in plan["messages"] if m["source"] == tile)
        samples.append(dict(tile=tile, ready_cycle=ready[0], command_cycle=command,
                            acknowledgement_cycle=ack, latency_cycles=ack-ready[0],
                            acknowledgement_source_wait_cycles=control_issues[tile, ack_message["address"]]-command))
    result = dict(case=plan["case"], status="PASS", control_samples=samples,
                  delivered_control_bytes=8*len(plan["messages"]),
                  delivered_kv_bytes=sum(j["bytes"] for j in plan["jobs"]),
                  kv_start=start, kv_done=done)
    if start:
        result["kv_completion_cycles"] = max(done.values()) - min(start.values()) + 1
    if samples:
        result["control_completion_cycles"] = max(s["latency_cycles"] for s in samples) + 1
        result["control_mean_latency_cycles"] = sum(s["latency_cycles"] for s in samples)/len(samples)
    perf = json.loads((directory / "perf.json").read_text())
    result["counter_window"] = perf.get("window")
    result["networks"] = {}
    jobs, beats, controls = len(plan["jobs"]), result["delivered_kv_bytes"]//plan["bus_bytes"], len(plan["messages"])
    expected = dict(req=2*controls + (jobs if plan["read"] else 0),
                    rsp=controls + (0 if plan["read"] else jobs),
                    dat=beats + (0 if plan["read"] else jobs))
    for plane in ("req", "rsp", "dat"):
        links = perf["noc"]["links"]
        injected = sum(l["flit_count"] for l in links if
                       re.fullmatch(plane+r"_inject_\d+", l["name"]) or
                       re.fullmatch(plane+r"_node\d+\.y_to_node\d+\.router", l["name"]))
        hops = sum(l["flit_count"] for l in links if re.fullmatch(plane+r"_\d+to\d+", l["name"]))
        if injected != expected[plane]:
            raise ValueError(f"{plane} injection count {injected} != {expected[plane]}")
        result["networks"][plane] = dict(injected_flits=injected, network_flit_hops=hops)
    review_traces(directory, plan["config"], result)
    return result


def run_kv_batch(directory, binary, resume=False):
    """Two isolated KV runs establish one common offset, then three control runs."""
    directory, binary = Path(directory).resolve(), Path(binary).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    results = {}
    manifest_tool = Path(__file__).with_name("emit_result_manifest.py")
    offset = 0
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    for case in ("offload", "restore", "control", "control_offload", "control_restore"):
        if case == "control":
            offset = min(results[c]["kv_completion_cycles"] for c in ("offload", "restore")) // 2
        target = directory / case
        reuse = resume and (target / "run.log").exists()
        if not reuse:
            generate_kv(target, case)
        elif (target / "summary.json").exists():
            previous = json.loads((target / "summary.json").read_text())
            if previous["binary_sha256"] != binary_hash:
                raise ValueError("cannot resume with a different simulator binary")
        command = ["stdbuf", "-oL", str(binary), f"+stim_dir={target}", "+kv_overlap", f"+control_offset={offset}",
                   "+injection_mode=0", "+verilator+seed+1", "+timeout_cycles=2000000", "+packet_trace",
                   f"+sam_config=/workspace/sim/configs/{KV_CONFIG}.yml",
                   f"+perf_out={target / 'perf.json'}", f"+perf_scenario=kv_{case}"]
        if not reuse:
            subprocess.run(["python3", str(manifest_tool), "--repo-root", "/workspace",
                            "--write-source-patch", "--out", str(target / "source.patch")], check=True)
            print(f"Running {case}, control offset {offset}", flush=True)
            with (target / "run.log").open("w") as output:
                subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=600)
        result = review_kv(target)
        result.update(binary_sha256=binary_hash, control_offset=offset)
        (target / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        subprocess.run(["python3", str(manifest_tool), "--repo-root", "/workspace",
                        "--out", str(target / "manifest.json"), "--config", f"sim/configs/{KV_CONFIG}.yml",
                        "--simulator", "verilator", "--seed", "1", "--command", shlex.join(command),
                        "--source-patch", str(target / "source.patch")], check=True)
        results[case] = result
        print(json.dumps(result), flush=True)
    (directory / "summary.json").write_text(json.dumps(results, indent=2) + "\n")


def generate(directory, mode):
    topo = gen_tb_top.load_topology("mesh_4x4")
    nodes, _, _ = gen_tb_top._nodes(topo)
    _, entries = address_map.pack_config(topo)
    bases = {e["dst_id"]: e["base"] for e in entries if e["space"] == "config"}
    config = {node: bases[cid] for node, _, _, cid in nodes}
    if len(config) != 16:
        raise ValueError("approved control mapping requires 16 tiles")
    # Disjoint wildcard closures exclude the host without adding a self delivery.
    groups = ([[n] for n in range(1, 16)] if mode == "unicast" else
              [[1], [2, 3], list(range(4, 8)), list(range(8, 16))])
    messages = []
    for stage in ("command", "status", "release"):
        transfers = ([(n, [0]) for n in range(1, 16)] if stage == "status" else
                     [(0, group) for group in groups])
        for source, targets in transfers:
            offset = {"command": 0, "status": source * 8, "release": 128}[stage]
            value = {"command": 0x434D4401, "status": 0x53540000 + source,
                     "release": 0x52454C01}[stage]
            mask = patterns.collective_addr_mask(config, targets, targets[0])
            user = patterns._awuser_multicast(mask) if len(targets) > 1 else 0
            wait_mask = (0 if stage == "command" else
                         1 if stage == "status" else (1 << 15) - 1)
            messages.append(dict(source=source, targets=targets, stage=stage,
                                 offset=offset, value=value, user=user, wait_mask=wait_mask))
    directory = Path(directory)
    for node in config:
        path = directory / f"node{node}"
        path.mkdir(parents=True, exist_ok=True)
        writes, waits, receives = [], [], []
        for m in messages:
            if m["source"] == node:
                addr = config[m["targets"][0]] + m["offset"]
                lane = addr % (patterns.axi_widths()["data"] // 8)
                writes += patterns._ax_fields(0, addr, 0, 3, True, m["user"])
                writes += [f'0x{m["value"] << (8 * lane):x} 0x{255 << lane:x} 0']
                waits.append(f'{m["wait_mask"]:x} {m["stage"]}')
            if node in m["targets"]:
                bit = m["source"] - 1 if node == 0 else (0 if m["stage"] == "command" else 1)
                receives.append(f'{config[node] + m["offset"]:x} {m["value"]:x} {bit} {m["stage"]} {m["source"]}')
        for name, lines in (("write.txt", writes), ("read.txt", []),
                            ("control_waits.txt", waits), ("control_receives.txt", receives)):
            (path / name).write_text("\n".join(lines) + ("\n" if lines else ""))
    (directory / "control_plan.json").write_text(json.dumps(dict(
        mode=mode, message_bytes=8, host=0, messages=messages,
        scheduling="one outstanding write per source, status after local command delivery, release after all status deliveries",
        compute_delay_cycles=0), indent=2) + "\n")


def review(directory):
    directory = Path(directory)
    plan = json.loads((directory / "control_plan.json").read_text())
    log = (directory / "run.log").read_text()
    if re.search(r"%Error|Unexpected RData|Assertion failed|\$fatal", log):
        raise ValueError("simulation reported a failure")
    receives = {}
    for stage, source, destination, cycle in re.findall(
            r"\[control_receive\] stage=(\w+) source=(\d+) destination=(\d+) cycle=(\d+)", log):
        key = stage, int(source), int(destination)
        if key in receives:
            raise ValueError("duplicate delivery")
        receives[key] = int(cycle)
    expected = {(m["stage"], m["source"], target) for m in plan["messages"] for target in m["targets"]}
    if receives.keys() != expected:
        raise ValueError("missing or unexpected deliveries")
    ready = {}
    for stage, source, job, cycle in re.findall(
            r"\[control_ready\] stage=(\w+) source=(\d+) job=(\d+) cycle=(\d+)", log):
        key = int(source), int(job)
        if key in ready:
            raise ValueError("duplicate source job")
        ready[key] = stage, int(cycle)
    counts = {}
    for m in plan["messages"]:
        source = m["source"]
        job = counts.get(source, 0)
        counts[source] = job + 1
        stage, cycle = ready[source, job]
        if stage != m["stage"]:
            raise ValueError("source stage disagrees with plan")
        prerequisite = (receives["command", 0, source] if stage == "status" else
                        max(receives["status", n, 0] for n in range(1, 16)) if stage == "release" else 0)
        if cycle < prerequisite or any(receives[stage, source, n] < cycle for n in m["targets"]):
            raise ValueError("control dependency or timestamp violation")
    if len(ready) != len(plan["messages"]):
        raise ValueError("unexpected source jobs")
    done = re.findall(r"\[control_done\] node=(\d+) writes=(\d+) cycle=(\d+)", log)
    if len(done) != 16 or {int(n): int(w) for n, w, _ in done} != counts:
        raise ValueError("incomplete endpoint response retirement")
    result = dict(mode=plan["mode"], status="PASS", phases={})
    for stage in ("command", "status", "release"):
        start = (ready[0, 0][1] if stage == "command" else
                 min(receives["command", 0, n] for n in range(1, 16)) if stage == "status" else
                 max(receives["status", n, 0] for n in range(1, 16)))
        end = max(c for (s, _, _), c in receives.items() if s == stage)
        result["phases"][stage] = dict(start_cycle=start, end_cycle=end,
            transfer_completion_cycles=end-start+1, delivered_payload_bytes=15*8,
            average_delivery_bytes_per_cycle=15*8/(end-start+1))
    # Counters span the simulator window, including response drain and settle.
    # Do not divide them by a shorter endpoint-delivery phase.
    perf = json.loads((directory / "perf.json").read_text())
    result["counter_window"] = perf.get("window")
    result["networks"] = {}
    for plane in ("req", "rsp", "dat"):
        links = perf["noc"]["links"]
        injected = sum(l["flit_count"] for l in links if re.fullmatch(plane+r"_inject_\d+", l["name"]))
        hops = sum(l["flit_count"] for l in links if re.fullmatch(plane+r"_\d+to\d+", l["name"]))
        result["networks"][plane] = dict(injected_flits=injected, network_flit_hops=hops)
    if result["networks"]["dat"]["injected_flits"]:
        raise ValueError("narrow control unexpectedly used DAT")
    # NMU emits one AW flit followed by one W flit for each single-beat write.
    expected_request_flits = sum(1 + 1 for _ in plan["messages"])
    if result["networks"]["req"]["injected_flits"] != expected_request_flits:
        raise ValueError("narrow request injection count disagrees with message count")
    if result["networks"]["rsp"]["injected_flits"] != len(expected):
        raise ValueError("response injection count disagrees with recipient count")
    review_traces(directory, "mesh_4x4", result)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--mode", choices=("unicast", "multicast"))
    parser.add_argument("--kv-batch", action="store_true", help="run the approved five-case KV/control comparison")
    parser.add_argument("--resume", action="store_true", help="validate and reuse existing KV logs before continuing the batch")
    parser.add_argument("--binary", type=Path, help="run and review using an existing built simulator")
    parser.add_argument("--review-only", action="store_true", help="reprocess existing raw records without simulation")
    args = parser.parse_args()
    if args.kv_batch:
        if not args.binary or args.mode or args.review_only:
            parser.error("--kv-batch requires --binary and no --mode/--review-only")
        run_kv_batch(args.directory, args.binary, args.resume)
        raise SystemExit(0)
    if not args.mode:
        parser.error("--mode is required for the original control comparison")
    if args.review_only and not args.binary:
        parser.error("--review-only requires the original --binary for provenance")
    if not args.review_only and (args.directory / "run.log").exists():
        raise ValueError("refusing to overwrite existing measurement inputs")
    if not args.review_only:
        generate(args.directory, args.mode)
    if args.binary:
        directory = args.directory.resolve()
        if not args.review_only and (directory / "run.log").exists():
            raise ValueError("refusing to overwrite an existing simulation log")
        command = [str(args.binary), f"+stim_dir={directory}", "+control_sequence",
                   "+injection_mode=0", "+verilator+seed+1", "+timeout_cycles=100000", "+packet_trace",
                   "+sam_config=/workspace/sim/configs/mesh_4x4.yml",
                   f"+perf_out={directory / 'perf.json'}", f"+perf_scenario=control_{args.mode}"]
        manifest_tool = Path(__file__).with_name("emit_result_manifest.py")
        if not args.review_only:
            subprocess.run(["python3", str(manifest_tool), "--repo-root", "/workspace",
                            "--write-source-patch", "--out", str(directory / "source.patch")], check=True)
            with (directory / "run.log").open("w") as output:
                subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=120)
        result = review(directory)
        (directory / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        subprocess.run(["python3", str(manifest_tool), "--repo-root", "/workspace",
                        "--out", str(directory / "manifest.json"), "--config", "sim/configs/mesh_4x4.yml",
                        "--simulator", "verilator", "--seed", "1", "--command", shlex.join(command),
                        "--source-patch", str(directory / "source.patch")], check=True)
        print(json.dumps(result, indent=2))
