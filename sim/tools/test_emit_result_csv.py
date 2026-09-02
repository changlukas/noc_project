import csv
import sys

import pytest

import emit_result_csv as e

# Read and Write carry different sample counts on purpose: an implementation
# that averaged the printed means instead of weighting them by N would read
# 50.0 network and 70.0 open here, not 45.0 and 60.0.
LOG_HEAD = """[Config] max_unique_ids=1 max_outstanding=32 dat_num_vc=2 router_vc_depth=8 mst_stall_random=0 ni_dat_rx_vc_depth=8
[TrafficMeta] active_sources=16 write_bursts=3200
"""

ROUND_LOG = """[TrafficMeta] active_sources=15 write_bursts=15
[RoundPerf] start_cycle=10 completion_cycle=210 round_cycles=200 active_sources=15 write_bursts=15
PASS: all 16 nodes done, non-vacuous
"""

LOG = LOG_HEAD + """[Monitor node0.master][Read] Latency: 40.00 +- 1.00, N: 300, BW: 64.00 Bits/cycle, Util: 10.00%
[Monitor node0.master][Write] Latency: 60.00 +- 1.00, N: 100, BW: 64.00 Bits/cycle, Util: 10.00%
[SrcQueue node0][Read] mean: 10.00, N: 300
[SrcQueue node0][Write] mean: 30.00, N: 100
"""


def _run(tmp_path, monkeypatch, log_text, **overrides):
    log = tmp_path / "run.log"
    log.write_text(log_text)
    out = tmp_path / "result.csv"
    args = {"--topology": "mesh_4x4", "--pattern": "neighbor", "--injection-mode": "1",
            "--injection-rate": "0.5", "--injection-count": "200", "--seed": "1",
            "--burst-len": "32", "--stim-size": "6"}
    args.update(overrides)
    argv = ["emit_result_csv", "--log", str(log), "--out", str(out)]
    for k, v in args.items():
        argv += [k, v]
    monkeypatch.setattr(sys, "argv", argv)
    e.main()
    return next(csv.DictReader(out.open()))


def test_open_latency_adds_source_queue_delay(tmp_path, monkeypatch):
    row = _run(tmp_path, monkeypatch, LOG)
    assert row["mean_latency_network"] == "45.0"   # (40*300 + 60*100) / 400
    assert row["mean_latency_open"] == "60.0"      # + (10*300 + 30*100) / 400
    # No perf.json here, so accepted falls back to the sum of the printed BW
    # fields, each over its own monitor span. The per-node column divides by
    # the 16 nodes mesh_4x4 names: 128 bits / 8 / 16.
    assert row["window_source"] == "monitor"
    assert row["accepted_bits_per_cycle"] == "128.0"
    assert row["accepted_bytes_per_node_cycle"] == "1.0"
    # offered on the DAT plane: p = 0.5 AX per cycle on each of AW and AR. BURST_LEN 32 is
    # AxLEN 32, 33 beats (gen_test_patterns.py emits axi_len + 1 W beats). A write is
    # 1 AW header + 33 W = 34 DAT flits, a read is 33 R = 33 DAT flits (AR rides REQ).
    # flits: 0.5 * 34 + 0.5 * 33 = 33.5. bytes (payload beats only): 0.5 * 33 * 64 * 2 = 2112.
    assert row["offered_flits_per_node_cycle"] == "33.5"
    assert row["offered_bytes_per_node_cycle"] == "2112.0"
    assert row["stim_size"] == "6"
    assert "mean_latency" not in row


def test_round_perf_fields_are_preserved():
    assert e.parse_round_perf(ROUND_LOG) == {
        "round_completion_cycles": "200",
        "round_active_sources": "15",
        "round_write_bursts": "15",
    }


@pytest.mark.parametrize("log_text", [
    ROUND_LOG.replace("round_cycles=200", "round_cycles=199"),
    ROUND_LOG.replace("[RoundPerf]", "[Other]"),
    ROUND_LOG + ROUND_LOG.splitlines()[1] + "\n",
    ROUND_LOG + "[RoundPerf] malformed\n",
    ROUND_LOG.replace("active_sources=15", "active_sources=0"),
    ROUND_LOG.replace("write_bursts=15", "write_bursts=0"),
])
def test_round_perf_rejects_invalid_evidence(log_text):
    with pytest.raises(SystemExit):
        e.parse_round_perf(log_text)


def test_traffic_meta_is_strict_and_matches_round_perf():
    assert e.parse_traffic_meta(ROUND_LOG) == {
        "round_active_sources": "15",
        "round_write_bursts": "15",
    }
    invalid = [
        ROUND_LOG.replace("[TrafficMeta]", "[Other]"),
        ROUND_LOG + ROUND_LOG.splitlines()[0] + "\n",
        ROUND_LOG + "[TrafficMeta] malformed\n",
        ROUND_LOG.replace("active_sources=15", "active_sources=0", 1),
        ROUND_LOG.replace("write_bursts=15", "write_bursts=0", 1),
    ]
    for log_text in invalid:
        with pytest.raises(SystemExit):
            e.parse_traffic_meta(log_text)


def test_round_perf_rejects_traffic_meta_mismatch():
    with pytest.raises(SystemExit):
        e.parse_round_perf(ROUND_LOG.replace(
            "[TrafficMeta] active_sources=15", "[TrafficMeta] active_sources=14"))


def test_latency_columns_split_read_from_write(tmp_path, monkeypatch):
    """Read and write are separate measurements and get separate columns.

    The monitor timestamps a read at its first R beat and a write at B, and the
    two return on different planes, so the combined mean compares against
    neither analytic. Here the read leg is 40 network and 50 open, the write leg
    60 and 90, and the combined columns stay the sample weighted mean of both."""
    row = _run(tmp_path, monkeypatch, LOG)
    assert row["mean_latency_network_read"] == "40.0"
    assert row["mean_latency_network_write"] == "60.0"
    assert row["mean_latency_open_read"] == "50.0"     # + 10.0 source queue
    assert row["mean_latency_open_write"] == "90.0"    # + 30.0 source queue
    assert row["mean_latency_network"] == "45.0" and row["mean_latency_open"] == "60.0"


def test_a_channel_with_no_sample_leaves_its_cells_empty(tmp_path, monkeypatch):
    """A run whose writes never retired must not report a write latency of zero.

    Zero is a measurement, empty is the absence of one, and section 2 of the
    report prints the second as `-`."""
    read_only = "\n".join(l for l in LOG.splitlines() if "[Write]" not in l) + "\n"
    row = _run(tmp_path, monkeypatch, read_only)
    assert row["mean_latency_network_read"] == "40.0"
    assert row["mean_latency_network_write"] == ""
    assert row["mean_latency_open_write"] == ""
    assert row["mean_latency_network"] == "40.0"


def test_accepted_uses_the_common_window_not_each_monitor_span(tmp_path, monkeypatch):
    """Two nodes retire the same work, one over half the run, and the row must
    describe the run rather than the sum of two private rates.

    node0 finishes in 1000 cycles and prints twice the BW of node1, which takes
    the full 2000. Summing the printed fields gives 3 * 64 = 192 bits per cycle,
    a rate the mesh never carried. Counted as delivered work over the common
    window: 4 monitor lines of 100 transactions, 33 beats of 64 B each, is
    400 * 33 * 64 * 8 / 2000 = 3379.2 bits per cycle.
    """
    log = LOG_HEAD + "".join(
        f"[Monitor node{n}.master][{d}] Latency: 50.00 +- 1.00, N: 100, "
        f"BW: {bw:.2f} Bits/cycle, Util: 10.00%\n"
        for n, bw in ((0, 128.0), (1, 64.0)) for d in ("Read", "Write"))
    (tmp_path / "perf.json").write_text(
        '{"window":{"start_cyc":0,"end_cyc":2000}}')
    row = _run(tmp_path, monkeypatch, log)
    assert row["window_source"] == "run"
    assert row["accepted_bits_per_cycle"] == "3379.2"
    # Per node over the 16 nodes mesh_4x4 names, the 14 silent ones included.
    assert row["accepted_bytes_per_node_cycle"] == str(round(3379.2 / 8 / 16, 6))


def test_self_traffic_nodes_report_zero_and_add_nothing(tmp_path, monkeypatch):
    """A node whose destination is itself never reaches a monitor.

    Measured, `output/continuous_mesh_4x4_transpose_r0.0179_s1/run.log`: transpose
    fixes the four diagonal nodes 0, 5, 10 and 15, and all four print `N: 0, BW:
    0.00`. So `accepted_bits_per_cycle` is a sum over the active nodes only, and
    its per node twin still divides by the full 16, which makes the per node
    column the fabric share diluted by `served`. Per active node is
    `accepted / served`, which is what `perf_report` compares against `ideal`.
    """
    log = LOG + "".join(
        f"[Monitor node{n}.master][{d}] Latency: 0.00 +- 0.00, N: 0, "
        f"BW: 0.00 Bits/cycle, Util: 0.00%\n"
        for n in (5, 10, 15) for d in ("Read", "Write"))
    row = _run(tmp_path, monkeypatch, log, **{"--pattern": "transpose"})
    # Unchanged from the single active node case above: three silent nodes add
    # no bandwidth, and they do not shrink the divisor either.
    assert row["accepted_bits_per_cycle"] == "128.0"
    assert row["accepted_bytes_per_node_cycle"] == "1.0"
    assert row["mean_latency_network"] == "45.0"


def test_open_equals_network_when_no_source_queue_line(tmp_path, monkeypatch):
    """A log from before the open-loop tb has no [SrcQueue] line: open == network,
    rather than a crash or a silently dropped column."""
    trimmed = "\n".join(l for l in LOG.splitlines() if "[SrcQueue" not in l)
    row = _run(tmp_path, monkeypatch, trimmed)
    assert row["mean_latency_open"] == row["mean_latency_network"] == "45.0"


def test_single_beat_offered_load(tmp_path, monkeypatch):
    """BURST_LEN 0 is AxLEN 0, one beat: a write is 1 AW + 1 W = 2 flits, a read 1 R."""
    row = _run(tmp_path, monkeypatch, LOG, **{"--burst-len": "0", "--injection-rate": "0.005"})
    assert row["offered_flits_per_node_cycle"] == "0.015"      # 0.005 * (2 + 1)
    assert row["offered_bytes_per_node_cycle"] == "0.64"       # 0.005 * 1 * 64 * 2


def test_many_to_many_offered_load_counts_write_data_only(tmp_path, monkeypatch):
    write_only = "\n".join(l for l in LOG.splitlines() if "[Read]" not in l) + "\n"
    row = _run(tmp_path, monkeypatch, write_only,
               **{"--pattern": "many_to_many", "--burst-len": "63",
                  "--injection-rate": "0.01"})
    assert row["offered_flits_per_node_cycle"] == "0.65"  # 0.01 * (1 header + 64 W)
    assert row["offered_bytes_per_node_cycle"] == "40.96"  # 0.01 * 64 beats * 64 B
    assert row["mean_latency_network_read"] == ""


CHANNEL_COMPARE_LOG = """[TrafficMeta] active_sources=3 write_bursts=192
PASS: all 16 nodes done, non-vacuous
[Monitor node0.master][Write] Latency: 289.50 +- 4.00, N: 64, BW: 64.00 Bits/cycle, Util: 10.00%
[ChannelCompare] case=write node=1 bursts=64 data_beats=16384
[ChannelCompare] case=write node=2 bursts=64 data_beats=16384
"""
READ_CHANNEL_COMPARE_LOG = """[TrafficMeta] active_sources=3 write_bursts=192
PASS: all 16 nodes done, non-vacuous
[Monitor node0.master][Read] Latency: 35.25 +- 2.00, N: 64, BW: 64.00 Bits/cycle, Util: 10.00%
[ChannelCompare] case=read node=1 bursts=64 data_beats=16384
[ChannelCompare] case=read node=2 bursts=64 data_beats=16384
"""


def _run_channel_compare(tmp_path, monkeypatch, log_text,
                         channel_mapping="2-channel", channel_case="write"):
    log = tmp_path / "run.log"
    log.write_text(log_text)
    out = tmp_path / "result.csv"
    monkeypatch.setattr(sys, "argv", [
        "emit_result_csv", "--log", str(log), "--out", str(out),
        "--topology", "mesh_4x4", "--channel-mapping", channel_mapping,
        "--channel-case", channel_case, "--seed", "1",
    ])
    e.main()
    return next(csv.DictReader(out.open()))


def test_channel_compare_writes_control_mean_and_interference_counts(tmp_path, monkeypatch):
    row = _run_channel_compare(tmp_path, monkeypatch, CHANNEL_COMPARE_LOG)
    assert row["channel_mapping"] == "2-channel"
    assert row["channel_case"] == "write"
    assert row["seed"] == "1"
    assert row["control_samples"] == "64"
    assert row["control_mean_latency_cycles"] == "289.5"
    assert row["data_bursts_per_node"] == "64"
    assert row["data_beats_per_node"] == "16384"


def test_channel_compare_reads_control_mean_and_interference_counts(tmp_path, monkeypatch):
    row = _run_channel_compare(tmp_path, monkeypatch, READ_CHANNEL_COMPARE_LOG,
                               channel_mapping="3-channel", channel_case="read")
    assert row["channel_mapping"] == "3-channel"
    assert row["channel_case"] == "read"
    assert row["control_samples"] == "64"
    assert row["control_mean_latency_cycles"] == "35.25"
    assert row["data_bursts_per_node"] == "64"
    assert row["data_beats_per_node"] == "16384"


def test_channel_compare_rejects_invalid_marker_evidence(tmp_path, monkeypatch):
    invalid_logs = [
        CHANNEL_COMPARE_LOG.replace("[Monitor node0.master]", "[Monitor node3.master]"),
        CHANNEL_COMPARE_LOG.replace("PASS: all 16 nodes done, non-vacuous", "PASS: all"),
        CHANNEL_COMPARE_LOG + "channel_compare_fault=1\n",
        CHANNEL_COMPARE_LOG.replace("N: 64", "N: 63"),
        CHANNEL_COMPARE_LOG.replace("node=2 bursts=64", "node=1 bursts=64"),
        CHANNEL_COMPARE_LOG.replace("bursts=64", "bursts=63", 1),
        CHANNEL_COMPARE_LOG.replace("data_beats=16384", "data_beats=16383", 1),
    ]
    for log in invalid_logs:
        with pytest.raises(SystemExit):
            _run_channel_compare(tmp_path, monkeypatch, log)
