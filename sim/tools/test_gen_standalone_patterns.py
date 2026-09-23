from pathlib import Path
import pytest
from gen_standalone_patterns import generate, REPO
from test_gen_test_patterns_filemaster import _parse_read, _parse_write


@pytest.mark.parametrize("width", [1, 3, 8])
@pytest.mark.parametrize("mode", ["control", "data", "rand"])
def test_cases_roundtrip_and_legal_bursts(tmp_path, width, mode):
    names = generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", width, mode=mode)
    assert len(names) == 19
    for name in names:
        writes = _parse_write(tmp_path / name / "write.txt")
        reads = _parse_read(tmp_path / name / "read.txt")
        assert writes or reads
        if name.startswith(("ctrl_write", "data_write")):
            assert not reads
        if name.startswith(("ctrl_read", "data_read")):
            assert not writes
        for txn in writes + reads:
            assert 0 <= txn["id"] < (1 << width)
            assert txn["size"] <= (3 if txn["addr"] % (1 << 32) >= 0x2000000 else 6)
            assert txn["addr"] % (1 << txn["size"]) == 0
            if txn["burst"] == 2:
                assert txn["len"] + 1 in (2, 4, 8, 16)
        for txn in writes:
            step = 1 << txn["size"]
            span = step * (txn["len"] + 1)
            assert len(txn["beats"]) == txn["len"] + 1
            for beat, line in enumerate(txn["beats"]):
                addr = txn["addr"] if txn["burst"] == 0 else txn["addr"] + beat*step
                if txn["burst"] == 2:
                    addr = (txn["addr"] & ~(span-1)) | (addr & (span-1))
                _, strobe, _ = line.split()
                legal_strobe = ((1 << step)-1) << (addr % 64)
                assert int(strobe, 16) & ~legal_strobe == 0
    basic = _parse_write(tmp_path / "ctrl_write_single/write.txt")
    assert len(basic) == 1 and basic[0]["len"] == 0
    multi = _parse_write(tmp_path / "multi_id_outstanding/write.txt")
    assert len({t["id"] for t in multi}) == min(8, 1 << width)
    cross = _parse_read(tmp_path / "same_id_cross_dst_reorder/read.txt")
    assert len({t["id"] for t in cross}) == 1
    assert len({t["addr"] >> 32 for t in cross}) == 2
    if width == 8:
        full = _parse_write(tmp_path / "outstanding_full_recover/write.txt")
        assert len({t["id"] for t in full}) > 8
    before = {f.relative_to(tmp_path): f.read_bytes() for f in tmp_path.rglob("*") if f.is_file()}
    generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", width, mode=mode)
    assert before == {f.relative_to(tmp_path): f.read_bytes() for f in tmp_path.rglob("*") if f.is_file()}


@pytest.mark.parametrize("mode", ["control", "data", "rand"])
def test_shared_modes_preserve_scenario_and_seed(tmp_path, mode):
    generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", mode=mode, seed=17)
    for name in ("same_id_outstanding", "same_id_cross_dst_reorder"):
        txns = _parse_write(tmp_path / name / "write.txt")
        assert len({t["id"] for t in txns}) == 1
        classes = {"control" if t["addr"] % (1 << 32) >= 0x2000000 else "data" for t in txns}
        assert classes == ({"control", "data"} if mode == "rand" else {mode})
    before = (tmp_path / "request_rand/write.txt").read_bytes()
    generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", mode=mode, seed=18)
    assert (tmp_path / "request_rand/write.txt").read_bytes() != before


@pytest.mark.parametrize("name", ["ctrl_write_burst", "data_write_burst"])
def test_burst_patterns_exercise_lanes_and_wrap(tmp_path, name):
    generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml")
    txns = _parse_write(tmp_path / name / "write.txt")
    assert len({t["addr"] % 64 for t in txns}) > 1
    assert {t["size"] for t in txns} == set(range(7 if name.startswith("data") else 4))
    assert {t["burst"] for t in txns} == {0, 1, 2}
    assert any(t["burst"] == 2 and t["addr"] % ((t["len"]+1)*(1 << t["size"])) != 0 for t in txns)


@pytest.mark.parametrize("width", [1, 3, 8])
def test_control_capacity_preserves_existing_recipe(tmp_path, width):
    from gen_nmu_standalone_patterns import generate as generate_legacy
    topology = REPO / "sim/configs/mesh_2x2.yml"
    generate_legacy(tmp_path / "legacy", topology, width)
    generate(tmp_path / "new", topology, width, case_name="outstanding_full_recover")
    for name in ("write.txt", "read.txt"):
        assert (tmp_path / "legacy" / name).read_bytes() == (tmp_path / "new/outstanding_full_recover" / name).read_bytes()


def test_in_order_performance_inputs(tmp_path):
    catalog = REPO / "sim/test_patterns/standalone/in_order_perf.json"
    names = generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", catalog=catalog)
    assert len(names) == 8
    for name in names:
        writes = _parse_write(tmp_path / name / "write.txt")
        reads = _parse_read(tmp_path / name / "read.txt")
        assert bool(writes) != bool(reads)
        txns = writes + reads
        assert len(txns) == 128
        assert len({t["id"] for t in txns}) == 1
        assert len({t["addr"] >> 32 for t in txns}) == 1
        beats = 16 if name.endswith("burst") else 1
        size = 6 if "data" in name else 3
        assert all(t["len"] == beats-1 and t["size"] == size and t["burst"] == 1 for t in txns)
        for t in txns:
            assert t["addr"] >> 12 == (t["addr"] + beats*(1 << size)-1) >> 12
        schedule = (tmp_path / name / "schedule.txt").read_text()
        for key in ("response_order", "response_delay", "startup_delay", "stall_enable", "reset_warmup"):
            assert "+" + key + "=0\n" in schedule


def test_out_of_order_performance_capacity(tmp_path):
    catalog = REPO / "sim/test_patterns/standalone/out_of_order_perf.json"
    names = generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", catalog=catalog)
    assert len(names) == 12
    for name in names:
        writes = _parse_write(tmp_path / name / "write.txt")
        reads = _parse_read(tmp_path / name / "read.txt")
        assert bool(writes) != bool(reads)
        txns = writes + reads
        assert len(txns) == (16 if name.endswith("mixed_id") else 8)
        assert all(t["len"] == 7 and t["burst"] == 1 for t in txns)
        ids = {t["id"] for t in txns}
        assert len(ids) == (1 if name.endswith("same_id") else 8)
        # At most all but the first request per ID need ROB entries.
        assert (len(txns) - len(ids))*8 < 128
        assert max(sum(t["id"] == i for t in txns) for i in ids) < 32
        if name.endswith("mixed_id"):
            assert any(txns[i]["addr"] >> 32 != txns[i+8]["addr"] >> 32 for i in range(8))
        schedule = (tmp_path / name / "schedule.txt").read_text()
        for key in ("response_delay", "stall_enable", "reset_warmup"):
            assert "+" + key + "=0\n" in schedule


def test_mixed_performance_inputs(tmp_path):
    catalog = REPO / "sim/test_patterns/standalone/mixed_perf.json"
    names = generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", catalog=catalog)
    assert len(names) == 4
    for name in names:
        writes = _parse_write(tmp_path / name / "write.txt")
        reads = _parse_read(tmp_path / name / "read.txt")
        assert len(writes) == len(reads) == 128
        txns = writes + reads
        assert len({t["id"] for t in txns}) == 1
        assert len({t["addr"] >> 32 for t in txns}) == 1
        beats = 16 if name.endswith("burst") else 1
        size = 6 if "data" in name else 3
        assert all(t["len"] == beats-1 and t["size"] == size and t["burst"] == 1 for t in txns)
        schedule = (tmp_path / name / "schedule.txt").read_text()
        for key in ("response_order", "response_delay", "startup_delay", "stall_enable", "reset_warmup"):
            assert "+" + key + "=0\n" in schedule


@pytest.mark.parametrize("mode", ["control", "data", "rand"])
def test_cosim_memory_dependencies(tmp_path, mode):
    topology = REPO / "sim/cosim/nmu/topology.yml"
    names = generate(tmp_path, topology, 3, mode=mode, profile="cosim")
    assert "request_rand" in names
    assert "same_id_cross_dst_reorder" not in names
    assert "cross_id_out_of_order" not in names
    for name in names:
        writes = _parse_write(tmp_path / name / "write.txt")
        reads = _parse_read(tmp_path / name / "read.txt")
        assert len(writes) == len(reads) > 0
        initialized = set()
        for write, read in zip(writes, reads):
            assert all(write[k] == read[k] for k in ("id", "addr", "len", "size", "burst"))
            assert write["burst"] == 1
            step = 1 << write["size"]
            for beat, line in enumerate(write["beats"]):
                address = write["addr"] + beat * step
                assert address >> 12 == write["addr"] >> 12
                byte_addresses = set(range(address, address + step))
                assert not (initialized & byte_addresses)
                initialized.update(byte_addresses)
                strobe = int(line.split()[1], 16)
                assert strobe == ((1 << step) - 1) << (address % 64)
            assert write["addr"] + (write["len"] + 1) * step <= (
                0x2001000 if write["addr"] >= 0x2000000 else 0x2000000)
        schedule = (tmp_path / name / "schedule.txt").read_text()
        assert "response_delay" not in schedule
        assert f"+backpressure={int(name == 'backpressure')}" in schedule
