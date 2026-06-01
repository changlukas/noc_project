"""Register L2 validator — Task 4."""
import pytest
from ni_spec import invariants


def test_check_offset_alignment_catches_misaligned():
    regs = {"registers": [{"offset": "0x003", "name": "BAD", "kind": "register"}]}
    issues = invariants.check_csr_offset_alignment(regs)
    assert any("BAD" in i.message for i in issues)


def test_check_offset_alignment_passes_aligned():
    regs = {"registers": [{"offset": "0x010", "name": "GOOD", "kind": "register"}]}
    issues = invariants.check_csr_offset_alignment(regs)
    assert len(issues) == 0


def test_check_offset_unique_catches_collision():
    regs = {"registers": [
        {"offset": "0x010", "name": "A", "kind": "register"},
        {"offset": "0x010", "name": "B", "kind": "register"},
    ]}
    issues = invariants.check_csr_offset_unique(regs)
    assert any("0x010" in i.message for i in issues)


def test_check_field_tiling_catches_overlap():
    regs = {"registers": [{
        "offset": "0x000", "name": "X", "kind": "register",
        "fields": [
            {"name": "a", "bit_high": 3, "bit_low": 0},
            {"name": "b", "bit_high": 5, "bit_low": 2},  # overlap at bits 2,3
        ]
    }]}
    issues = invariants.check_field_bit_tiling(regs)
    assert any("overlap" in i.message.lower() for i in issues)


def test_check_reset_in_data_width_catches_overflow():
    regs = {"registers": [{
        "offset": "0x000", "name": "HUGE", "kind": "register",
        "reset_expr": "0x100000000",  # exceeds 32 bits
    }]}
    issues = invariants.check_reset_in_data_width(regs)
    assert any("HUGE" in i.message for i in issues)


def test_check_reset_in_data_width_passes_valid():
    regs = {"registers": [{
        "offset": "0x000", "name": "OK", "kind": "register",
        "reset_expr": "0xFFFF",
    }]}
    issues = invariants.check_reset_in_data_width(regs)
    assert len(issues) == 0


def test_all_four_validators_pass_on_real_data():
    """Run all 4 L2 validators on the real generated ni_registers.json — must all PASS."""
    from pathlib import Path
    import json
    gen_dir = Path(__file__).resolve().parent.parent / "generated" / "json"
    regs_path = gen_dir / "ni_registers.json"
    if not regs_path.exists():
        pytest.skip("ni_registers.json not yet generated")
    regs = json.loads(regs_path.read_text(encoding="utf-8"))
    issues = []
    issues += invariants.check_csr_offset_alignment(regs)
    issues += invariants.check_csr_offset_unique(regs)
    issues += invariants.check_field_bit_tiling(regs)
    issues += invariants.check_reset_in_data_width(regs)
    errors = [i for i in issues if i.severity == "ERROR"]
    assert errors == [], f"L2 register errors:\n" + "\n".join(str(e) for e in errors)
