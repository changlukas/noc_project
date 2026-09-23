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


def generate(out, topology, id_width=8, catalog=CATALOG, mode="auto", seed=1, case_name=None):
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
    if any(len(v) < 2 for v in routes.values()):
        raise ValueError("standalone suite needs two control/data destinations")
    if id_width not in (1, 3, 8) or mode not in ("auto", "control", "data", "rand"):
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
        if name in names or not name.replace("_", "").isalnum():
            raise ValueError("invalid or duplicate case name")
        names.append(name)
        selected = case.get("mode", "control" if mode == "auto" else mode)
        if case_name is not None and "mode" in case and mode not in ("auto", selected):
            raise ValueError(name + " requires MODE=" + selected)
        random_fields = case.get("random", False) or selected == "rand"
        rng = random.Random(seed)
        target = out / name
        target.mkdir(parents=True, exist_ok=True)
        writes, reads = [], []
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
                axi_id = (128 + txn % min(8, 1 << id_width)) % (1 << id_width)
            else:
                axi_id = 200 % (1 << id_width)
            size = (txn % (7 if is_data else 4)) if case.get("burst_sweep") or capacity else (6 if is_data else 3)
            length = ((0, 1, 3, 7)[txn % 4] if capacity else
                      (1, 3, 7)[txn % 3] if case.get("burst_sweep") else 0)
            burst = ((1, 0, 2)[txn % 3] if capacity else
                     (0, 1, 2)[(txn // 4) % 3] if case.get("burst_sweep") else 1)
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
            route = routes[classes[txn]][dest]
            step = 1 << size
            offset = 256 + (txn % 8)*max(8, step)
            if random_fields:
                offset = 256 + rng.randrange(16)*64 + rng.randrange(64 // step)*step
            address = route["base"] + offset
            operation = case.get("operation", "both")
            if case.get("random"):
                # Paired directions keep every class/single/burst category non-vacuous.
                operation = "both"
            if operation in ("write", "both"):
                fields = _ax_fields(axi_id, address, length, size, True, user=txn)
                fields[4] = str(burst)
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
                    elif (case.get("burst_sweep") and txn % 3 == 0) or (capacity and txn % 5 == 0):
                        strobe = hex(int(strobe, 16) & 0x5555555555555555)
                    writes.append(f"{data} {strobe} {user}")
            if operation in ("read", "both"):
                fields = _ax_fields(axi_id, address, length, size, False)
                fields[4] = str(burst)
                reads.extend(fields)
                coverage[classes[txn]+"_read"] += 1
        (target / "write.txt").write_text("\n".join(writes) + ("\n" if writes else ""))
        (target / "read.txt").write_text("\n".join(reads) + ("\n" if reads else ""))
        defaults = dict(response_delay=1, min_outstanding=1, min_unique=1)
        args = ["+block_case", f"+case_id_width={id_width}", f"+case_name={name}",
                f"+mode={selected}", f"+seed={seed}", f"+random_case={int(case.get('random', False))}"]
        args += [f"+{key}={case.get(key, defaults.get(key, 0))}" for key in SCHEDULE]
        (target / "schedule.txt").write_text("\n".join(args) + "\n")
        (target / "manifest.json").write_text(json.dumps(dict(case=name, mode=selected, seed=seed,
                                                              id_width=id_width, coverage=coverage), indent=2)+"\n")
    (out / "cases.list").write_text("\n".join(names) + "\n")
    return names


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--topology", default=str(REPO / "sim/configs/mesh_2x2.yml"))
    parser.add_argument("--catalog", default=str(CATALOG))
    parser.add_argument("--id-width", type=int, choices=(1, 3, 8), default=8)
    parser.add_argument("--case", dest="case_name")
    parser.add_argument("--mode", choices=("auto", "control", "data", "rand"), default="auto")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    generate(args.out, args.topology, args.id_width, args.catalog, args.mode, args.seed, args.case_name)
