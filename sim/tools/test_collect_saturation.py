from collect_saturation import parse_bw


def test_parse_bw_sums_read_and_write_across_nodes():
    # Real axi_bw_monitor line format (per node, per direction).
    log = (
        "[Monitor node0.manager][Read] Latency: 100.6 +- 27.8, BW: 4.00 Bits/cycle, Util: 1.5%\n"
        "[Monitor node0.manager][Write] Latency: 100.6 +- 27.8, BW: 2.00 Bits/cycle, Util: 1.5%\n"
        "[Monitor node1.manager][Read] Latency: 100.6 +- 27.8, BW: 3.00 Bits/cycle, Util: 1.5%\n"
        "[Monitor node1.manager][Write] Latency: 100.6 +- 27.8, BW: 1.00 Bits/cycle, Util: 1.5%\n"
    )
    out = parse_bw(log)
    assert out["read_bw"] == 7.0
    assert out["write_bw"] == 3.0


def test_parse_bw_empty_log_is_zero():
    out = parse_bw("no monitor lines here\n")
    assert out["read_bw"] == 0.0
    assert out["write_bw"] == 0.0
