"""Register parser — Task 4."""
from pathlib import Path
import pytest
from ni_spec import generator

SPEC_VALIDATE = Path(__file__).resolve().parent.parent
MD_DIR = SPEC_VALIDATE.parent / "spec" / "ni" / "doc"


def test_parse_csr_policy_has_four_keys():
    policy = generator.parse_csr_policy(MD_DIR / "registers.md")
    assert set(policy.keys()) >= {"sub_word_write", "unmapped_read", "misaligned", "wo_read"}


def test_parse_register_map_includes_reserved_row():
    """0x110 (reserved for LAST_ERR_INFO_HI) must show up as kind=reserved."""
    regs = generator.parse_register_map(MD_DIR / "registers.md")
    reserved = [r for r in regs if r["offset"] == "0x110"]
    assert len(reserved) == 1
    assert reserved[0]["kind"] == "reserved"
    assert reserved[0].get("access") is None  # em-dash means no access


def test_parse_register_map_handles_rw1c():
    regs = generator.parse_register_map(MD_DIR / "registers.md")
    err_status = next(r for r in regs if r["name"] == "ERR_STATUS")
    assert err_status["access"] == "RW1C"
    assert err_status["reset_expr"] == "0x0"


def test_parse_register_map_skips_section_header_rows():
    """Rows like '**Error Status / IRQ**' must not be parsed as registers."""
    regs = generator.parse_register_map(MD_DIR / "registers.md")
    names = [r["name"] for r in regs]
    assert not any("Error Status" in n for n in names)


def test_parse_register_map_count():
    """All real registers from the map plus the 1 reserved row."""
    regs = generator.parse_register_map(MD_DIR / "registers.md")
    normal = [r for r in regs if r["kind"] == "register"]
    reserved = [r for r in regs if r["kind"] == "reserved"]
    # registers.md has 31 real registers (40 - 9 QoS Generator regs removed) + 1 reserved placeholder (0x110)
    assert len(normal) == 31
    assert len(reserved) == 1


def test_parse_register_fields_err_status():
    """ERR_STATUS register must have ecc_uncorr_err, route_par_err, axi_parity_err fields."""
    fields = generator.parse_register_fields(MD_DIR / "registers.md", "ERR_STATUS")
    field_names = [f["name"] for f in fields]
    assert "ecc_uncorr_err" in field_names
    assert "route_par_err" in field_names
    assert "axi_parity_err" in field_names


def test_csr_policy_elaborated_as_constexpr():
    """ni_regs.h must expose csr_policy fields as constexpr in ni::regs::csr_policy."""
    from pathlib import Path
    text = (Path(__file__).resolve().parent.parent / "include" / "ni_regs.h").read_text()
    ns_idx = text.find("namespace csr_policy")
    assert ns_idx != -1, "missing namespace csr_policy"
    body = text[ns_idx:]
    end_idx = body.find("} // namespace csr_policy")
    if end_idx != -1:
        body = body[:end_idx]
    for key in ("SUB_WORD_WRITE", "UNMAPPED_READ", "MISALIGNED", "WO_READ"):
        assert key in body, f"missing csr_policy elaboration: {key} (within namespace)"


def test_per_register_reset_const_elaborated():
    """ni_regs.h must expose <REG>_RESET constexpr per non-reserved register."""
    from pathlib import Path
    text = (Path(__file__).resolve().parent.parent / "include" / "ni_regs.h").read_text()
    assert "constexpr uint32_t TXN_MIN_LATENCY_RESET = 0xFFFF" in text, \
        "non-zero reset for TXN_MIN_LATENCY missing or wrong"
    assert "constexpr uint32_t PKT_PROBE_EN_RESET = 0x0" in text or \
           "constexpr uint32_t PKT_PROBE_EN_RESET = 0" in text, \
        "zero reset for PKT_PROBE_EN missing"


def test_all_offsets_array_elaborated():
    """ni_regs.h must expose constexpr uint32_t ALL_OFFSETS[] and ALL_OFFSETS_COUNT."""
    from pathlib import Path
    text = (Path(__file__).resolve().parent.parent / "include" / "ni_regs.h").read_text()
    assert "constexpr uint32_t ALL_OFFSETS[]" in text
    assert "constexpr std::size_t ALL_OFFSETS_COUNT" in text


def test_access_mode_enum_class_emitted():
    """ni_regs.h must expose enum class AccessMode { RO, RW, RW1C, WO } and
    per-register <REG>_ACCESS constexpr instead of 30 single-value enum classes."""
    from pathlib import Path
    text = (Path(__file__).resolve().parent.parent / "include" / "ni_regs.h").read_text()
    assert "enum class AccessMode { RO, RW, RW1C, WO };" in text, \
        "missing single AccessMode enum class"
    assert "constexpr AccessMode ERR_STATUS_ACCESS              = AccessMode::RW1C;" in text or \
           "constexpr AccessMode ERR_STATUS_ACCESS = AccessMode::RW1C;" in text, \
        "missing ERR_STATUS_ACCESS = RW1C"
    assert "AccessMode::WO" in text, "missing WO usage (EXCLUSIVE_MONITOR_CTRL)"
    assert "enum class ERR_STATUSAccess" not in text, \
        "old per-reg single-value enum still present"
