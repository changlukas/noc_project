from pathlib import Path
import pytest
from gen_standalone_patterns import generate, REPO
from test_gen_test_patterns_filemaster import _parse_read, _parse_write


@pytest.mark.parametrize("width", [1, 3, 8])
def test_cases_roundtrip_and_legal_bursts(tmp_path, width):
    names = generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", width)
    assert len(names) == 12
    for name in names:
        writes = _parse_write(tmp_path / name / "write.txt")
        reads = _parse_read(tmp_path / name / "read.txt")
        assert writes or reads
        if name.startswith("ctrl_write"):
            assert not reads
        if name.startswith("ctrl_read"):
            assert not writes
        for txn in writes + reads:
            assert 0 <= txn["id"] < (1 << width)
            assert txn["size"] <= 3
            assert txn["addr"] % (1 << txn["size"]) == 0
            assert txn["addr"] % (1 << 32) >= 0x2000000
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
    generate(tmp_path, REPO / "sim/configs/mesh_2x2.yml", width)
    assert before == {f.relative_to(tmp_path): f.read_bytes() for f in tmp_path.rglob("*") if f.is_file()}
