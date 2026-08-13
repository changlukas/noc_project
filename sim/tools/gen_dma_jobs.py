#!/usr/bin/env python3
"""Emit per-node iDMA job files (jobs.txt) from a topology's address map.

Usage:
    gen_dma_jobs.py --topology mesh_2x2_vc1 --out <dir> \\
        [--jobs-per-node 4] [--length 0x400]

Writes <out>/node<i>/jobs.txt for each node i. Eleven lines per job, in
idma_job_driver.sv's read order:

    length, src_addr, dst_addr, src_protocol, dst_protocol, max_src_len,
    max_dst_len, aw_decoupled, rw_decoupled, num_errors, axi_id

The first ten are FlooNoC's job file; axi_id is added here (see below).
Addresses are hex with an 0x prefix, every other field decimal -- the same
lexical shape gen_test_patterns.py writes write.txt in.

Addresses
---------
A job reads the node's OWN memory window and writes the next node's, so the
read is answered by the tile crossbar and the write crosses the fabric.  Both
addresses are base(dst_id) + offset with the base packed by address_map.pack()
over the topology's route span -- the base c_model's SamTable::packed computes
from the same YAML, never a restatement of its formula.

    offset(node, job) = _BASE_LOCAL + (node * jobs_per_node + job) * length

The offset carries the source node, so two sources writing one destination
cannot overlap whatever destination rule this grows.  Both windows are checked
against the memory tile's declared size.

axi_id
------
FlooNoC leaves idma_job.id at '0, so every transfer of a run carries one AXI
id.  The NMU keeps a reorder-buffer slot and a meta-buffer bucket per id, so a
single-id stream reaches one of each.  The field cycles over the id space
(2**INITIATOR_ID_WIDTH, the tile crossbar's slave port) so every per-id
structure is reached.  A coverage requirement, not a performance one.
"""

import argparse
import os
import sys

import address_map
import gen_tb_top
from gen_test_patterns import axi_widths

# Local offset of the job window inside a tile's memory region.  Matches
# gen_test_patterns.py's base_local: offset 0 stays clear.
_BASE_LOCAL = 0x1000

# AxLEN is 8 b, so 256 beats is the longest AXI burst; the legalizer takes the
# bound as a beat count minus one, like AxLEN itself.
_MAX_BURST_LEN = 255

# idma_pkg::protocol_e AXI (sim/dv/idma-0.6.5/src/idma_pkg.sv:91).  Both ends
# of every job are the tile's AXI.
_PROTOCOL_AXI = 0


def job_lines(length, src_addr, dst_addr, axi_id):
    """The eleven field lines of one job, in idma_job_driver.sv's read order."""
    return [str(length), f"0x{src_addr:x}", f"0x{dst_addr:x}",
            str(_PROTOCOL_AXI), str(_PROTOCOL_AXI),
            str(_MAX_BURST_LEN), str(_MAX_BURST_LEN),
            "0", "0",     # aw_decoupled, rw_decoupled -- the backend's defaults
            "0",          # num_errors -- the error path is out of scope
            str(axi_id)]


def emit_jobs(out_root, nodes, bases, sizes, jobs_per_node, length, num_axi_ids):
    """Write node<i>/jobs.txt for every node.

    nodes: [(idx, x, y, coord_id), ...] from gen_tb_top._nodes().
    bases/sizes: {coord_id: base} and {coord_id: size} of the MEMORY space,
    from address_map.pack().
    """
    n_nodes = len(nodes)
    for (idx, _x, _y, src_cid) in nodes:
        dst_cid = nodes[(idx + 1) % n_nodes][3]
        lines = []
        for job in range(jobs_per_node):
            seq = idx * jobs_per_node + job
            offset = _BASE_LOCAL + seq * length
            limit = min(sizes[src_cid], sizes[dst_cid])
            if offset + length > limit:
                sys.exit(f"ERROR: node{idx} job {job} window {offset:#x}+{length:#x} "
                         f"overruns the {limit:#x} B memory tile; reduce "
                         f"--jobs-per-node or --length")
            lines += job_lines(length, bases[src_cid] + offset,
                               bases[dst_cid] + offset, seq % num_axi_ids)
        node_dir = os.path.join(out_root, f"node{idx}")
        os.makedirs(node_dir, exist_ok=True)
        with open(os.path.join(node_dir, "jobs.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Emit per-node iDMA job files (jobs.txt) from a topology's address map.")
    ap.add_argument("--topology", default="mesh_4x4_vc1",
                    help="Topology name (matches sim/topologies/<name>.yaml)")
    ap.add_argument("--out", required=True,
                    help="Output directory; writes <out>/node<i>/jobs.txt")
    ap.add_argument("--jobs-per-node", type=int, default=4,
                    help="Jobs in each node's file")
    ap.add_argument("--length", type=lambda v: int(v, 0), default=0x400,
                    help="Bytes moved per job (default 0x400 = 16 beats of the 512 b bus)")
    a = ap.parse_args(argv)

    topo = gen_tb_top.load_topology(a.topology)
    x_span, y_span = gen_tb_top._route_span(topo["topology"])[:2]
    bases, entries = address_map.pack(topo.get("address_map"), x_span, y_span)
    nodes, _x_dim, _y_dim = gen_tb_top._nodes(topo)
    # A peripheral sits outside the tile region and XY routing reaches it from
    # its own row only (gen_test_patterns.peripheral_reaches), so "the next
    # node" is not a legal destination rule there.  Refuse rather than emit
    # stimulus the NMU's reachability check would abort on.
    if gen_tb_top._peripherals(topo):
        sys.exit(f"ERROR: topology {a.topology} carries a peripheral; the job emitter's "
                 f"destination rule (the next node) does not respect XY reachability "
                 f"from one")
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    emit_jobs(a.out, nodes, bases, sizes, a.jobs_per_node, a.length,
              1 << axi_widths()["id"])


if __name__ == "__main__":
    main(sys.argv[1:])
