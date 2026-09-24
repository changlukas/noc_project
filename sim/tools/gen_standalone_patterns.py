#!/usr/bin/env python3
"""Shared directed/constrained-random AXI files and standalone checker schedule."""
import argparse
import json
import random
from pathlib import Path
from axi_file_format import _ax_fields, encode_write_beats

REPO = Path(__file__).resolve().parents[2]
CATALOG = REPO / "sim/test_patterns/standalone/cases.json"
SCHEDULE = ("response_order", "response_delay", "startup_delay",
            "stall_enable", "reset_warmup", "min_outstanding", "min_unique",
            "require_ooo", "require_buffered", "require_capacity", "require_stall")


def generate(out, topology, id_width=8, catalog=CATALOG, mode="auto", seed=1, case_name=None, profile="standalone"):
    out = Path(out)
    topology = Path(topology)
    if topology.suffix == ".json":
        entries = json.loads(topology.read_text())
    else:
        import yaml
        from address_map import pack_config
        _, entries = pack_config(yaml.safe_load(topology.read_text()))
    routes = {m: [e for e in entries if e["space"] == space]
              for m, space in (("control", "config"), ("data", "memory"))}
    if profile not in ("standalone", "cosim"):
        raise ValueError("unknown verification profile")
    required_destinations = 2 if profile == "standalone" else 1
    if any(len(v) < required_destinations for v in routes.values()):
        raise ValueError("standalone suite needs two control/data destinations")
    if not 1 <= id_width <= 8 or mode not in ("auto", "control", "data", "rand"):
        raise ValueError("invalid ID width or MODE")
    if not 0 <= seed <= 0xffffffff:
        raise ValueError("SEED must be an unsigned 32-bit integer")
    names = []
    cases = json.loads(Path(catalog).read_text())["cases"]
    if case_name is not None:
        cases = [c for c in cases if c["name"] == case_name]
        if not cases:
            raise ValueError("unknown CASE: " + case_name)
    for case in cases:
        name = case["name"]
        if profile == "cosim" and (case.get("reset_warmup") or case.get("legacy_mixed") or
                                    case.get("require_stall")):
            if case_name is not None:
                raise ValueError(name + " requires focused standalone response scheduling")
            continue
        if name in names or not name.replace("_", "").isalnum():
            raise ValueError("invalid or duplicate case name")
        num_ids = case.get("num_ids", min(8, 1 << id_width))
        if type(num_ids) is not int or not 1 <= num_ids <= (1 << id_width):
            raise ValueError("num_ids must fit the selected AXI ID space")
        names.append(name)
        selected = case.get("mode", "control" if mode == "auto" else mode)
        if case_name is not None and "mode" in case and mode not in ("auto", selected):
            raise ValueError(name + " requires MODE=" + selected)
        random_fields = case.get("random", False) or selected == "rand"
        rng = random.Random(seed)
        target = out / name
        target.mkdir(parents=True, exist_ok=True)
        writes, reads = [], []
        init_writes, verify_reads = [], []
        capacity = case.get("legacy_mixed", False)
        count = 64 if capacity else case["count"]
        classes = [selected] * count
        if selected == "rand":
            # Stratify classes, then shuffle: every mixed run actually covers both.
            classes = ["control" if i % 2 == 0 else "data" for i in range(count)]
            rng.shuffle(classes)
        coverage = {m + "_" + op: 0 for m in ("control", "data") for op in ("write", "read")}
        for txn in range(count):
            is_data = classes[txn] == "data"
            if capacity:
                axi_id = ((128 + txn) if txn < 16 else 200 + txn % 2) % (1 << id_width)
            elif case.get("ids") == "multiple":
                axi_id = (128 + txn % num_ids) % (1 << id_width)
            else:
                axi_id = 200 % (1 << id_width)
            size = (txn % (7 if is_data else 4)) if case.get("burst_sweep") or capacity else (6 if is_data else 3)
            length = ((0, 1, 3, 7)[txn % 4] if capacity else
                      (1, 3, 7)[txn % 3] if case.get("burst_sweep") else 0)
            burst = ((1, 0, 2)[txn % 3] if capacity else
                     (0, 1, 2)[(txn // 4) % 3] if case.get("burst_sweep") else 1)
            if "burst_beats" in case:
                beats = case["burst_beats"]
                if not isinstance(beats, int) or not 1 <= beats <= 256:
                    raise ValueError("burst_beats must be in [1, 256]")
                length = beats - 1
            if random_fields:
                if case.get("random"):
                    axi_id = rng.randrange(min(8, 1 << id_width))
                size = rng.randrange(7 if is_data else 4)
                length = (0, 1, 3, 7)[txn % 4]
                burst = rng.randrange(3)
            if burst == 2 and length == 0:
                if capacity:
                    length = 1
                else:
                    burst = 1
            dest = (txn % len(routes[classes[txn]]) if capacity else
                    txn % 2 if case.get("destinations") == "alternate" else
                    rng.randrange(2) if case.get("destinations") == "random" else 0)
            if profile == "cosim":
                if case.get("require_ooo") or case.get("require_buffered"):
                    if len(routes[classes[txn]]) < 4:
                        raise ValueError("co-simulation reorder cases need four destinations")
                    dest = txn % 4
                elif case.get("capacity_test"):
                    dest = txn % len(routes[classes[txn]])
                elif case.get("destinations") == "random":
                    dest = rng.randrange(len(routes[classes[txn]]))
            route = routes[classes[txn]][dest]
            step = 1 << size
            offset = 256 + (txn % 8)*max(8, step)
            if random_fields:
                offset = 256 + rng.randrange(16)*64 + rng.randrange(64 // step)*step
            if profile == "cosim":
                # Disjoint transactions make the batch write/read barrier unambiguous.
                # The existing memory scoreboard supports INCR and single beats.
                burst = 1
                offset = txn * (512 if is_data else 64)
                if case.get("capacity_test"):
                    offset = (txn // len(routes[classes[txn]])) * (length + 1) * step
            address = route["base"] + offset
            operation = case.get("operation", "both")
            if case.get("random"):
                # Paired directions keep every class/single/burst category non-vacuous.
                operation = "both"
            if profile == "cosim":
                operation = "both"  # read cases initialize through the real write path
            if operation in ("write", "both"):
                # Keep the transaction marker in opaque AWUSER[7:0], below collective control.
                fields = _ax_fields(axi_id, address, length, size, True, user=txn & 0xff)
                fields[4] = str(burst)
                if profile == "cosim" and (case.get("partial_write") or case.get("concurrent_rw")):
                    init_writes.extend(fields)
                    if case.get("concurrent_rw"):
                        fields[1] = hex(address + (0x10000 if is_data else 0x800))
                        if int(fields[1], 16) + (length+1)*step > route["base"] + route["size"]:
                            raise ValueError("concurrent write crosses SAM window")
                writes.extend(fields)
                coverage[classes[txn]+"_write"] += 1
                span = (length+1)*(1 << size)
                for beat in range(length+1):
                    addr = address if burst == 0 else address + beat*(1 << size)
                    if burst == 2:
                        addr = (address & ~(span-1)) | (addr & (span-1))
                    if not route["base"] <= addr < route["base"] + route["size"] or addr >> 12 != address >> 12:
                        raise ValueError("burst crosses SAM or 4 KB boundary")
                    data, strobe, user = encode_write_beats(addr, size, 0, 512)[0].split()
                    if random_fields:
                        data = hex(rng.getrandbits(512))
                        strobe = hex(int(strobe, 16) & rng.getrandbits(64))
                    elif profile == "cosim" and (case.get("require_ooo") or case.get("require_buffered")):
                        # Repeated address bytes cannot distinguish reordered read responses.
                        data = hex(rng.getrandbits(512))
                    elif (case.get("burst_sweep") and txn % 3 == 0) or (capacity and txn % 5 == 0):
                        strobe = hex(int(strobe, 16) & 0x5555555555555555)
                    if profile == "cosim":
                        # The normal phase initializes all active bytes. Partial writes
                        # save this full-strobe version as a separate initialization phase.
                        strobe = encode_write_beats(addr, size, 0, 512)[0].split()[1]
                    if profile == "cosim" and (case.get("partial_write") or case.get("concurrent_rw")):
                        init_writes.append(f"{data} {strobe} {user}")
                        data = hex(int(data, 16) ^ ((1 << 512)-1))
                        if case.get("partial_write"):
                            mask = (0, 0x5555555555555555, 0xaaaaaaaaaaaaaaaa,
                                    1 << (addr % 64))[(txn + beat) % 4]
                            strobe = hex(int(strobe, 16) & mask)
                    writes.append(f"{data} {strobe} {user}")
            if operation in ("read", "both"):
                fields = _ax_fields(axi_id, address, length, size, False)
                fields[4] = str(burst)
                reads.extend(fields)
                if profile == "cosim" and case.get("concurrent_rw"):
                    fields[1] = hex(address + (0x10000 if is_data else 0x800))
                    verify_reads.extend(fields)
                coverage[classes[txn]+"_read"] += 1
        (target / "write.txt").write_text("\n".join(writes) + ("\n" if writes else ""))
        (target / "read.txt").write_text("\n".join(reads) + ("\n" if reads else ""))
        if init_writes:
            (target / "init_write.txt").write_text("\n".join(init_writes) + "\n")
        if verify_reads:
            (target / "verify_read.txt").write_text("\n".join(verify_reads) + "\n")
        defaults = dict(response_delay=1, min_outstanding=1, min_unique=1)
        args = ["+block_case", f"+case_id_width={id_width}", f"+case_name={name}",
                f"+mode={selected}", f"+seed={seed}", f"+random_case={int(case.get('random', False))}"]
        args += [f"+{key}={case.get(key, defaults.get(key, 0))}" for key in SCHEDULE]
        if profile == "cosim":
            args = [f"+case_name={name}", f"+seed={seed}",
                    f"+min_outstanding={case.get('min_outstanding', 1)}",
                    f"+min_unique={case.get('min_unique', 1)}",
                    f"+backpressure={int(name == 'backpressure')}",
                    f"+init_phase={int(bool(init_writes))}",
                    f"+concurrent_rw={int(bool(case.get('concurrent_rw')))}",
                    f"+stall_cycles={case.get('stall_cycles', 0)}",
                    f"+hold_cycles={case.get('hold_cycles', 0)}",
                    f"+capacity_test={int(bool(case.get('capacity_test')))}",
                    f"+data_case={int(selected == 'data')}",
                    f"+reorder_test={2 if case.get('require_buffered') else int(bool(case.get('require_ooo')))}"]
        (target / "schedule.txt").write_text("\n".join(args) + "\n")
        (target / "manifest.json").write_text(json.dumps(dict(case=name, mode=selected, seed=seed,
                                                              id_width=id_width, coverage=coverage,
                                                              **({"profile": profile} if profile == "cosim" else {})), indent=2)+"\n")
    (out / "cases.list").write_text("\n".join(names) + "\n")
    return names


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--topology", default=str(REPO / "sim/configs/mesh_2x2.yml"))
    parser.add_argument("--catalog", default=str(CATALOG))
    parser.add_argument("--id-width", type=int, choices=range(1, 9), default=8)
    parser.add_argument("--case", dest="case_name")
    parser.add_argument("--mode", choices=("auto", "control", "data", "rand"), default="auto")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--profile", choices=("standalone", "cosim"), default="standalone")
    args = parser.parse_args()
    generate(args.out, args.topology, args.id_width, args.catalog, args.mode, args.seed, args.case_name, args.profile)
