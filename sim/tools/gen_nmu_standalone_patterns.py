#!/usr/bin/env python3
"""Directed control-plane files using the shared AXI file-master encoder."""
import argparse
from pathlib import Path
import yaml
from address_map import pack_config
from gen_test_patterns import _ax_fields, encode_write_beats


def generate(out, topology, id_width):
    _, entries = pack_config(yaml.safe_load(Path(topology).read_text()))
    bases = [e["base"] for e in entries if e["space"] == "config"]
    writes, reads = [], []
    for txn in range(64):
        axi_id = ((128 + txn) if txn < 16 else 200 + (txn % 2)) % (1 << id_width)
        size = txn % 4
        length = (0, 1, 3, 7)[txn % 4]
        burst = (1, 0, 2)[txn % 3]
        if burst == 2 and length == 0:
            length = 1
        address = bases[txn % len(bases)] + 256 + (txn % 8)*8
        aw = _ax_fields(axi_id, address, length, size, True, user=txn)
        ar = _ax_fields(axi_id, address, length, size, False)
        aw[4] = ar[4] = str(burst)
        writes.extend(aw)
        reads.extend(ar)
        step = 1 << size
        span = (length + 1)*step
        for beat in range(length + 1):
            addr = address if burst == 0 else address + beat*step
            if burst == 2:
                addr = (address & ~(span-1)) | (addr & (span-1))
            line = encode_write_beats(addr, size, 0, 512)[0]
            data, strobe, user = line.split()
            # Legal sparse byte enables, including zero-strobe beats.
            if txn % 5 == 0:
                strobe = hex(int(strobe, 16) & 0x5555555555555555)
            writes.append(f"{data} {strobe} {user}")
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "write.txt").write_text("\n".join(writes) + "\n")
    (out / "read.txt").write_text("\n".join(reads) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--topology", required=True)
    parser.add_argument("--id-width", type=int, choices=(1, 3, 8), default=8)
    args = parser.parse_args()
    generate(args.out, args.topology, args.id_width)
