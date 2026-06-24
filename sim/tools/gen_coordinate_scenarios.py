#!/usr/bin/env python3
"""Emit per-node coordinate-bearing scenario variants for the co-sim.

Usage: gen_coordinate_scenarios.py <src scenario.yaml> <out_dir> [--topology NAME]

Writes <out_dir>/node<i>/scenario.yaml for each node i in the topology. Node i's
variant adds `coord_id(i) << 32` to every transaction addr and to
config.memory_base, where coord_id = (y << X_WIDTH) | x mirrors the c_model
route_compute / addr_trans dst_id encoding (bit 32+ of the address selects the
destination tile). node0 (coord 0) is the identity variant.

--topology defaults to mesh_4x4_vc1 (16 nodes): it emits node0..node15 at coord
ids matching the (y<<X_WIDTH)|x encoding.

File references (data_file/dump_file/strb_file) are rewritten to relative paths
from the variant subdir back to the source data file (load_scenario resolves
relative paths against the YAML's own dir), so a materialized variant stays valid
wherever the scenario tree is checked out.
"""
import argparse
import copy
import os
import sys
import yaml

# Coordinate-id composition must mirror c_model addr_trans / route_compute:
# dst_id = (y << X_WIDTH) | x, address bit 32+ carries dst_id.
X_WIDTH = 4
ADDR_DST_SHIFT = 32
_FILE_KEYS = ("data_file", "dump_file", "strb_file")


def _as_int(v):
    return v if isinstance(v, int) else int(str(v), 0)


def _node_coords(name):
    """Return ordered [(idx, coord_id), ...] for a topology name's nodes."""
    here = os.path.dirname(os.path.abspath(__file__))
    topo_path = os.path.join(here, "..", "topologies", f"{name}.yaml")
    with open(topo_path) as f:
        topo = yaml.safe_load(f)
    x_dim = topo["topology"]["x_dim"]
    y_dim = topo["topology"]["y_dim"]
    out = []
    idx = 0
    for y in range(y_dim):
        for x in range(x_dim):
            out.append((idx, (y << X_WIDTH) | x))
            idx += 1
    return out


def _emit(sc, src_dir, out_dir, offset):
    sc = copy.deepcopy(sc)
    for t in sc.get("transactions", []):
        if offset:
            t["addr"] = _as_int(t["addr"]) + offset
        for k in _FILE_KEYS:
            if t.get(k) and not os.path.isabs(t[k]):
                abs_src = os.path.join(src_dir, t[k])
                try:
                    rel = os.path.relpath(abs_src, out_dir)
                except ValueError:
                    # Different Windows drives have no relative path; the
                    # absolute one still resolves from the YAML's own dir.
                    rel = os.path.abspath(abs_src)
                t[k] = rel.replace(os.sep, "/")
    if offset:
        cfg = sc.setdefault("config", {})
        cfg["memory_base"] = _as_int(cfg.get("memory_base", 0)) + offset
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "scenario.yaml"), "w") as f:
        yaml.safe_dump(sc, f, sort_keys=False)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--topology", default="mesh_4x4_vc1")
    a = ap.parse_args(argv[1:])

    src_dir = os.path.dirname(os.path.abspath(a.src))
    with open(a.src) as f:
        sc = yaml.safe_load(f)
    for idx, coord_id in _node_coords(a.topology):
        _emit(sc, src_dir, os.path.join(a.out, f"node{idx}"), coord_id << ADDR_DST_SHIFT)


if __name__ == "__main__":
    main(sys.argv)
