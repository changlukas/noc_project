"""Pair passive NMU AXI observations without changing the simulated interface."""

import argparse
from collections import Counter, defaultdict, deque
import csv
import json
import re
from pathlib import Path

import yaml

from address_map import members, pack_config, router_array
from gen_tb_top import ROOT, num_vc


def packet_layout():
    text = (ROOT / "specgen/generated/cpp/ni_flit_constants.h").read_text()
    header_width = int(re.search(r"constexpr int\s+HEADER_WIDTH\s*=\s*(\d+)", text)[1])
    fields = {}
    for namespace, offset in (("header", 0), ("payload::aw", header_width),
                               ("payload::ar", header_width)):
        block = text.split("namespace " + namespace + " {")[1].split("}")[0]
        numbers = {name: int(value) for name, value in
                   re.findall(r"constexpr int\s+(\w+)\s*=\s*(\d+)", block)}
        for name in numbers:
            if name.endswith("_LSB"):
                field = name.removesuffix("_LSB")
                fields[field] = (offset + numbers[name], numbers[field + "_WIDTH"])
    channels = {int(value): name for name, value in
                re.findall(r"constexpr int AXI_CH_(\w+)\s*=\s*(\d+)", text)}
    return fields, channels


def attach_admission(transactions, packet_path):
    """Match requests at NMU AXI ingress to their injected AW/AR headers.

    The start is first request VALID, not descriptor readiness. Responses and
    write-data flits are not request-admission samples. Repeated requests with
    the same ID and shape retain source order.
    """
    fields, channels = packet_layout()
    pending = defaultdict(deque)
    for txn in transactions:
        if txn["direction"] not in ("AW", "AR") or txn["accepted_cycle"] is None:
            raise ValueError("admission requires accepted AW/AR requests")
        key = tuple(txn[k] for k in ("node", "direction", "axi_class", "id", "address", "beats", "size"))
        pending[key].append(txn)
    ended = False
    with packet_path.open() as stream:
        for row in csv.DictReader(stream):
            if ended:
                raise ValueError("packet event after END")
            if row["event"] == "END":
                ended = True
            if row["event"] != "IN":
                continue
            word = int(row["flit"], 16)
            def field(name):
                shift, width = fields[name]
                return (word >> shift) & ((1 << width) - 1)
            channel = channels[field("AXI_CH")]
            if channel not in ("DataAw", "DataAr", "NarrowAw", "NarrowAr"):
                continue
            direction = channel[-2:].upper()
            key = (int(row["node"]), direction, channel[:-2], field(direction + "ID"),
                   field(direction + "ADDR"), field(direction + "LEN") + 1, field(direction + "SIZE"))
            if not pending[key]:
                raise ValueError("injected request has no matching AXI transaction")
            txn = pending[key].popleft()
            cycle = int(row["cycle"])
            if not txn["first_valid_cycle"] <= txn["accepted_cycle"] <= cycle:
                raise ValueError("request injection precedes AXI acceptance")
            txn["header_injection_cycle"] = cycle
            txn["request_plane"] = row["plane"]
            txn["admission_latency_cycles"] = cycle - txn["first_valid_cycle"]
    if not ended or any(pending.values()):
        raise ValueError("missing request injection or packet trace END")


def require_rows(rows, fields, expected):
    identities = {tuple(row[field] for field in fields) for row in rows}
    if identities != expected or len(rows) != len(expected):
        raise ValueError(f"missing, extra or duplicate counter rows: {fields}")


def read_transactions(path: Path, window=None, stall_cycles=None) -> list[dict]:
    transactions = []
    pending = {"AW": None, "AR": None}
    reads, writes = defaultdict(deque), defaultdict(deque)
    aw_order, w_bursts = deque(), deque()
    w_beats = 0
    ended = False
    previous_cycle = -1

    def pair_w():
        while aw_order and w_bursts:
            txn = aw_order.popleft()
            cycle, beats = w_bursts.popleft()
            if beats != txn["beats"]:
                raise ValueError(f"{path}: W burst length mismatch")
            txn["wlast_cycle"] = cycle

    with path.open() as stream:
        for row in csv.DictReader(stream):
            event = row["event"]
            cycle = int(row["cycle"])
            if ended:
                raise ValueError(f"{path}: events after END")
            if event == "RESET":
                for txn in transactions:
                    if txn["status"] == "incomplete":
                        txn["status"] = "reset"
                pending = {"AW": None, "AR": None}
                reads.clear()
                writes.clear()
                aw_order.clear()
                w_bursts.clear()
                w_beats = 0
                previous_cycle = -1
                continue
            if cycle < previous_cycle:
                raise ValueError(f"{path}: cycle regressed without reset")
            previous_cycle = cycle
            if event == "END":
                ended = True
                continue
            ready = int(row["ready"])
            selected = bool(int(row["in_window"]))
            if window is not None and selected != (window["start_cyc"] <= cycle < window["end_cyc"]):
                raise ValueError(f"{path}: AXI event outside declared perf window")
            if stall_cycles is not None and selected and not ready:
                stall_cycles[event] += 1
            ident = int(row["id"])
            if event in pending:
                shape = (ident, int(row["address"], 16), int(row["len"]) + 1,
                         int(row["size"]), int(row.get("user", "0"), 16))
                txn = pending[event]
                if txn is None:
                    txn = dict(sequence=len(transactions), direction=event,
                               id=ident, address=shape[1], beats=shape[2], size=shape[3],
                               user=shape[4],
                               first_valid_cycle=cycle, in_window=selected,
                               status="incomplete", accepted_cycle=None,
                               first_response_valid_cycle=None,
                               first_response_cycle=None, completion_cycle=None,
                               wlast_cycle=None, response_beats=0)
                    transactions.append(txn)
                    pending[event] = txn
                elif shape != (txn["id"], txn["address"], txn["beats"], txn["size"], txn["user"]):
                    raise ValueError(f"{path}: address changed while stalled")
                if ready:
                    txn["accepted_cycle"] = cycle
                    pending[event] = None
                    if event == "AR":
                        reads[ident].append(txn)
                    else:
                        writes[ident].append(txn)
                        aw_order.append(txn)
                        pair_w()
            elif event == "W":
                if ready:
                    w_beats += 1
                    if int(row["last"]):
                        w_bursts.append((cycle, w_beats))
                        w_beats = 0
                        pair_w()
            elif event in ("R", "B"):
                queue = (reads if event == "R" else writes)[ident]
                if not queue:
                    raise ValueError(f"{path}: {event} without accepted address, ID {ident}")
                txn = queue[0]
                if txn["first_response_valid_cycle"] is None:
                    txn["first_response_valid_cycle"] = cycle
                if ready:
                    if txn["first_response_cycle"] is None:
                        txn["first_response_cycle"] = cycle
                    txn["response_beats"] += 1
                    if event == "R" and bool(int(row["last"])) != (txn["response_beats"] == txn["beats"]):
                        raise ValueError(f"{path}: RLAST/beat count mismatch")
                    if event == "B" or int(row["last"]):
                        if event == "B" and txn["wlast_cycle"] is None:
                            raise ValueError(f"{path}: B before WLAST")
                        queue.popleft()
                        txn["completion_cycle"] = cycle
                        txn["status"] = "complete" if selected else "completed_outside_window"
            else:
                raise ValueError(f"{path}: unknown event {event}")
    if not ended:
        raise ValueError(f"{path}: missing END marker")
    # W has no ID. An unmatched W burst cannot be attributed to an AXI address.
    if w_bursts or w_beats:
        transactions.append(dict(sequence=len(transactions), direction="W",
                                 status="unmatched_write_data", in_window=None,
                                 completed_bursts=len(w_bursts), partial_beats=w_beats))
    return transactions


def summarize(transactions: list[dict]) -> dict:
    values = defaultdict(list)
    statuses = Counter()
    for txn in transactions:
        statuses[txn["status"]] += 1
        if not txn["in_window"] or txn["status"] != "complete":
            continue
        direction = "read" if txn["direction"] == "AR" else "write"
        accepted = txn["accepted_cycle"]
        values[f"{direction}_source_wait"].append(accepted - txn["first_valid_cycle"])
        if "admission_latency_cycles" in txn:
            values[f"{direction}_admission"].append(txn["admission_latency_cycles"])
        values[f"{direction}_completion"].append(txn["completion_cycle"] - accepted)
        values[f"{direction}_first_response_wait"].append(
            txn["first_response_cycle"] - txn["first_response_valid_cycle"])
        if direction == "read":
            values["read_first_response"].append(txn["first_response_cycle"] - accepted)
        else:
            values["write_wlast_to_b"].append(txn["completion_cycle"] - txn["wlast_cycle"])
    return dict(status_counts=dict(statuses), outside_window_requests=sum(
        txn["in_window"] is False for txn in transactions),
        metrics={name: dict(samples=len(data), sum_cycles=sum(data),
                            mean_cycles=sum(data) / len(data), max_cycles=max(data))
                 for name, data in values.items()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    config = yaml.safe_load(args.config.read_text())
    perf = json.loads((args.run_dir / "perf.json").read_text())
    window = perf["window"]
    duration = window["end_cyc"] - window["start_cyc"]
    endpoint_count = sum(members(ep) for ep in config["endpoints"])
    x_dim, y_dim = router_array(config)
    outputs = {(f"router_{node}", port) for node in range(x_dim * y_dim)
               for port in ("NORTH", "EAST", "SOUTH", "WEST", "LOCAL")}
    require_rows(perf["noc"]["nmu_requests"], ("node", "channel"),
                 {(f"nmu_{node}", channel) for node in range(endpoint_count)
                  for channel in ("AW", "AR")})
    require_rows(perf["noc"]["router_dat_switches"], ("router", "port"), outputs)
    require_rows(perf["noc"]["router_dat_input_vcs"], ("router", "port", "vc"),
                 {(node, port, vc) for node, port in outputs for vc in range(num_vc())})
    for request in perf["noc"]["nmu_requests"]:
        counts = [request[key] for key in ("ordering_wait_cycles", "order_list_full_cycles",
                                          "reorder_storage_full_cycles", "downstream_wait_cycles")]
        if request["samples"] != duration or min(counts) < 0 or sum(counts) > duration:
            parser.error("invalid NMU request wait counts or window")
    for switch in perf["noc"]["router_dat_switches"]:
        grants = switch["grant_cycles"]
        waiting = switch["arbitration_wait_vc_cycles"]
        if (switch["samples"] != duration or not 0 <= grants <= duration
                or waiting < 0 or switch["eligible_vc_cycles"] != grants + waiting):
            parser.error("invalid Router arbitration counts or window")
    for queue in perf["noc"]["router_dat_input_vcs"]:
        samples, capacity = queue["samples"], queue["capacity_flits"]
        total, full = queue["occupancy_sum_flits"], queue["full_cycles"]
        if (samples != duration or capacity <= 0 or not 0 <= full <= samples
                or not full * capacity <= total <= samples * capacity
                or not 0 <= queue["hwm_flits"] <= capacity):
            parser.error("invalid Router occupancy samples or window")
    for wait in perf["noc"].get("router_dat_allocations", []):
        total, occupied, full, both = (wait[k] for k in
            ("waiting_cycles", "occupied_cycles", "input_full_cycles", "occupied_input_full_cycles"))
        if not (0 <= both <= min(occupied, full) <= max(occupied, full) <= total <= duration
                and occupied + full - both <= total):
            parser.error("invalid VC allocation wait subsets")
    result = read_run(args.run_dir, config, window)
    (args.run_dir / "axi_transactions.json").write_text(json.dumps(result, indent=2) + "\n")


def read_run(directory, config, window, measured_cycles=None):
    _, sam = pack_config(config)
    endpoint_count = sum(members(ep) for ep in config["endpoints"])
    duration = window["end_cyc"] - window["start_cyc"] if measured_cycles is None else measured_cycles
    paths = sorted(directory.glob("perf.json.axi*.csv"))
    expected = {f"perf.json.axi{i}.csv" for i in range(endpoint_count)}
    if {path.name for path in paths} != expected:
        raise ValueError("missing or extra passive AXI endpoint traces")
    transactions = []
    endpoint_stalls = []
    for path in paths:
        node = int(path.name.removeprefix("perf.json.axi").removesuffix(".csv"))
        stalls = Counter({channel: 0 for channel in ("AW", "AR", "W", "R", "B")})
        for txn in read_transactions(path, window, stalls):
            if "address" in txn:
                match = next((entry for entry in sam
                              if entry["base"] <= txn["address"] < entry["base"] + entry["size"]), None)
                if match is None:
                    raise ValueError(f"node {node}: AXI address misses SAM")
                txn["sam_destination"] = {key: match[key] for key in ("x", "y", "port", "space")}
                txn["axi_class"] = "Narrow" if match["space"] == "config" else "Data"
            transactions.append(dict(node=node, **txn))
        endpoint_stalls.append(dict(node=node, samples=duration, valid_not_ready_cycles=dict(stalls)))
    attach_admission(transactions, directory / "perf.json.packets.csv")
    result = dict(schema_version=1, observation_point="NMU AXI ingress after ID remap",
                  time_unit="AXI clock cycles", window=window, summary=summarize(transactions),
                  transactions=transactions, endpoint_stalls=endpoint_stalls)
    return result


if __name__ == "__main__":
    main()
