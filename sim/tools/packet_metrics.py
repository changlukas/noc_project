"""Pair accepted NI/Router link packets, retaining one result per destination."""

import argparse
from collections import Counter, defaultdict, deque
import csv
import hashlib
import json
import re
from pathlib import Path

import yaml

from gen_tb_top import _endpoints, _nodes, _peripherals


def measure(rows, endpoints, window):
    active, packets = {}, []
    flits = Counter()
    epoch, previous_cycle = 0, -1
    ended = False
    for row in rows:
        event, cycle = row["event"], int(row["cycle"])
        if ended:
            raise ValueError("packet events after END")
        if event == "RESET":
            for packet in active.values():
                packet["status"] = "reset"
            active.clear()
            epoch += 1
            previous_cycle = -1
            continue
        if cycle < previous_cycle:
            raise ValueError("packet clock regressed without reset")
        previous_cycle = cycle
        if event == "END":
            ended = True
            continue
        if event not in ("IN", "OUT") or row["plane"] not in ("REQ", "RSP", "DAT"):
            raise ValueError("invalid packet event/plane")
        selected = bool(int(row["in_window"]))
        if selected != (window["start_cyc"] <= cycle < window["end_cyc"]):
            raise ValueError("packet event/window mismatch")
        node = int(row["node"])
        if node not in endpoints:
            raise ValueError("unknown packet endpoint")
        if selected:
            flits[node, row["plane"], event] += 1
        vc = int(row["vc"]) if row["plane"] == "DAT" else 0
        stream = node, row["plane"], event, vc
        word = format(int(row["flit"], 16), "x")
        packet = active.get(stream)
        if packet is None:
            packet = dict(epoch=epoch, node=node, plane=row["plane"], event=event,
                          vc=vc, start_cycle=cycle, end_cycle=None, in_window=selected,
                          head=word, src=int(row["src"]), dst=int(row["dst"]),
                          dst_port=int(row["dst_port"]), collective=int(row["collective"]),
                          mask=int(row["mask"]), flit_count=0, status="incomplete",
                          digest=hashlib.sha256())
            active[stream] = packet
            packets.append(packet)
        packet["digest"].update((word + "\n").encode())
        packet["flit_count"] += 1
        if int(row["tail"]):
            packet["end_cycle"] = cycle
            packet["end_in_window"] = selected
            packet["status"] = "complete"
            del active[stream]
    if not ended:
        raise ValueError("packet trace missing END")
    for packet in packets:
        packet["digest"] = packet["digest"].hexdigest()

    def identity(packet):
        return (packet["epoch"], packet["plane"], packet["head"],
                packet["digest"], packet["flit_count"])

    injected = defaultdict(deque)
    excluded, incomplete = [], []
    for packet in packets:
        if packet["status"] != "complete":
            incomplete.append(packet)
            continue
        # A collective B response merges several source packets into one.
        # Preserve its observations separately from one-to-one transport timing.
        if packet["collective"] and packet["plane"] == "RSP":
            excluded.append(packet)
            continue
        if packet["event"] != "IN":
            continue
        if packet["collective"] not in (0, 1):
            raise ValueError("unsupported collective encoding")
        targets = [node for node, (cid, port) in endpoints.items()
                   if port == packet["dst_port"] and
                   ((cid ^ packet["dst"]) & ~(packet["mask"] if packet["collective"] else 0)) == 0]
        if not targets:
            raise ValueError("packet destination has no endpoint")
        for target in targets:
            injected[identity(packet), target].append(packet)

    deliveries = []
    for packet in sorted(packets, key=lambda p: (p["epoch"], p["start_cycle"])):
        if (packet["event"] != "OUT" or packet["status"] != "complete"
                or packet["collective"] and packet["plane"] == "RSP"):
            continue
        queue = injected[identity(packet), packet["node"]]
        if not queue:
            raise ValueError("ejected packet has no matching injection or payload differs")
        available = [p for p in queue if p["start_cycle"] <= packet["start_cycle"]]
        if len({p["vc"] for p in available}) > 1:
            raise ValueError("identical overlapping packets on different source VCs are ambiguous")
        source = queue.popleft()
        if source["start_cycle"] > packet["start_cycle"] or source["end_cycle"] > packet["end_cycle"]:
            raise ValueError("packet ejection precedes injection")
        deliveries.append(dict(source=source["node"], destination=packet["node"],
                               plane=packet["plane"], flit_count=packet["flit_count"],
                               injection_head=source["start_cycle"],
                               injection_tail=source["end_cycle"],
                               ejection_head=packet["start_cycle"], ejection_tail=packet["end_cycle"],
                               latency_cycles=packet["end_cycle"] - source["start_cycle"],
                               in_window=source["in_window"] and packet["end_in_window"]))
    missing = [dict(packet, destination=target)
               for (_, target), queue in injected.items() for packet in queue]
    values = defaultdict(list)
    for delivery in deliveries:
        if delivery["in_window"]:
            values[delivery["plane"]].append(delivery["latency_cycles"])
    return dict(schema_version=1, time_unit="NoC clock cycles", window=window,
                summary={plane: dict(samples=len(data), mean_cycles=sum(data) / len(data),
                                     max_cycles=max(data), sum_cycles=sum(data))
                         for plane, data in values.items()},
                deliveries=deliveries, incomplete_packets=incomplete,
                missing_deliveries=missing, aggregation_observations=excluded,
                accepted_flits=[dict(node=node, plane=plane, event=event, count=count)
                                for (node, plane, event), count in sorted(flits.items())])


def check_link_counts(result, links, nodes, peripherals):
    observed = {(row["node"], row["plane"], row["event"]): row["count"]
                for row in result["accepted_flits"]}
    counters = {row["name"]: row["flit_count"] for row in links}
    for node in range(len(nodes) + len(peripherals)):
        for plane in ("req", "rsp", "dat"):
            for event, direction in (("IN", "inject"), ("OUT", "eject")):
                if node < len(nodes):
                    name = f"{plane}_{direction}_{node}"
                else:
                    peripheral = peripherals[node - len(nodes)]
                    host = peripheral["router_idx"]
                    face = "x" if peripheral["port"] == 1 else "y"
                    source, target = f"node{host}.{face}", f"node{host}.router"
                    if event == "OUT":
                        source, target = target, source
                    name = f"{plane}_{source}_to_{target}"
                if name not in counters or counters[name] != observed.get((node, plane.upper(), event), 0):
                    raise ValueError(f"packet trace/link counter mismatch: {name}")


def measure_link_windows(rows, links, windows):
    """Count accepted directed-link flits and clock slots within each window.

    Windows include both endpoint timestamps. Counts include all flows present
    during that interval, not only the flow that defines the interval.
    """
    expected = {l["name"]: l["flit_count"] for l in links
                if re.fullmatch(r"(?:req|rsp|dat)_\d+to\d+", l["name"])}
    totals = Counter()
    counts = {name: Counter() for name in windows}
    slots = Counter()
    last_tick, ended, selected = -1, False, False
    seen = set()
    for row in rows:
        cycle, event = int(row["cycle"]), row["event"]
        if ended:
            raise ValueError("link events after END")
        if event == "END":
            ended = True
            continue
        if event == "TICK":
            if last_tick >= 0 and cycle != last_tick + 1:
                raise ValueError("missing, duplicate or regressing link clock")
            last_tick, selected = cycle, bool(int(row["in_window"]))
            seen.clear()
            if selected:
                for name, (start, end) in windows.items():
                    slots[name] += start <= cycle <= end
        elif event == "LINK":
            link = row["link"]
            if (cycle != last_tick or link not in expected or link in seen
                    or bool(int(row["in_window"])) != selected):
                raise ValueError("invalid or duplicate link transfer")
            seen.add(link)
            if selected:
                totals[link] += 1
                for name, (start, end) in windows.items():
                    if start <= cycle <= end:
                        counts[name][link] += 1
        else:
            raise ValueError("invalid link event")
    if not ended or any(totals[name] != value for name, value in expected.items()):
        raise ValueError("incomplete link trace or counter mismatch")
    result = {}
    for name, values in counts.items():
        if not slots[name]:
            raise ValueError("window has no measured link slots")
        result[name] = dict(start_cycle=windows[name][0], end_cycle=windows[name][1],
            measured_cycles=slots[name], planes={})
        for plane in ("req", "rsp", "dat"):
            names = [link for link in expected if link.startswith(plane + "_")]
            peak = max((values[link] for link in names), default=0)
            result[name]["planes"][plane] = dict(maximum_flits=peak,
                maximum_link_utilization_percent=100*peak/slots[name],
                busiest_links=[link for link in names if peak and values[link] == peak])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    config = yaml.safe_load(args.config.read_text())
    nodes, _, _ = _nodes(config)
    peripherals = _peripherals(config)
    endpoints = {idx: (cid, port) for idx, _, _, cid, port
                 in _endpoints(nodes, peripherals)}
    perf = json.loads((args.run_dir / "perf.json").read_text())
    window = perf["window"]
    with (args.run_dir / "perf.json.packets.csv").open() as stream:
        result = measure(csv.DictReader(stream), endpoints, window)
    check_link_counts(result, perf["noc"]["links"], nodes, peripherals)
    (args.run_dir / "packet_latencies.json").write_text(json.dumps(result, indent=2) + "\n")
    if result["incomplete_packets"] or result["missing_deliveries"]:
        parser.error("completed DMA operation left incomplete packet observations")


if __name__ == "__main__":
    main()
