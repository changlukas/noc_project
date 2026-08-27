import csv
import sys

import emit_result_csv as e

# Read and Write carry different sample counts on purpose: an implementation
# that averaged the printed means instead of weighting them by N would read
# 50.0 network and 70.0 open here, not 45.0 and 60.0.
LOG = """[Config] max_unique_ids=1 max_outstanding=32 dat_num_vc=2 router_vc_depth=8 mst_stall_random=0 ni_dat_rx_vc_depth=8
[Monitor node0.master][Read] Latency: 40.00 +- 1.00, N: 300, BW: 64.00 Bits/cycle, Util: 10.00%
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
            "--burst-len": "32"}
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
    # accepted is the sum over every node monitor, so the per-node column
    # divides by the 16 nodes mesh_4x4 names: 128 bits / 8 / 16.
    assert row["accepted_bits_per_cycle"] == "128.0"
    assert row["accepted_bytes_per_node_cycle"] == "1.0"
    # offered on the DAT plane: p = 0.5 AX per cycle on each of AW and AR. BURST_LEN 32 is
    # AxLEN 32, 33 beats (gen_test_patterns.py emits axi_len + 1 W beats). A write is
    # 1 AW header + 33 W = 34 DAT flits, a read is 33 R = 33 DAT flits (AR rides REQ).
    # flits: 0.5 * 34 + 0.5 * 33 = 33.5. bytes (payload beats only): 0.5 * 33 * 64 * 2 = 2112.
    assert row["offered_flits_per_node_cycle"] == "33.5"
    assert row["offered_bytes_per_node_cycle"] == "2112.0"
    assert "mean_latency" not in row


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
