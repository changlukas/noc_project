import csv
from collections import Counter

import pytest

from axi_transaction_metrics import attach_admission, packet_layout, read_transactions, require_rows, summarize


def trace(tmp_path, rows):
    path = tmp_path / "trace.csv"
    with path.open("w") as stream:
        writer = csv.writer(stream)
        writer.writerow("cycle,event,id,address,len,size,last,ready,in_window,user".split(","))
        writer.writerows(rows)
    return read_transactions(path)


def event(cycle, channel, ident=0, *, length=0, last=0, ready=1, window=1, user="0"):
    return cycle, channel, ident, "1000", length, 6, last, ready, window, user


@pytest.mark.parametrize("fault", [None, "early", "missing", "duplicate", "wrong_address"])
def test_request_admission_pairs_repeated_requests(tmp_path, fault):
    fields, channels = packet_layout()
    values = dict(AXI_CH=next(k for k, v in channels.items() if v == "DataAw"),
                  AWID=0, AWADDR=0x1000, AWLEN=0, AWSIZE=6)
    if fault == "wrong_address":
        values["AWADDR"] += 1
    word = sum(value << fields[name][0] for name, value in values.items())
    txns = [dict(node=0, direction="AW", axi_class="Data", id=0, address=0x1000,
                 beats=1, size=6, first_valid_cycle=start, accepted_cycle=accepted)
            for start, accepted in ((1, 3), (4, 5))]
    cycles = [2 if fault == "early" else 7, 9]
    if fault == "missing":
        cycles.pop()
    if fault == "duplicate":
        cycles.append(10)
    path = tmp_path / "packets.csv"
    with path.open("w") as stream:
        writer = csv.writer(stream)
        writer.writerow(["cycle", "event", "node", "plane", "flit"])
        writer.writerows((cycle, "IN", 0, "DAT", f"{word:x}") for cycle in cycles)
        writer.writerow([11, "END", 0, "DAT", "0"])
    if fault:
        with pytest.raises(ValueError):
            attach_admission(txns, path)
    else:
        attach_admission(txns, path)
        assert [t["admission_latency_cycles"] for t in txns] == [6, 5]
        assert [t["header_injection_cycle"] for t in txns] == [7, 9]


def test_read_same_id_fifo_and_cross_id_completion(tmp_path):
    txns = trace(tmp_path, [
        event(1, "AR", ready=0), event(3, "AR", length=0),
        event(4, "AR"), event(5, "AR", 1, length=1),
        event(6, "R", 1), event(7, "R", 1, last=1),
        event(8, "R", ready=0, last=1), event(10, "R", last=1),
        event(11, "R", last=1), event(12, "END")])
    assert [txn["completion_cycle"] for txn in txns] == [10, 11, 7]
    metrics = summarize(txns)["metrics"]
    assert metrics["read_source_wait"]["sum_cycles"] == 2
    assert metrics["read_completion"]["sum_cycles"] == 16
    assert metrics["read_first_response_wait"]["sum_cycles"] == 2


def test_independent_write_channels_and_same_cycle_events(tmp_path):
    txns = trace(tmp_path, [
        event(1, "W", last=1), event(2, "AW", 1),
        event(3, "AW", 2), event(3, "W", last=1),
        event(3, "B", 2), event(4, "B", 1), event(5, "END")])
    assert [txn["wlast_cycle"] for txn in txns] == [1, 3]
    assert summarize(txns)["metrics"]["write_completion"]["sum_cycles"] == 2
    assert summarize(txns)["metrics"]["write_wlast_to_b"]["sum_cycles"] == 3


def test_reset_and_window_censoring_are_explicit(tmp_path):
    txns = trace(tmp_path, [
        event(1, "AR"), event(2, "RESET"),
        event(1, "AR", window=0), event(2, "R", last=1),
        event(3, "AR"), event(4, "R", last=1, window=0),
        event(5, "AW", ready=0), event(6, "END")])
    assert [txn["status"] for txn in txns] == [
        "reset", "complete", "completed_outside_window", "incomplete"]
    summary = summarize(txns)
    assert summary["outside_window_requests"] == 1
    assert summary["metrics"] == {}


@pytest.mark.parametrize("rows, message", [
    ([event(1, "AR")], "missing END"),
    ([event(1, "R", last=1)], "without accepted address"),
    ([event(1, "AW"), event(2, "B")], "B before WLAST"),
    ([event(1, "AW", length=1), event(2, "W", last=1)], "W burst length"),
    ([event(1, "AR", length=1), event(2, "R", last=1)], "RLAST"),
    ([event(1, "AR", ready=0), event(2, "AR", 1)], "address changed"),
    ([event(1, "AW", ready=0), event(2, "AW", user="1")], "address changed"),
])
def test_malformed_evidence_rejected(tmp_path, rows, message):
    with pytest.raises(ValueError, match=message):
        trace(tmp_path, rows)


def test_stalls_count_every_beat_only_inside_window(tmp_path):
    trace(tmp_path, [event(0, "AR", window=0),
                     event(1, "R", last=1, ready=0),
                     event(2, "R", last=1, ready=0),
                     event(3, "R", last=1),
                     event(4, "AW", ready=0, window=0), event(5, "END")])
    stalls = Counter()
    read_transactions(tmp_path / "trace.csv", dict(start_cyc=1, end_cyc=4), stalls)
    assert stalls == {"R": 2}


def test_missing_zero_activity_counter_is_not_zero_measurement():
    row = dict(node="nmu_0", channel="AW")
    expected = {("nmu_0", "AW")}
    require_rows([row], ("node", "channel"), expected)
    for rows in ([], [row, row], [dict(node="nmu_1", channel="AW")]):
        with pytest.raises(ValueError, match="counter rows"):
            require_rows(rows, ("node", "channel"), expected)
