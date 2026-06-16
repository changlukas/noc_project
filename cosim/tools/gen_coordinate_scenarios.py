#!/usr/bin/env python3
"""Emit per-node coordinate-bearing scenario variants for bidirectional cosim.

Usage: gen_coordinate_scenarios.py <src scenario.yaml> <out_dir>
Writes <out_dir>/node0/scenario.yaml (identity) and <out_dir>/node1/scenario.yaml
(+NODE1_OFFSET on every transaction addr and on config.memory_base). File
references (data_file/dump_file/strb_file) are rewritten to absolute paths against
the source directory so they resolve from any cwd (load_scenario resolves relative
paths against the YAML's own dir).
"""
import copy
import os
import sys
import yaml

NODE1_OFFSET = 0x100000000  # bit 32 -> dst coordinate (1,0)
_FILE_KEYS = ("data_file", "dump_file", "strb_file")


def _as_int(v):
    return v if isinstance(v, int) else int(str(v), 0)


def _emit(sc, src_dir, out_dir, offset):
    sc = copy.deepcopy(sc)
    for t in sc.get("transactions", []):
        if offset:
            t["addr"] = _as_int(t["addr"]) + offset
        for k in _FILE_KEYS:
            if t.get(k) and not os.path.isabs(t[k]):
                t[k] = os.path.join(src_dir, t[k])
    if offset:
        cfg = sc.setdefault("config", {})
        cfg["memory_base"] = _as_int(cfg.get("memory_base", 0)) + offset
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "scenario.yaml"), "w") as f:
        yaml.safe_dump(sc, f, sort_keys=False)


def main(argv):
    src, out = argv[1], argv[2]
    src_dir = os.path.dirname(os.path.abspath(src))
    with open(src) as f:
        sc = yaml.safe_load(f)
    _emit(sc, src_dir, os.path.join(out, "node0"), 0)
    _emit(sc, src_dir, os.path.join(out, "node1"), NODE1_OFFSET)


if __name__ == "__main__":
    main(sys.argv)
