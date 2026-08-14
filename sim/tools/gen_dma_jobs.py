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
Every job names two windows: the issuing node's own (`local`) and the next
node's (`ext`).  Direction decides which is the source, the swap FlooNoC's
traffic generator makes on `flow.rw` (floogen/model/traffic.py:286-287).  A
WRITE job reads local and writes ext, so its write crosses the fabric and the
tile crossbar answers its read; a READ job swaps them, so its READ crosses the
fabric.  Both addresses are base(dst_id) + offset with the base packed by
address_map.pack() over the topology's route span -- the base c_model's
SamTable::packed computes from the same YAML, never a restatement of its
formula.

    offset(node, job) = _BASE_LOCAL + (node * jobs_per_node + job) * length

The offset carries the ISSUING node, not the source, so the two windows one job
touches are reached by that job alone whichever way its direction points them.
Both windows are checked against the memory tile's declared size.

axi_id
------
Every job of every node carries _AXI_ID, and it has to: idma_axi_read.sv never
reads r.id.  Each arriving R beat is pushed against the head of the read
datapath queue, which the legalizer fills in AR-issue order ACROSS job
boundaries, so the backend requires read data in global AR order -- an ordering
AXI guarantees per id and not between ids.  Two ids reaching two slaves at two
latencies is what breaks it, and this emitter's jobs do exactly that: a read
job's source is another node's window and the next job's is the local memory.

Upstream agrees on both sides.  FlooNoC's job format has no id field at all --
ten fields, and the job class constructor sets id = '0.  iDMA's own multi-channel
frontend (idma_inst64_top) does give each channel its own id, by replicating the
whole backend onto its own top-level AXI port in a genvar loop, so the ordering
requirement is enforced by port separation rather than lifted.

It costs coverage, on the NMU side.  nmu/rob.hpp keeps write_order_by_id_ and
read_order_by_id_ as arrays of AXI_ID_SPACE = 8, and every admission decision
keys on the id, so a single-id stream reaches one bucket of the eight and cannot
form the shape the DAT deadlock argument is about -- one id's fallback-allocate
interleaved with another id's bypass streak.  Downstream is where nothing is
lost: NSU_META_BUFFER_MAX_UNIQUE_IDS_DFLT is 1, so meta_buffer.hpp collapses
every upstream id onto one anyway.  Both gaps are recorded in
docs/known-limitations.md.
"""

import argparse
import os
import sys

import address_map
import gen_tb_top

# Local offset of the job window inside a tile's memory region.  Matches
# gen_test_patterns.py's base_local: offset 0 stays clear.
_BASE_LOCAL = 0x1000

# max_src_len / max_dst_len reach the backend as beo.*_max_llen, a THREE-BIT LOG
# length that only acts when the matching *_reduce_len bit is set
# (idma_pkg.sv:75-82, idma_legalizer_page_splitter.sv:37:
# page_addr_width = OffsetWidth + (reduce_len_i ? max_llen_i : 'd8)).  So the
# field states log2(beats), 0 to 7, and 8 -- outside the field -- encodes "no
# reduction", the same 'd8 the splitter uses when reduce_len is clear: the
# legalizer then splits on the 4 KiB page alone.  A beat COUNT here would be
# silently mistranslated, most values to the 1-beat minimum.
_MAX_LOG_LEN_NONE = 8

# idma_pkg::protocol_e AXI (sim/dv/idma-0.6.5/src/idma_pkg.sv:91).  Both ends
# of every job are the tile's AXI.
_PROTOCOL_AXI = 0

# The one AXI id every job carries, upstream's own value: FlooNoC's job class
# constructor sets id = '0.  See the module docstring for why one id is the
# correct usage of an iDMA backend rather than a concession.
_AXI_ID = 0


def job_lines(length, src_addr, dst_addr, axi_id):
    """The eleven field lines of one job, in idma_job_driver.sv's read order."""
    return [str(length), f"0x{src_addr:x}", f"0x{dst_addr:x}",
            str(_PROTOCOL_AXI), str(_PROTOCOL_AXI),
            str(_MAX_LOG_LEN_NONE), str(_MAX_LOG_LEN_NONE),
            "0", "0",     # aw_decoupled, rw_decoupled -- the backend's defaults
            "0",          # num_errors -- the error path is out of scope
            str(axi_id)]


def job_table(topo, jobs_per_node, length):
    """Every node's jobs as
    [(node_idx, src_ep, dst_ep, src_addr, dst_addr, axi_id), ...].

    node_idx is the node whose jobs.txt the row lands in.  src_ep and dst_ep are
    the endpoints whose memories the bytes leave and arrive at, which is a
    different question: a read job's source is another node's window.

    The ONE place a job's geometry is computed.  This file writes it to
    jobs.txt; gen_tb_top.py stamps the same addresses AND the same endpoints
    into the DMA top's memory preload and region compare, so the stimulus and
    the check that reads it back cannot disagree about where a job's bytes are.
    """
    x_span, y_span = gen_tb_top._route_span(topo["topology"])[:2]
    bases, entries = address_map.pack(topo.get("address_map"), x_span, y_span)
    # The router array only: a peripheral is an endpoint, not a node, so every
    # job is router to router and XY reaches between any two of them.
    nodes, _x_dim, _y_dim = gen_tb_top._nodes(topo)
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    n_nodes = len(nodes)
    out = []
    for (idx, _x, _y, local_cid) in nodes:
        ext_idx, _ex, _ey, ext_cid = nodes[(idx + 1) % n_nodes]
        for job in range(jobs_per_node):
            seq = idx * jobs_per_node + job
            offset = _BASE_LOCAL + seq * length
            limit = min(sizes[local_cid], sizes[ext_cid])
            if offset + length > limit:
                sys.exit(f"ERROR: node{idx} job {job} window {offset:#x}+{length:#x} "
                         f"overruns the {limit:#x} B memory tile; reduce "
                         f"--jobs-per-node or --length")
            # Odd jobs read, even jobs write, so every node issues both. One
            # direction alone leaves the opposite path of the NMU untouched.
            is_read = job % 2 == 1
            src_ep, src_cid = (ext_idx, ext_cid) if is_read else (idx, local_cid)
            dst_ep, dst_cid = (idx, local_cid) if is_read else (ext_idx, ext_cid)
            out.append((idx, src_ep, dst_ep, bases[src_cid] + offset,
                        bases[dst_cid] + offset, _AXI_ID))
    return out


def emit_jobs(out_root, jobs, length):
    """Write node<i>/jobs.txt from job_table()'s rows."""
    per_node = {}
    for (node_idx, _src_ep, _dst_ep, src_addr, dst_addr, axi_id) in jobs:
        per_node.setdefault(node_idx, []).extend(
            job_lines(length, src_addr, dst_addr, axi_id))
    for idx, lines in per_node.items():
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
    emit_jobs(a.out, job_table(topo, a.jobs_per_node, a.length), a.length)


if __name__ == "__main__":
    main(sys.argv[1:])
