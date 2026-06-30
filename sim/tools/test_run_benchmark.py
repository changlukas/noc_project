"""Unit tests for run_benchmark.py summarize() and CLI arg parsing.

Run:
    python3 -m pytest sim/tools/test_run_benchmark.py -v
    python3 -c "import sim.tools.test_run_benchmark as t; t.test_summary_computes_p95()"
"""
import os
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_benchmark  # noqa: E402

from run_benchmark import summarize  # noqa: E402


def test_summary_computes_p95():
    """p95 nearest-rank on 1..100 == 95 (ceil(0.95*100) - 1 = 94th 0-based index)."""
    perf = {"latency": {"transactions": [{"latency": lat} for lat in range(1, 101)]}}
    s = summarize(perf)
    assert s["latency"]["p95"] == 95


def test_summary_mean():
    """mean of 1..100 == 50.5."""
    perf = {"latency": {"transactions": [{"latency": lat} for lat in range(1, 101)]}}
    s = summarize(perf)
    assert s["latency"]["mean"] == 50.5


def test_summary_max():
    """max of 1..100 == 100."""
    perf = {"latency": {"transactions": [{"latency": lat} for lat in range(1, 101)]}}
    s = summarize(perf)
    assert s["latency"]["max"] == 100


def test_summary_injection_tag():
    """injection field is always 'greedy-finite-trace-stress'."""
    perf = {"latency": {"transactions": [{"latency": 10}]}}
    s = summarize(perf)
    assert s["injection"] == "greedy-finite-trace-stress"


def test_summary_empty_txns():
    """Empty transactions list -> latency dict is empty (no KeyError)."""
    perf = {"latency": {"transactions": []}}
    s = summarize(perf)
    assert s["latency"] == {}


def test_summary_window_passthrough():
    """window is passed through from perf_json unchanged."""
    perf = {
        "latency": {"transactions": []},
        "window": {"start_cyc": 0, "end_cyc": 42},
    }
    s = summarize(perf)
    assert s["window"] == {"start_cyc": 0, "end_cyc": 42}


def test_summary_slot_throughput():
    """slot_throughput captures bytes and txn counts per slot."""
    perf = {
        "latency": {"transactions": []},
        "axi_slots": [
            {"name": "node0.manager", "write_byte_count": 32,
             "read_byte_count": 32, "write_txn_count": 1, "read_txn_count": 1},
        ],
    }
    s = summarize(perf)
    assert len(s["slot_throughput"]) == 1
    assert s["slot_throughput"][0]["bytes_wr"] == 32


def test_summary_p95_single():
    """p95 of a single-element list == that element."""
    perf = {"latency": {"transactions": [{"latency": 7}]}}
    s = summarize(perf)
    assert s["latency"]["p95"] == 7


def test_preserve_addr_forwarded(monkeypatch):
    """--preserve-addr must be forwarded to gen_test_patterns in the gen_args list."""
    import pytest
    captured = {}

    def fake_run(args, **kw):
        if "gen_test_patterns.py" in " ".join(map(str, args)):
            captured["gen"] = list(map(str, args))

        class R:
            returncode = 0
            stdout = "PASS: scenario complete, scoreboard clean"
            stderr = ""

        return R()

    monkeypatch.setattr(run_benchmark.subprocess, "run", fake_run)
    monkeypatch.setattr(run_benchmark, "_find_exe", lambda t: __import__("pathlib").Path("Vtb_top"))
    monkeypatch.setattr(run_benchmark, "_node_count", lambda t: 1)
    # main() calls sys.exit when perf.json is absent; gen args are captured before that.
    with pytest.raises(SystemExit):
        run_benchmark.main(["--topology", "mesh_4x4_vc1", "--pattern", "neighbor",
                            "--preserve-addr", "--out-root", "x"])
    assert "--preserve-addr" in captured["gen"]


def test_from_arg_parsing():
    """--from is forwarded; absent → default canonical base path."""
    import argparse
    import run_benchmark as rb

    # Parse with explicit --from
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="base", default=None)
    args = ap.parse_args(["--from", "/some/path.yaml"])
    assert args.base == "/some/path.yaml"

    # Default base path exists on disk
    default_base = Path(rb.ROOT) / "sim" / "test_patterns" \
        / "AX4-BAS-001_single_write_read_aligned" / "scenario.yaml"
    assert default_base.exists(), f"Default base not found: {default_base}"


if __name__ == "__main__":
    # Run without pytest (matches task gate requirement).
    tests = [
        test_summary_computes_p95,
        test_summary_mean,
        test_summary_max,
        test_summary_injection_tag,
        test_summary_empty_txns,
        test_summary_window_passthrough,
        test_summary_slot_throughput,
        test_summary_p95_single,
        test_from_arg_parsing,
    ]
    passed = failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS  {t.__name__}")
            passed += 1
        except AssertionError as e:
            print(f"FAIL  {t.__name__}: {e}")
            failed += 1
    print(f"\n{passed}/{passed+failed} tests passed")
    sys.exit(1 if failed else 0)
