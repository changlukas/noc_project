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
    g.main(["--topology", "mesh_2x2_vc1", "--out", str(out), "--jobs-per-node", "4"])
    _bases, entries = address_map.pack(_topology("mesh_2x2_vc1")["address_map"], 2, 2)
    windows = [(e["base"], e["base"] + e["size"]) for e in entries]

    def _owner(addr):
        return [e["dst_id"] for e in entries if e["base"] <= addr < e["base"] + e["size"]][0]

    for node in range(4):
        for job in _parse_jobs(out / f"node{node}" / "jobs.txt"):
            assert any(lo <= job["src_addr"] < hi for lo, hi in windows)
            assert any(lo <= job["dst_addr"] < hi for lo, hi in windows)
            # The write has to cross the fabric: the two windows are different
            # nodes. Without this, both addresses in the emitting node's own
            # window would pass -- and never leave the tile crossbar.
            assert _owner(job["src_addr"]) != _owner(job["dst_addr"])


def test_burst_bound_is_a_log_length(tmp_path):
    """max_src_len / max_dst_len reach the backend as beo.*_max_llen, a 3-bit
    LOG length gated by *_reduce_len, so the file states log2(beats) 0..7 with 8
    for "no reduction". A beat count truncates instead of erroring -- 3'(8) and
    3'(16) are both 0, which asks for 1-beat bursts -- so idma_job_driver.sv
    rejects anything above 8 and this pins the emitter to the same range."""
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2_vc1", "--out", str(out)])
    for job in _parse_jobs(out / "node0" / "jobs.txt"):
        # Equality, not a range: 0 is inside the field and means reduce_len = 1
        # with page_addr_width = OffsetWidth, i.e. one-beat bursts -- the exact
        # regression this pins against.
        assert job["max_src_len"] == g._MAX_LOG_LEN_NONE
        assert job["max_dst_len"] == g._MAX_LOG_LEN_NONE


def test_jobs_use_more_than_one_axi_id(tmp_path):
    """FlooNoC leaves idma_job.id at '0, so every transfer uses one ID. This
    NMU keeps a reorder-buffer slot and a meta-buffer bucket per ID, so a
    single-ID stream would report that a DMA runs while touching one of
    eight.

    At the SHIPPED geometry -- the one the gate builds, not a larger one --
    each node emits jobs_per_node distinct IDs and no two nodes share one."""
    n = gen_tb_top._DMA_JOBS_PER_NODE
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2_vc1", "--out", str(out), "--jobs-per-node", str(n)])
    per_node = [{job["axi_id"] for job in _parse_jobs(out / f"node{i}" / "jobs.txt")}
                for i in range(4)]
    for ids in per_node:
        assert len(ids) == n
    # A node's IDs are its own slice of the space, not the whole of it.
    assert not (per_node[0] & per_node[1])
