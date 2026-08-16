import pytest

import address_map
import gen_dma_jobs as g
import gen_tb_top

# jobs.txt field order (idma_job_driver.sv reads them in this order).
_FIELDS = ("length", "src_addr", "dst_addr", "src_protocol", "dst_protocol",
           "max_src_len", "max_dst_len", "aw_decoupled", "rw_decoupled",
           "num_errors", "axi_id")


def _topology(name):
    return gen_tb_top.load_topology(name)


def _parse_jobs(path):
    """Parse a jobs.txt back into jobs. Mirrors gen_dma_jobs.job_lines' field order."""
    toks = [t for t in path.read_text().split("\n") if t != ""]
    assert len(toks) % len(_FIELDS) == 0, f"{path} is not a whole number of jobs"
    return [dict(zip(_FIELDS, (int(t, 0) for t in toks[i:i + len(_FIELDS)])))
            for i in range(0, len(toks), len(_FIELDS))]


def test_every_job_addresses_a_real_sam_region(tmp_path):
    """A job's src and dst are SAM addresses, so both must land inside a
    region the address map actually declares. FlooNoC's generator has no
    knowledge of this map, which is why the emitter is written here rather
    than ported."""
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2", "--out", str(out), "--jobs-per-node", "4"])
    _bases, entries = address_map.pack_document(_topology("mesh_2x2"))
    windows = [(e["base"], e["base"] + e["size"]) for e in entries]

    def _owner(addr):
        return [e["dst_id"] for e in entries if e["base"] <= addr < e["base"] + e["size"]][0]

    for node in range(4):
        for job in _parse_jobs(out / f"node{node}" / "jobs.txt"):
            assert any(lo <= job["src_addr"] < hi for lo, hi in windows)
            assert any(lo <= job["dst_addr"] < hi for lo, hi in windows)
            # One end of every job has to cross the fabric -- the write for a
            # write job, the read for a read job -- so the two windows are
            # different nodes. Without this, both addresses in the emitting
            # node's own window would pass and never leave the tile crossbar.
            assert _owner(job["src_addr"]) != _owner(job["dst_addr"])


@pytest.mark.parametrize("rw", ["read", "write"])
def test_direction_crosses_the_fabric(tmp_path, rw):
    """A job whose source is the issuing node's OWN window has its read answered
    by the tile crossbar, so a run of those alone never puts an AR on the NoC --
    the NMU's reorder buffer and its read admission clauses stay at zero.
    Direction is run-level (FlooNoC's util/gen_jobs.py --rw), not per-job: a
    read run sources every job from the next node's window, a write run sources
    every job from its own, so the two directions have to run as two gates, not
    one file with both mixed in.

    At the SHIPPED geometry -- the one the gate builds, not a larger one."""
    n = gen_tb_top._DMA_JOBS_PER_NODE
    topo = _topology("mesh_2x2")
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2", "--out", str(out), "--jobs-per-node", str(n),
            "--rw", rw])
    bases, entries = address_map.pack_document(topo)
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    nodes, _x_dim, _y_dim = gen_tb_top._nodes(topo)

    for (idx, _x, _y, cid) in nodes:
        own = range(bases[cid], bases[cid] + sizes[cid])
        for job in _parse_jobs(out / f"node{idx}" / "jobs.txt"):
            # read: source is the next node's window (remote), so every AR
            # crosses the fabric. write: source is the issuing node's own
            # window (local); the AW crosses instead.
            assert (job["src_addr"] not in own) == (rw == "read")


def test_read_write_swap_src_dst(tmp_path):
    """The same job index has src and dst swapped between --rw read and
    --rw write -- job_table's is_read flips which end is source, and nothing
    else about the geometry moves."""
    n = gen_tb_top._DMA_JOBS_PER_NODE
    out_read = tmp_path / "read"
    out_write = tmp_path / "write"
    g.main(["--topology", "mesh_2x2", "--out", str(out_read),
            "--jobs-per-node", str(n), "--rw", "read"])
    g.main(["--topology", "mesh_2x2", "--out", str(out_write),
            "--jobs-per-node", str(n), "--rw", "write"])
    for idx in range(4):
        reads = _parse_jobs(out_read / f"node{idx}" / "jobs.txt")
        writes = _parse_jobs(out_write / f"node{idx}" / "jobs.txt")
        for read_job, write_job in zip(reads, writes):
            assert read_job["src_addr"] == write_job["dst_addr"]
            assert read_job["dst_addr"] == write_job["src_addr"]


def test_burst_bound_is_a_log_length(tmp_path):
    """max_src_len / max_dst_len reach the backend as beo.*_max_llen, a 3-bit
    LOG length gated by *_reduce_len, so the file states log2(beats) 0..7 with 8
    for "no reduction". A beat count truncates instead of erroring -- 3'(8) and
    3'(16) are both 0, which asks for 1-beat bursts -- so idma_job_driver.sv
    rejects anything above 8 and this pins the emitter to the same range."""
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2", "--out", str(out)])
    for job in _parse_jobs(out / "node0" / "jobs.txt"):
        # Equality, not a range: 0 is inside the field and means reduce_len = 1
        # with page_addr_width = OffsetWidth, i.e. one-beat bursts -- the exact
        # regression this pins against.
        assert job["max_src_len"] == g._MAX_LOG_LEN_NONE
        assert job["max_dst_len"] == g._MAX_LOG_LEN_NONE


def test_every_job_carries_one_axi_id(tmp_path):
    """One ID for the whole run, because an iDMA backend cannot take two.
    idma_axi_read.sv never reads r.id: an R beat is pushed against the head of
    the read datapath queue, which the legalizer fills in AR-issue order across
    job boundaries, so the backend needs read data in global AR order. AXI
    guarantees that per ID only.

    Emitting one ID per job passed while every read was answered by the local
    memory. It stopped passing the moment a read crossed the fabric: the fabric
    read and the local read of the next job returned out of order, and the two
    jobs wrote each other's payload byte for byte. The constraint is pinned here
    rather than left to be rediscovered."""
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2", "--out", str(out),
            "--jobs-per-node", str(gen_tb_top._DMA_JOBS_PER_NODE)])
    ids = {job["axi_id"] for i in range(4)
           for job in _parse_jobs(out / f"node{i}" / "jobs.txt")}
    assert ids == {g._AXI_ID}
