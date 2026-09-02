import csv
import json
import re

import pytest

import perf_report as pr


MAPPINGS = {
    "broadcast_row": ("broadcast", 4, 4, 16),
    "broadcast_col": ("broadcast", 4, 4, 16),
    "broadcast_submesh": ("broadcast", 4, 4, 16),
    "broadcast_global": ("broadcast", 1, 1, 16),
    "gather_global_root0": ("gather", 15, 15, 15),
    "gather_submesh": ("gather", 12, 12, 12),
    "alltoall": ("alltoall", 16, 240, 240),
    "neighbor_exchange": ("neighbor_exchange", 16, 48, 48),
    "pipeline": ("pipeline", 15, 15, 15),
    "many_to_many": ("many_to_many", 16, 64, 64),
}


def _write_result(root, mapping, offered, seed="1", stim_size="6",
                  burst_len="63", directed=False):
    pattern, active, writes, deliveries = MAPPINGS[mapping]
    prefix = "directed" if directed else "continuous"
    run = root / f"{prefix}_mesh_4x4_{mapping}_{offered}_{seed}"
    run.mkdir(parents=True)
    row = {
        "topology": "mesh_4x4", "vc": "2", "router_vc_depth": "8",
        "ni_dat_rx_vc_depth": "8", "pattern": pattern,
        "traffic_mapping": mapping, "active_sources": str(active),
        "injection_mode": "0" if directed else "1", "injection_rate": "0.001",
        "injection_count": "16", "seed": seed, "max_unique_ids": "8",
        "max_outstanding": "32", "max_txns_per_id": "32",
        "ids_per_initiator": "1", "burst_len": burst_len,
        "stim_size": stim_size, "space": "memory", "mst_stall_random": "0",
        "offered_load_per_active_source": str(offered),
        "offered_load_mesh_avg": str(offered * active / 16),
        "accepted_injection_load_mesh_avg": str(offered * active / 20),
        "delivered_payload_bytes_per_cycle": str(1000 * offered * deliveries),
        "destination_deliveries": str(deliveries),
        "mean_latency_open_write": str(40 + 100 * offered),
        "round_completion_cycles": "250" if directed else "",
        "round_active_sources": str(active) if directed else "",
        "round_write_bursts": str(writes) if directed else "",
    }
    with (run / "result.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(row))
        writer.writeheader()
        writer.writerow(row)
    perf = {
        "window": {"start_cyc": 0, "end_cyc": 1000},
        "noc": {"links": [
            {"name": f"dat_{index}", "flit_count": 100 + 10 * index}
            for index in range(48)
        ]},
    }
    (run / "perf.json").write_text(json.dumps(perf))


def _complete_results(root):
    for mapping in MAPPINGS:
        for offered in (0.05, 0.2, 0.5):
            _write_result(root, mapping, offered)
        _write_result(root, mapping, 0.0, directed=True)


def test_policy_tables_cover_only_ai_inference_traffic():
    assert pr.AI_WRITE_ONLY == frozenset({
        "broadcast", "gather", "alltoall", "neighbor_exchange", "pipeline",
        "many_to_many",
    })
    assert pr.DISPLAY_NAME["many_to_many"] == "Regional Exchange"
    assert pr.DISPLAY_NAME["broadcast"] == "Broadcast / Multicast"


@pytest.mark.parametrize(("field", "value"), [
    ("stim_size", "5"), ("burst_len", "31"), ("seed", "2"),
    ("offered_load_per_active_source", "0.3"),
])
def test_report_rejects_incomparable_rows(tmp_path, field, value):
    _complete_results(tmp_path)
    target = tmp_path / "continuous_mesh_4x4_pipeline_0.2_1/result.csv"
    rows = list(csv.DictReader(target.open()))
    rows[0][field] = value
    with target.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    with pytest.raises(SystemExit):
        pr.collect(tmp_path)


def test_ai_report_uses_common_metrics_and_plain_formulas(tmp_path):
    _complete_results(tmp_path)
    text = pr.report(tmp_path)
    assert re.findall(r"^## .+$", text, re.M) == [
        "## 1. Test Setup and Measurement",
        "## 2. AI Communication Types",
        "## 3. Continuous-load Results",
        "## 4. Synchronized Directed Round",
    ]
    for mapping in MAPPINGS:
        assert pr.mapping_label(mapping) in text
    assert "Low-load completion latency (cycles/transaction)" in text
    assert "Useful delivered bandwidth (B/cycle)" in text
    assert "DAT-link utilization (%)" in text
    assert "AXI write round completion time (cycles/round)" in text
    assert "common issue start through the final expected B response" in text
    assert "Offered load per active source = Injection rate × 65 DAT flits" in text
    assert "Offered load mesh average = Offered load per active source × active sources / 16" in text
    assert "Regional Exchange" in text
    assert "uniform_random" not in text and "Hotspot" not in text
    assert "RR vs RRD" not in text
    assert "$" not in text and "\\times" not in text
    assert len(text.splitlines()) <= 120


def test_broadcast_useful_bandwidth_applies_gemm_payload_ratio(tmp_path):
    _complete_results(tmp_path)
    results = pr.collect(tmp_path)
    broadcast = next(point for point in results["continuous"]
                     if point["mapping"] == "broadcast_global" and point["offered"] == 0.5)
    pipeline = next(point for point in results["continuous"]
                    if point["mapping"] == "pipeline" and point["offered"] == 0.5)
    assert broadcast["useful_bandwidth"] == broadcast["delivered_bandwidth"] * 0.5
    assert pipeline["useful_bandwidth"] == pipeline["delivered_bandwidth"]


def test_figures_are_english_and_show_all_mappings(tmp_path):
    _complete_results(tmp_path)
    results = pr.collect(tmp_path)
    paths = pr.write_figures(results, tmp_path)
    assert {path.name for path in paths} == {
        "perf_ai_latency.svg", "perf_ai_bandwidth.svg",
        "perf_ai_link_utilization.svg", "perf_ai_round_completion.svg",
    }
    for path in paths:
        svg = path.read_text(encoding="utf-8")
        assert re.search(r"[\u4e00-\u9fff]", svg) is None
        assert "cycles" in svg or "B/cycle" in svg or "%" in svg
    round_svg = (tmp_path / "perf_ai_round_completion.svg").read_text(
        encoding="utf-8")
    assert all(pr.mapping_label(mapping) in round_svg for mapping in MAPPINGS)
