"""Unit tests for check_perf_parity.py.

TDD: write these tests first (they fail because check_perf_parity.py does not
exist yet), implement the script, then verify they all pass.

Run:
    python3 -m pytest sim/tools/test_check_perf_parity.py -v
    python3 sim/tools/test_check_perf_parity.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKER = os.path.join(HERE, "check_perf_parity.py")


def _write(path: str, obj: dict) -> str:
    with open(path, "w") as f:
        json.dump(obj, f)
    return path


BASE = {
    "schema_version": 1,
    "scenario": "AX4-BAS-002",
    "window": {"start_cyc": 0, "end_cyc": 421},
    "axi_slots": [{"name": "node0.manager", "write_txn_count": 4}],
    "latency": {"measured_at": "manager slot", "transactions": []},
    "noc": {
        "routers": [{"name": "req.router_0", "in_fifo_occ_max": 2}],
        "links": [{"name": "req_0to1", "flit_count": 12, "stall_cyc": 0}],
    },
}


def _run(a: str, b: str) -> int:
    r = subprocess.run([sys.executable, CHECKER, a, b], capture_output=True)
    return r.returncode


def test_identical_exit_0():
    """Two identical JSONs → exit 0."""
    with tempfile.TemporaryDirectory() as td:
        a = _write(os.path.join(td, "a.json"), BASE)
        b = _write(os.path.join(td, "b.json"), dict(BASE))  # shallow copy sufficient for this
        assert _run(a, b) == 0


def test_links_only_diff_exit_0():
    """JSONs differing only in noc.links → exit 0 (links field is excluded)."""
    import copy
    with tempfile.TemporaryDirectory() as td:
        obj_a = copy.deepcopy(BASE)
        obj_b = copy.deepcopy(BASE)
        obj_b["noc"]["links"] = [{"name": "req_0to1", "flit_count": 99, "stall_cyc": 5}]
        a = _write(os.path.join(td, "a.json"), obj_a)
        b = _write(os.path.join(td, "b.json"), obj_b)
        assert _run(a, b) == 0


def test_window_diff_exit_1():
    """JSONs differing in window.end_cyc → exit 1."""
    import copy
    with tempfile.TemporaryDirectory() as td:
        obj_a = copy.deepcopy(BASE)
        obj_b = copy.deepcopy(BASE)
        obj_b["window"]["end_cyc"] = 999
        a = _write(os.path.join(td, "a.json"), obj_a)
        b = _write(os.path.join(td, "b.json"), obj_b)
        assert _run(a, b) == 1


def test_schema_version_diff_exit_1():
    """JSONs differing in schema_version → exit 1."""
    import copy
    with tempfile.TemporaryDirectory() as td:
        obj_a = copy.deepcopy(BASE)
        obj_b = copy.deepcopy(BASE)
        obj_b["schema_version"] = 2
        a = _write(os.path.join(td, "a.json"), obj_a)
        b = _write(os.path.join(td, "b.json"), obj_b)
        assert _run(a, b) == 1


def test_no_noc_key_exit_0():
    """JSONs without noc key at all, otherwise equal → exit 0."""
    import copy
    with tempfile.TemporaryDirectory() as td:
        obj_a = copy.deepcopy(BASE)
        obj_b = copy.deepcopy(BASE)
        del obj_a["noc"]
        del obj_b["noc"]
        a = _write(os.path.join(td, "a.json"), obj_a)
        b = _write(os.path.join(td, "b.json"), obj_b)
        assert _run(a, b) == 0


def test_routers_diff_exit_1():
    """JSONs differing in noc.routers (not links) → exit 1."""
    import copy
    with tempfile.TemporaryDirectory() as td:
        obj_a = copy.deepcopy(BASE)
        obj_b = copy.deepcopy(BASE)
        obj_b["noc"]["routers"][0]["in_fifo_occ_max"] = 99
        a = _write(os.path.join(td, "a.json"), obj_a)
        b = _write(os.path.join(td, "b.json"), obj_b)
        assert _run(a, b) == 1


if __name__ == "__main__":
    tests = [
        test_identical_exit_0,
        test_links_only_diff_exit_0,
        test_window_diff_exit_1,
        test_schema_version_diff_exit_1,
        test_no_noc_key_exit_0,
        test_routers_diff_exit_1,
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
        except Exception as e:
            print(f"ERROR {t.__name__}: {e}")
            failed += 1
    print(f"\n{passed}/{passed + failed} tests passed")
    sys.exit(1 if failed else 0)
