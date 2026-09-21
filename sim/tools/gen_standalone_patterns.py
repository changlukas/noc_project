#!/usr/bin/env python3
"""Generate simulator-independent AXI files and NMU schedule from directed cases."""
import argparse
import json
from pathlib import Path
import yaml
from address_map import pack_config
from gen_test_patterns import _ax_fields, encode_write_beats
from gen_nmu_standalone_patterns import generate as generate_mixed

REPO = Path(__file__).resolve().parents[2]
CATALOG = REPO / "sim/test_patterns/standalone/cases.json"
SCHEDULE = ("response_order", "response_delay", "startup_delay",
            "stall_enable", "reset_warmup", "min_outstanding", "min_unique",
            "require_ooo", "require_buffered", "require_capacity", "require_stall")


def generate(out, topology, id_width=8, catalog=CATALOG):
    out = Path(out)
    _, entries = pack_config(yaml.safe_load(Path(topology).read_text()))
    bases = [e["base"] for e in entries if e["space"] == "config"]
    if len(bases) < 2:
        raise ValueError("standalone suite needs two config destinations")
    if id_width not in (1, 3, 8):
        raise ValueError("validated ID widths: 1, 3, 8")
    names = []
    for case in json.loads(Path(catalog).read_text())["cases"]:
        name = case["name"]
        if name in names or not name.replace("_", "").isalnum():
            raise ValueError("invalid or duplicate case name")
        names.append(name)
        target = out / name
        target.mkdir(parents=True, exist_ok=True)
        if case.get("legacy_mixed"):
            generate_mixed(target, topology, id_width)
        else:
            writes, reads = [], []
            for txn in range(case["count"]):
                axi_id = (128 + (txn % min(8, 1 << id_width)) if case.get("ids") == "multiple" else 200) % (1 << id_width)
                size = txn % 4 if case.get("burst_sweep") else 3
                length = (1, 3, 7)[txn % 3] if case.get("burst_sweep") else 0
                burst = (0, 1, 2)[(txn // 4) % 3] if case.get("burst_sweep") else 1
                address = bases[txn % 2 if case.get("destinations") == "alternate" else 0] + 256 + (txn % 8)*8
                operation = case.get("operation", "both")
                if operation in ("write", "both"):
                    fields = _ax_fields(axi_id, address, length, size, True, user=txn)
                    fields[4] = str(burst)
                    writes.extend(fields)
                    span = (length+1)*(1 << size)
                    for beat in range(length+1):
                        addr = address if burst == 0 else address + beat*(1 << size)
                        if burst == 2:
                            addr = (address & ~(span-1)) | (addr & (span-1))
                        data, strobe, user = encode_write_beats(addr, size, 0, 512)[0].split()
                        if case.get("burst_sweep") and txn % 3 == 0:
                            strobe = hex(int(strobe, 16) & 0x5555555555555555)
                        writes.append(f"{data} {strobe} {user}")
                if operation in ("read", "both"):
                    fields = _ax_fields(axi_id, address, length, size, False)
                    fields[4] = str(burst)
                    reads.extend(fields)
            (target / "write.txt").write_text("\n".join(writes) + ("\n" if writes else ""))
            (target / "read.txt").write_text("\n".join(reads) + ("\n" if reads else ""))
        defaults = dict(response_delay=1, min_outstanding=1, min_unique=1)
        args = ["+block_case", f"+case_id_width={id_width}", f"+case_name={name}"]
        args += [f"+{key}={case.get(key, defaults.get(key, 0))}" for key in SCHEDULE]
        (target / "schedule.txt").write_text("\n".join(args) + "\n")
    (out / "cases.list").write_text("\n".join(names) + "\n")
    return names


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--topology", default=str(REPO / "sim/configs/mesh_2x2.yml"))
    parser.add_argument("--id-width", type=int, choices=(1, 3, 8), default=8)
    args = parser.parse_args()
    generate(args.out, args.topology, args.id_width)
