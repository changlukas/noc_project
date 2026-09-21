"""Packet conservation and timing checks for the accepted-link trace."""

import pytest

from packet_metrics import check_link_counts, measure, measure_link_windows


WINDOW = dict(start_cyc=1, end_cyc=20)
ENDPOINTS = {0: (0, 0), 1: (1, 0), 2: (2, 0)}


def event(cycle, direction, node=0, word="ab", tail=1, vc=0, **fields):
    return dict(cycle=cycle, event=direction, node=node, plane="DAT", vc=vc,
                tail=tail, src=0, dst=1, dst_port=0, collective=0, mask=0,
                in_window=int(1 <= cycle < 20), flit=word, **fields)


def run(rows):
    return measure(rows + [dict(cycle=21, event="END")], ENDPOINTS, WINDOW)


def test_interleaved_packets_and_destination_vc_remap():
    rows = [event(1, "IN", tail=0), event(2, "IN", word="cd", vc=1),
            event(3, "IN", word="ef"), event(4, "OUT", 1, tail=0, vc=2),
            event(5, "OUT", 1, word="cd", vc=3),
            event(6, "OUT", 1, word="ef", vc=2)]
    result = run(rows)
    assert sorted(d["latency_cycles"] for d in result["deliveries"]) == [3, 5]
    assert result["summary"]["DAT"] == dict(samples=2, mean_cycles=4,
                                           max_cycles=5, sum_cycles=8)
    assert not result["missing_deliveries"]


def test_multicast_pairs_each_destination_and_reports_missing_branch():
    rows = [event(1, "IN"), event(4, "OUT", 0), event(7, "OUT", 1)]
    for row in rows:
        row.update(collective=1, mask=1, dst=0)
    result = run(rows)
    assert [(d["destination"], d["latency_cycles"]) for d in result["deliveries"]] == [(0, 3), (1, 6)]
    missing = run(rows[:-1])["missing_deliveries"]
    assert len(missing) == 1 and missing[0]["destination"] == 1


@pytest.mark.parametrize("outputs", [
    [event(4, "OUT", 1, word="bad")],
    [event(4, "OUT", 1), event(5, "OUT", 1)],
])
def test_corrupt_or_duplicate_delivery_rejected(outputs):
    with pytest.raises(ValueError, match="no matching injection"):
        run([event(1, "IN")] + outputs)


def test_incomplete_and_cross_window_are_not_mean_samples():
    result = run([event(0, "IN"), event(2, "OUT", 1),
                  event(3, "IN", tail=0, word="12")])
    assert not result["summary"]
    assert len(result["deliveries"]) == 1
    assert len(result["incomplete_packets"]) == 1
    with pytest.raises(ValueError, match="missing END"):
        measure([event(1, "IN")], ENDPOINTS, WINDOW)


def test_collective_responses_retained_without_one_to_one_claim():
    rows = [event(1, "IN"), event(2, "IN", 2), event(5, "OUT", 1)]
    for row in rows:
        row.update(plane="RSP", collective=1)
    result = run(rows)
    assert len(result["aggregation_observations"]) == 3
    assert not result["deliveries"] and not result["missing_deliveries"]


def test_reset_aborts_partial_packet():
    result = run([event(1, "IN", tail=0), dict(event="RESET", cycle=2)])
    assert result["incomplete_packets"][0]["status"] == "reset"


def test_empty_trace_cannot_hide_active_link():
    links = [dict(name=f"{plane}_{direction}_0", flit_count=0)
             for plane in ("req", "rsp", "dat") for direction in ("inject", "eject")]
    result = run([])
    check_link_counts(result, links, [0], [])
    links[0]["flit_count"] = 1
    with pytest.raises(ValueError, match="trace/link counter mismatch"):
        check_link_counts(result, links, [0], [])


def test_link_windows_count_idle_slots_and_reject_missing_flits():
    rows = [dict(cycle=cycle, event="TICK", link="", in_window=1)
            for cycle in range(1, 5)]
    rows.insert(2, dict(cycle=2, event="LINK", link="req_0to1", in_window=1))
    rows.append(dict(cycle=5, event="END", link="", in_window=0))
    links = [dict(name="req_0to1", flit_count=1)]
    windows = dict(all=(1, 4), overlap=(2, 3), idle=(3, 4))
    result = measure_link_windows(rows, links, windows)
    assert result["all"]["planes"]["req"]["maximum_link_utilization_percent"] == 25
    assert result["overlap"]["planes"]["req"]["maximum_link_utilization_percent"] == 50
    assert result["idle"]["planes"]["req"]["maximum_link_utilization_percent"] == 0
    with pytest.raises(ValueError, match="counter mismatch"):
        measure_link_windows([r for r in rows if r["event"] != "LINK"], links, windows)
    with pytest.raises(ValueError, match="link clock"):
        measure_link_windows([r for r in rows if r["cycle"] != 3], links, windows)
