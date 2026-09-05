import csv
import json
import re
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


def test_router_dat_stats_roll_up_per_vc_diagnostics(tmp_path):
    (tmp_path / "perf.json").write_text(json.dumps({
        "noc": {
            "router_dat_input_vcs": [
                {"router": "router_0", "port": "EAST", "vc": 0,
                 "hwm_flits": 5, "capacity_flits": 8},
                {"router": "router_1", "port": "WEST", "vc": 1,
                 "hwm_flits": 7, "capacity_flits": 8},
            ],
            "router_dat_output_vcs": [
                {"router": "router_0", "port": "EAST", "vc": 0,
                 "credit_block_cycles": 3},
                {"router": "router_1", "port": "WEST", "vc": 1,
                 "credit_block_cycles": 4},
            ],
        },
    }))

    assert e.router_dat_stats(tmp_path / "run.log") == {
        "router_dat_input_vc_hwm_max_flits": "7",
        "router_dat_input_vc_capacity_flits": "8",
        "router_dat_output_vc_credit_block_cycles_sum": "7",
    }


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
    meta = tmp_path / "traffic_meta.json"
    meta.write_text(json.dumps({
        "active_sources": 16,
        "source_write_bursts": 3200,
        "destination_deliveries": 3200,
    }))
    row = _run(tmp_path, monkeypatch, write_only,
               **{"--pattern": "many_to_many", "--burst-len": "63",
                  "--injection-rate": "0.01", "--traffic-meta": str(meta),
                  "--traffic-mapping": "many_to_many"})
    assert row["offered_flits_per_node_cycle"] == "0.65"  # 0.01 * (1 header + 64 W)
    assert row["offered_bytes_per_node_cycle"] == "40.96"  # 0.01 * 64 beats * 64 B
    assert row["mean_latency_network_read"] == ""


def test_windowed_mode_records_depth_and_has_no_analytic_offered_load(tmp_path, monkeypatch):
    write_only = LOG.replace(
        "[TrafficMeta] active_sources=16 write_bursts=3200",
        "[TrafficMeta] direction=write axi_initiators=16 source_requests=3200")
    write_only = "\n".join(l for l in write_only.splitlines() if "[Read]" not in l) + "\n"
    write_only += "[SourceWindow node0] depth=4 max_outstanding=4\n"
    write_only += "[HWM] node=0 read_slot_hwm=128 order_list_hwm=9 write_txns_hwm=7 read_txns_hwm=8 aw_clause={idle=1 same_dest=2 alloc=3} ar_clause={idle=4 same_dest=5 alloc=6}\n"
    write_only += "[HWM] node=1 read_slot_hwm=64 order_list_hwm=4 write_txns_hwm=3 read_txns_hwm=2 aw_clause={idle=1 same_dest=2 alloc=3} ar_clause={idle=4 same_dest=5 alloc=7}\n"
    write_only += "[NSU_HWM] node=0 write_meta_hwm=11 read_meta_hwm=5\n"
    write_only += "[NSU_HWM] node=1 write_meta_hwm=6 read_meta_hwm=12\n"
    write_only += "[MeasurementWindow] start_cycle=100 completion_cycle=1100\n"
    write_only += "PASS: all 16 nodes done, non-vacuous\n"
    meta = tmp_path / "traffic_meta.json"
    meta.write_text(json.dumps({
        "direction": "write", "rounds": 16,
        "data_producers": 16, "consumers": 16, "axi_initiators": 16,
        "source_requests": 3200, "payload_deliveries": 3200,
    }))
    (tmp_path / "perf.json").write_text(json.dumps({
        "window": {"start_cyc": 100, "end_cyc": 1100},
        "noc": {
            "links": [{"name": "dat_0to1", "flit_count": 800}],
            "router_dat_input_vcs": [
                {"router": "router_0", "port": "EAST", "vc": 0,
                 "hwm_flits": 7, "capacity_flits": 8},
            ],
            "router_dat_output_vcs": [
                {"router": "router_0", "port": "WEST", "vc": 1,
                 "credit_block_cycles": 9},
            ],
        },
    }))

    row = _run(tmp_path, monkeypatch, write_only, **{
        "--pattern": "many_to_many",
        "--injection-mode": "4",
        "--injection-rate": "N/A",
        "--source-outstanding-depth": "4",
        "--traffic-direction": "write",
        "--r-rob-depth": "64",
        "--max-txns-per-id": "8",
        "--traffic-meta": str(meta),
        "--traffic-mapping": "many_to_many",
    })

    assert row["source_outstanding_depth"] == "4"
    assert row["source_outstanding_hwm"] == "4"
    assert row["direction"] == "write"
    assert row["axi_initiators"] == "16"
    assert row["source_requests"] == "3200"
    assert row["payload_deliveries"] == "3200"
    assert row["rounds"] == "16"
    assert row["transaction_bytes"] == "2112"
    assert row["checker_status"] == "PASS"
    assert row["r_rob_depth"] == "64"
    assert row["max_txns_per_id"] == "8"
    assert row["nmu_read_slot_hwm_beats"] == "128"
    assert row["nmu_order_list_hwm_transactions"] == "9"
    assert row["nmu_ar_fallback_allocations"] == "13"
    assert row["nsu_write_meta_hwm_transactions"] == "11"
    assert row["nsu_read_meta_hwm_transactions"] == "12"
    assert row["router_dat_input_vc_hwm_max_flits"] == "7"
    assert row["router_dat_input_vc_capacity_flits"] == "8"
    assert row["router_dat_output_vc_credit_block_cycles_sum"] == "9"
    assert "injection_rate" not in row
    assert not any(name.startswith("offered_") for name in row)


def test_windowed_mode_records_shipped_max_txns_per_id_by_default(tmp_path, monkeypatch):
    write_only = LOG.replace(
        "[TrafficMeta] active_sources=16 write_bursts=3200",
        "[TrafficMeta] direction=write axi_initiators=16 source_requests=3200")
    write_only = "\n".join(l for l in write_only.splitlines() if "[Read]" not in l) + "\n"
    write_only += "[SourceWindow node0] depth=4 max_outstanding=4\n"
    write_only += "[MeasurementWindow] start_cycle=100 completion_cycle=1100\n"
    write_only += "PASS: all 16 nodes done, non-vacuous\n"
    meta = tmp_path / "traffic_meta.json"
    meta.write_text(json.dumps({
        "direction": "write", "rounds": 16,
        "data_producers": 16, "consumers": 16, "axi_initiators": 16,
        "source_requests": 3200, "payload_deliveries": 3200,
    }))
    (tmp_path / "perf.json").write_text(json.dumps({
        "window": {"start_cyc": 100, "end_cyc": 1100},
        "noc": {
            "links": [{"name": "dat_0to1", "flit_count": 800}],
            "router_dat_input_vcs": [
                {"router": "router_0", "port": "LOCAL", "vc": 0,
                 "hwm_flits": 0, "capacity_flits": 8},
            ],
            "router_dat_output_vcs": [
                {"router": "router_0", "port": "LOCAL", "vc": 0,
                 "credit_block_cycles": 0},
            ],
        },
    }))

    row = _run(tmp_path, monkeypatch, write_only, **{
        "--pattern": "many_to_many",
        "--injection-mode": "4",
        "--injection-rate": "N/A",
        "--source-outstanding-depth": "4",
        "--traffic-direction": "write",
        "--traffic-meta": str(meta),
        "--traffic-mapping": "many_to_many",
    })

    assert row["max_txns_per_id"] == "32"


@pytest.mark.parametrize(("multicast_mode", "requests", "deliveries"), [
    ("hardware", 16, 256),
    ("repeated_unicast", 256, 256),
])
def test_windowed_broadcast_records_and_validates_multicast_mode(
        tmp_path, monkeypatch, multicast_mode, requests, deliveries):
    log = LOG.replace(
        "[TrafficMeta] active_sources=16 write_bursts=3200",
        f"[TrafficMeta] direction=write axi_initiators=1 source_requests={requests}")
    log = "\n".join(line for line in log.splitlines() if "[Read]" not in line) + "\n"
    log += "[SourceWindow node0] depth=32 max_outstanding=32\n"
    log += "[MeasurementWindow] start_cycle=100 completion_cycle=1100\n"
    log += "PASS: all 16 nodes done, non-vacuous\n"
    meta = tmp_path / f"{multicast_mode}.json"
    meta.write_text(json.dumps({
        "direction": "write", "rounds": 16, "transactions_per_flow": 1,
        "burst_beats": 64, "bytes_per_beat": 64,
        "bytes_per_flow_round": 4096, "data_producers": 1,
        "consumers": 16, "axi_initiators": 1, "source_requests": requests,
        "payload_deliveries": deliveries, "multicast_mode": multicast_mode,
        "destinations_per_source": 16,
    }))
    (tmp_path / "perf.json").write_text(json.dumps({
        "window": {"start_cyc": 100, "end_cyc": 1100},
        "noc": {
            "links": [{"name": "dat_0to1", "flit_count": 1}],
            "router_dat_input_vcs": [
                {"hwm_flits": 1, "capacity_flits": 8},
            ],
            "router_dat_output_vcs": [{"credit_block_cycles": 0}],
        },
    }))

    row = _run(tmp_path, monkeypatch, log, **{
        "--pattern": "broadcast", "--injection-mode": "4",
        "--source-outstanding-depth": "32", "--traffic-direction": "write",
        "--traffic-meta": str(meta), "--traffic-mapping": "broadcast_global",
        "--multicast-mode": multicast_mode, "--burst-len": "63", "--stim-size": "6",
    })
    assert row["multicast_mode"] == multicast_mode
    assert row["destinations_per_source"] == "16"
    assert row["transactions_per_flow"] == "1"
    assert row["bytes_per_flow_round"] == "4096"


@pytest.mark.parametrize("metadata_mode", [None, "software"])
def test_broadcast_requires_a_valid_recorded_multicast_mode(tmp_path, metadata_mode):
    payload = {
        "direction": "write", "rounds": 16, "transactions_per_flow": 1,
        "burst_beats": 64, "bytes_per_beat": 64,
        "bytes_per_flow_round": 4096, "data_producers": 1,
        "consumers": 16, "axi_initiators": 1, "source_requests": 16,
        "payload_deliveries": 256,
        "destinations_per_source": 16,
    }
    if metadata_mode is not None:
        payload["multicast_mode"] = metadata_mode
    meta = tmp_path / "traffic_meta.json"
    meta.write_text(json.dumps(payload))

    with pytest.raises(SystemExit, match="multicast mode"):
        e.load_traffic_meta(
            meta,
            "[TrafficMeta] direction=write axi_initiators=1 source_requests=16\n",
            "broadcast", "write", "hardware", 64, 64)


def test_windowed_result_rejects_burst_metadata_disagreement(tmp_path, monkeypatch):
    log = LOG.replace(
        "[TrafficMeta] active_sources=16 write_bursts=3200",
        "[TrafficMeta] direction=write axi_initiators=15 source_requests=960")
    log = "\n".join(line for line in log.splitlines() if "[Read]" not in line) + "\n"
    log += "[SourceWindow node0] depth=32 max_outstanding=32\n"
    log += "[MeasurementWindow] start_cycle=100 completion_cycle=1100\n"
    log += "PASS: all 16 nodes done, non-vacuous\n"
    meta = tmp_path / "traffic_meta.json"
    meta.write_text(json.dumps({
        "direction": "write", "rounds": 16, "transactions_per_flow": 4,
        "burst_beats": 15, "bytes_per_beat": 64,
        "bytes_per_flow_round": 3840, "data_producers": 15,
        "consumers": 15, "axi_initiators": 15, "source_requests": 960,
        "payload_deliveries": 960,
    }))
    (tmp_path / "perf.json").write_text(json.dumps({
        "window": {"start_cyc": 100, "end_cyc": 1100},
        "noc": {
            "links": [{"name": "dat_0to1", "flit_count": 1}],
            "router_dat_input_vcs": [{"hwm_flits": 1, "capacity_flits": 8}],
            "router_dat_output_vcs": [{"credit_block_cycles": 0}],
        },
    }))
    with pytest.raises(SystemExit, match="traffic metadata"):
        _run(tmp_path, monkeypatch, log, **{
            "--pattern": "pipeline", "--injection-mode": "4",
            "--source-outstanding-depth": "32", "--traffic-direction": "write",
            "--traffic-meta": str(meta), "--traffic-mapping": "pipeline",
            "--burst-len": "15", "--stim-size": "6",
        })


@pytest.mark.parametrize("perf_window", [
    {"start_cyc": 0, "end_cyc": 1000},
    {"start_cyc": 100, "end_cyc": 1200},
])
def test_windowed_mode_rejects_zero_or_settle_contaminated_counter_window(
        tmp_path, monkeypatch, perf_window):
    write_only = LOG.replace(
        "[TrafficMeta] active_sources=16 write_bursts=3200",
        "[TrafficMeta] direction=write axi_initiators=16 source_requests=3200")
    write_only = "\n".join(l for l in write_only.splitlines() if "[Read]" not in l) + "\n"
    write_only += "[SourceWindow node0] depth=4 max_outstanding=4\n"
    write_only += "[MeasurementWindow] start_cycle=100 completion_cycle=1100\n"
    write_only += "PASS: all 16 nodes done, non-vacuous\n"
    meta = tmp_path / "traffic_meta.json"
    meta.write_text(json.dumps({
        "direction": "write", "rounds": 16,
        "data_producers": 16, "consumers": 16, "axi_initiators": 16,
        "source_requests": 3200, "payload_deliveries": 3200,
    }))
    (tmp_path / "perf.json").write_text(json.dumps({
        "window": perf_window,
        "noc": {"links": [{"name": "dat_0to1", "flit_count": 800}]},
    }))

    with pytest.raises(SystemExit, match="performance window"):
        _run(tmp_path, monkeypatch, write_only, **{
            "--pattern": "many_to_many",
            "--injection-mode": "4",
            "--injection-rate": "N/A",
            "--source-outstanding-depth": "4",
            "--traffic-direction": "write",
            "--traffic-meta": str(meta),
            "--traffic-mapping": "many_to_many",
        })


@pytest.mark.parametrize("active_sources", [1, 4, 15, 16])
def test_ai_load_is_normalized_per_active_source_and_mesh(
        tmp_path, monkeypatch, active_sources):
    log_text = LOG_HEAD.replace(
        "active_sources=16 write_bursts=3200",
        f"active_sources={active_sources} write_bursts={active_sources * 10}")
    log_text += "[Monitor node0.master][Write] Latency: 30.00 +- 1.00, N: 10, BW: 64.00 Bits/cycle, Util: 10.00%\n"
    meta = tmp_path / f"meta_{active_sources}.json"
    meta.write_text(json.dumps({
        "active_sources": active_sources,
        "source_write_bursts": active_sources * 10,
        "destination_deliveries": active_sources * 10,
    }))
    (tmp_path / "perf.json").write_text(
        '{"window":{"start_cyc":0,"end_cyc":100}}')
    row = _run(tmp_path, monkeypatch, log_text, **{
        "--pattern": "pipeline", "--injection-rate": "0.1",
        "--burst-len": "0", "--traffic-meta": str(meta),
        "--traffic-mapping": "pipeline",
    })
    assert row["active_sources"] == str(active_sources)
    assert row["offered_load_per_active_source"] == "0.2"
    assert row["offered_load_mesh_avg"] == str(round(0.2 * active_sources / 16, 6))
    assert row["accepted_injection_load_mesh_avg"] == str(
        round(active_sources * 10 * 2 / 100 / 16, 6))
    assert row["delivered_payload_bytes_per_cycle"] == str(
        round(active_sources * 10 * 64 / 100, 6))
    assert row["destination_deliveries"] == str(active_sources * 10)


def test_ai_metadata_rejects_log_mismatch_and_bad_broadcast_fanout(tmp_path, monkeypatch):
    (tmp_path / "perf.json").write_text(
        '{"window":{"start_cyc":0,"end_cyc":100}}')
    invalid = [
        {"active_sources": 15, "source_write_bursts": 3200,
         "destination_deliveries": 3200},
        {"active_sources": 16, "source_write_bursts": 3199,
         "destination_deliveries": 3199},
        {"active_sources": 16, "source_write_bursts": 3200,
         "destination_deliveries": 0},
    ]
    for index, payload in enumerate(invalid):
        meta = tmp_path / f"invalid_{index}.json"
        meta.write_text(json.dumps(payload))
        with pytest.raises(SystemExit):
            _run(tmp_path, monkeypatch, LOG, **{
                "--pattern": "broadcast", "--traffic-meta": str(meta),
                "--traffic-mapping": "broadcast_global",
            })


_WRITE_BACKGROUND_NODES = (1, 2, 3, 7, 6, 5, 4, 8, 9, 10, 11, 15, 14, 13)
_READ_BACKGROUND_NODES = (2, 3, 7, 6, 5, 4, 8, 9, 10, 11, 15, 14, 13, 12)

CHANNEL_COMPARE_LOG = """[TrafficMeta] active_sources=15 write_bursts=512
PASS: all 16 nodes done, non-vacuous
[Monitor node0.master][Write] Latency: 289.50 +- 4.00, N: 64, BW: 64.00 Bits/cycle, Util: 10.00%
[SrcQueue node0][Write] mean: 0.00, N: 64
[ChannelCompareInterval] role=control node=0 start_cycle=100 completion_cycle=400
[ChannelCompareOverlap] control_node=0 background_nodes=14 status=PASS
[ChannelCompareBarrier] release_cycle=915
""" + "".join(
    f"[ChannelCompare] case=write node={node} bursts=32 data_beats=8192\n"
    f"[ChannelCompareInterval] role=background node={node} "
    f"start_cycle=100 completion_cycle={900 + node}\n"
    for node in _WRITE_BACKGROUND_NODES)
READ_CHANNEL_COMPARE_LOG = """[TrafficMeta] active_sources=15 write_bursts=512
PASS: all 16 nodes done, non-vacuous
[Monitor node0.master][Read] Latency: 35.25 +- 2.00, N: 64, BW: 64.00 Bits/cycle, Util: 10.00%
[SrcQueue node0][Read] mean: 0.00, N: 64
[ChannelCompareInterval] role=control node=0 start_cycle=100 completion_cycle=400
[ChannelCompareOverlap] control_node=0 background_nodes=14 status=PASS
[ChannelCompareBarrier] release_cycle=915
""" + "".join(
    f"[ChannelCompare] case=read node={node} bursts=32 data_beats=8192\n"
    f"[ChannelCompareInterval] role=background node={node} "
    f"start_cycle=100 completion_cycle={900 + node}\n"
    for node in _READ_BACKGROUND_NODES)


def _run_channel_compare(tmp_path, monkeypatch, log_text,
                         channel_mapping="2-channel", channel_case="write",
                         window_end=None):
    log = tmp_path / "run.log"
    log.write_text(log_text)
    out = tmp_path / "result.csv"
    meta = tmp_path / "traffic_meta.json"
    background_resource = ("dat_1to2" if channel_mapping == "3-channel" else
                           "req_1to2")
    meta.write_text(json.dumps({
        "direction": channel_case, "rounds": 16, "transactions_per_flow": 2,
        "control_probes": 64, "background_bursts_per_flow": 32,
        "background_beats_per_flow": 8192, "background_nodes": 14,
        "shared_directed_edge": "1to2", "control_resource": "req_1to2",
        "rr_background_resource": "req_1to2",
        "rrd_background_resource": "dat_1to2",
    }))
    control_flits = 128 if channel_case == "write" else 64
    background_flits = 32 * (257 if channel_case == "write" else 1)
    links = {"req_1to2": control_flits}
    links[background_resource] = links.get(background_resource, 0) + background_flits
    background_nodes = (_WRITE_BACKGROUND_NODES if channel_case == "write" else
                        _READ_BACKGROUND_NODES)
    if window_end is None:
        window_end = max(900 + node for node in background_nodes)
    (tmp_path / "perf.json").write_text(json.dumps({
        "window": {"start_cyc": 100, "end_cyc": window_end},
        "noc": {"links": [{"name": name, "flit_count": count}
                           for name, count in links.items()]},
    }))
    monkeypatch.setattr(sys, "argv", [
        "emit_result_csv", "--log", str(log), "--out", str(out),
        "--topology", "mesh_4x4", "--channel-mapping", channel_mapping,
        "--channel-case", channel_case, "--seed", "1", "--traffic-meta", str(meta),
        "--source-outstanding-depth", "32", "--max-txns-per-id", "32",
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
    assert row["data_bursts_per_flow"] == "32"
    assert row["data_beats_per_flow"] == "8192"
    assert row["shared_directed_edge"] == "1to2"
    assert row["overlap_status"] == "PASS"


def test_channel_compare_reads_control_mean_and_interference_counts(tmp_path, monkeypatch):
    row = _run_channel_compare(tmp_path, monkeypatch, READ_CHANNEL_COMPARE_LOG,
                               channel_mapping="3-channel", channel_case="read")
    assert row["channel_mapping"] == "3-channel"
    assert row["channel_case"] == "read"
    assert row["control_samples"] == "64"
    assert row["control_mean_latency_cycles"] == "35.25"
    assert row["data_bursts_per_flow"] == "32"
    assert row["data_beats_per_flow"] == "8192"
    assert row["control_resource"] == "req_1to2"
    assert row["background_resource"] == "dat_1to2"


def test_channel_compare_completion_time_includes_ready_low_before_handshake(
        tmp_path, monkeypatch):
    log = CHANNEL_COMPARE_LOG.replace(
        "[SrcQueue node0][Write] mean: 0.00, N: 64",
        "[SrcQueue node0][Write] mean: 7.25, N: 64",
    )

    row = _run_channel_compare(tmp_path, monkeypatch, log)

    assert row["control_mean_latency_cycles"] == "296.75"


def test_channel_compare_rejects_invalid_marker_evidence(tmp_path, monkeypatch):
    invalid_logs = [
        CHANNEL_COMPARE_LOG.replace("[Monitor node0.master]", "[Monitor node3.master]"),
        CHANNEL_COMPARE_LOG.replace("PASS: all 16 nodes done, non-vacuous", "PASS: all"),
        CHANNEL_COMPARE_LOG + "channel_compare_fault=1\n",
        CHANNEL_COMPARE_LOG.replace("N: 64", "N: 63"),
        CHANNEL_COMPARE_LOG.replace("node=2 bursts=32", "node=1 bursts=32", 1),
        CHANNEL_COMPARE_LOG.replace("bursts=32", "bursts=31", 1),
        CHANNEL_COMPARE_LOG.replace("data_beats=8192", "data_beats=8191", 1),
        CHANNEL_COMPARE_LOG.replace("completion_cycle=901", "completion_cycle=300", 1),
        CHANNEL_COMPARE_LOG.replace("status=PASS", "status=FAIL"),
    ]
    for log in invalid_logs:
        with pytest.raises(SystemExit):
            _run_channel_compare(tmp_path, monkeypatch, log)


def test_channel_compare_rejects_completion_cycles_copied_from_barrier(
        tmp_path, monkeypatch):
    fake = re.sub(r"completion_cycle=\d+", "completion_cycle=777",
                  CHANNEL_COMPARE_LOG)
    fake = fake.replace("release_cycle=915", "release_cycle=777")
    with pytest.raises(SystemExit, match="barrier"):
        _run_channel_compare(tmp_path, monkeypatch, fake, window_end=777)
