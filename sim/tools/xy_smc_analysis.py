"""Original SMC reference for direct XY, using accepted NI-to-delivery events.

Read-only reconstruction from accepted simulation traces. Never reinterpret old
DMA durations or fitted model estimates as NI/SMC measurements.
"""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re

MODEL = {"model_id": "paper_smc_direct_flit_v1",
         "equation": "max(C, E/N + L) + (2*T_R + 1)*D",
         "T_R": 2, "D": 1, "unit": "one DAT flit per ideal wavelet, headers included",
         "assumptions": ["one ideal flit per directional link per cycle", "one cycle per fabric hop",
                         "independent simultaneous endpoint send/receive", "original inputs ready; direct delivery"],
         "start_event": "first_source_NI_AW_handshake",
         "end_event": "last_destination_NI_DAT_tail_received",
         "cycle_convention": "end - start + 1 (inclusive sampled cycles)",
         "limitations": ["ideal reference, not a fitted hardware estimate",
                         "post-acceptance W supply gaps and issuance of later requests remain inside the window",
                         "response return is excluded from the end event; response-driven scheduling may still affect later sends"]}


def analyze(contract, transactions, packets, links):
    if contract.get("variant") != "direct_xy" or contract.get("routing") != "XY":
        raise ValueError("SMC mapping only approved for direct XY")
    transfers = contract["costs"]["transfers"]
    if any(t["prerequisites"] for t in transfers):
        raise ValueError("D=1 requires independent original inputs")
    if packets["incomplete_packets"] or packets["missing_deliveries"]:
        raise ValueError("incomplete packet evidence")
    txns = list(transactions)
    if not txns or any(t["direction"] != "AW" or t["status"] != "complete" or
                       not t["in_window"] or t.get("request_plane") != "DAT" for t in txns):
        raise ValueError("expected complete in-window DAT write requests")
    deliveries = [d for d in packets["deliveries"] if d["plane"] == "DAT"]
    lookup = {}
    for d in deliveries:
        key = (d["source"], d["injection_head"])
        if key in lookup or not d["in_window"]:
            raise ValueError("duplicate or out-of-window DAT request")
        lookup[key] = d
    sends, receives, edges, service = Counter(), Counter(), Counter(), Counter()
    start, end = None, None
    maximum_hops = 0
    for t in txns:
        key = (t["node"], t["header_injection_cycle"])
        if key not in lookup:
            raise ValueError("AW has no matching DAT delivery")
        d = lookup.pop(key)
        dest = t["sam_destination"]
        target = dest["x"] + 4*dest["y"]
        if dest["port"] != 0 or target != d["destination"]:
            raise ValueError("delivery has wrong final owner")
        if not (t["accepted_cycle"] <= d["injection_head"] <= d["ejection_tail"] <= t["completion_cycle"]):
            raise ValueError("invalid NI acceptance/delivery/response ordering")
        if d["flit_count"] != t["beats"] + 1:
            raise ValueError("DAT write must contain AW header and all W flits")
        if t["size"] != 6:
            raise ValueError("approved mapping requires 64-byte AXI beats")
        start = t["accepted_cycle"] if start is None else min(start, t["accepted_cycle"])
        end = d["ejection_tail"] if end is None else max(end, d["ejection_tail"])
        source, flits = t["node"], d["flit_count"]
        service[source, target] += t["beats"] * 64
        sends[source] += flits
        receives[target] += flits
        node, hops = source, 0
        while node != target:
            nxt = node + (1 if node%4 < target%4 else -1) if node%4 != target%4 else node + (4 if node < target else -4)
            edges[f"dat_{node}to{nxt}"] += flits
            node, hops = nxt, hops+1
        maximum_hops = max(maximum_hops, hops)
    if lookup:
        raise ValueError("extra DAT delivery")
    expected_service = Counter()
    for t in transfers:
        expected_service[t["path"][0], t["path"][-1]] += t["service_bytes"]
    if service != expected_service:
        raise ValueError("AXI service coverage differs from transfer contract")
    measured_edges = Counter({x["name"]:x["flit_count"] for x in links
                              if re.fullmatch(r"dat_\d+to\d+",x["name"]) and x["flit_count"]})
    if edges != measured_edges:
        raise ValueError("DAT directed-link flit counts disagree")
    C, E, N, L = max(max(sends.values()), max(receives.values())), sum(edges.values()), len(edges), maximum_hops
    if not N:
        raise ValueError("empty fabric workload")
    estimated = max(C, E/N + L) + (2*MODEL["T_R"] + 1)*MODEL["D"]
    duration = end - start + 1
    if duration <= 0:
        raise ValueError("invalid delivery window")
    return dict(start_cycle=start, end_cycle=end, execution_cycles=duration,
                estimated_cycles=estimated, slowdown=duration/estimated,
                C_flits=C, E_flit_hops=E, N_directed_links=N, L_hops=L, D=MODEL["D"], T_R=MODEL["T_R"],
                matched_transactions=len(txns), maximum_send_flits=max(sends.values()),
                maximum_receive_flits=max(receives.values()), maximum_link_flits=max(edges.values()),
                receive_utilization_percent=100*max(receives.values())/duration,
                fabric_utilization_percent=100*max(edges.values())/duration)


def main():
    import yaml
    import perf_report
    import axi_transaction_metrics as axi
    import packet_metrics as packet
    from gen_tb_top import _nodes, _peripherals, _endpoints
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-primary", type=int, default=30,
                        help="Expected measurement count; use 3 for a targeted supplementary batch")
    args = parser.parse_args()
    config_path = Path(__file__).resolve().parents[1]/"configs/mesh_4x4_dual_edge_large.yml"
    config = yaml.safe_load(config_path.read_text())
    nodes, _, _ = _nodes(config)
    peripherals = _peripherals(config)
    endpoints = {idx:(cid,port) for idx,_,_,cid,port in _endpoints(nodes,peripherals)}
    primary, accepted = [], []
    for case in json.loads((args.batch/"batch_status.json").read_text()):
        if case["status"] != "passed":
            raise ValueError("unaccepted source case")
        if not case["operation"].startswith("xy_direct_"):
            continue
        directory = args.batch/case["tag"]
        row = perf_report.collect_operations([directory])[0]
        perf = json.loads((directory/"perf.json").read_text())
        print("ANALYZE", case["tag"], flush=True)
        transactions = axi.read_run(directory,config,perf["window"])["transactions"]
        with (directory/"perf.json.packets.csv").open() as stream:
            packets = packet.measure(csv.DictReader(stream),endpoints,perf["window"])
        packet.check_link_counts(packets,perf["noc"]["links"],nodes,peripherals)
        result = analyze(row["contract"],transactions,packets,perf["noc"]["links"])
        contract = row["contract"]
        result.update(operation=contract["kind"],participants=len(contract["participants"]),
                      elements_per_pe=contract["elements_per_pe"],bytes_per_pe=contract["per_pe_bytes"],
                      root=contract["root"],source=str(directory.resolve()),case=case["tag"],category=case["category"])
        # Hash original event evidence so the reconstruction is independently traceable.
        result["source_sha256"] = {}
        for path in sorted(directory.glob("perf.json*.csv")):
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda:stream.read(1024*1024),b""): digest.update(block)
            result["source_sha256"][path.name] = digest.hexdigest()
        accepted.append(result)
        if case["category"] == "measurement": primary.append(result)
        print("PASS",result["execution_cycles"],result["estimated_cycles"],flush=True)
    if len(primary) != args.expected_primary:
        raise ValueError(f"expected {args.expected_primary} primary direct cases")
    args.output.mkdir(parents=True,exist_ok=True)
    (args.output/"measurements.json").write_text(json.dumps(dict(model=MODEL,records=primary,accepted=accepted),indent=2)+"\n")
    fields = [k for k in primary[0] if k != "source_sha256"]
    with (args.output/"measurements.csv").open("w") as stream:
        writer=csv.DictWriter(stream,fieldnames=fields,extrasaction="ignore")
        writer.writeheader();writer.writerows(primary)
    print("COMPLETE",len(accepted),"accepted",len(primary),"primary",flush=True)

if __name__ == "__main__":
    main()
