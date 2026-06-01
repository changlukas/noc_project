"""Signal schema + pin_level_reset parser — Task 2."""
import json
from pathlib import Path
import pytest
from ni_spec import generator, constants, loader

SPECGEN_ROOT = Path(__file__).resolve().parent.parent
MD_DIR = SPECGEN_ROOT.parent / "spec" / "ni" / "doc"


def test_extract_reset_signals_returns_arst_and_noc_rst():
    """parse_pin_level_reset extracts the `Reset signals:` bullet list."""
    reset_signals = generator.extract_reset_signals(MD_DIR / "pin_level_reset.md")
    assert "arst_ni" in reset_signals
    assert "noc_rst_ni" in reset_signals
    assert len(reset_signals) >= 2


def test_extract_reset_signals_handles_missing_file():
    """Missing file raises FileNotFoundError."""
    with pytest.raises(FileNotFoundError):
        generator.extract_reset_signals(Path("/nonexistent"))


def test_signals_reset_domains_after_extract():
    """constants.signals_reset_domains returns same set as meta.reset_signals."""
    spec = {"meta": {"reset_signals": ["arst_ni", "noc_rst_ni"]}}
    assert constants.signals_reset_domains(spec) == {"arst_ni", "noc_rst_ni"}


def test_regenerated_ni_signals_has_pin_name_field():
    """After regen, every signal entry has pin_name key (non-null since Task 3)."""
    spec = loader.load_doc(SPECGEN_ROOT / "generated" / "json" / "ni_signals.json")
    for iface in spec["interfaces"]:
        for ch in iface.get("channels", []):
            for sig in ch["signals"]:
                assert "pin_name" in sig, f"signal {sig.get('pin_name','<no pin_name>')} missing pin_name field"
        # Also cover NoC link signals at interface top level
        for sig in iface.get("signals", []):
            assert "pin_name" in sig, f"NoC signal {sig.get('pin_name','<no pin_name>')} missing pin_name field"


def test_regenerated_ni_signals_has_meta_reset_signals():
    """After regen, meta.reset_signals is populated."""
    spec = loader.load_doc(SPECGEN_ROOT / "generated" / "json" / "ni_signals.json")
    assert "reset_signals" in spec["meta"]
    assert "arst_ni" in spec["meta"]["reset_signals"]


def test_pins_bundle_struct_emitted_per_interface():
    """Each ni_signals interface should produce a ni::pins::<Name>Pins struct.

    Interface names in ni_signals.json are UPPERCASE_SNAKE
    (e.g. AXI_SLAVE_PORT, NOC_REQ_OUT). PascalCase + 'Pins' suffix.
    """
    header = SPECGEN_ROOT / "generated" / "cpp" / "ni_signals.h"
    text = header.read_text(encoding="ascii")
    expected_bundles = (
        "AxiSlavePortPins",
        "AxiMasterPortPins",
        "NocReqInPins",
        "NocReqOutPins",
        "NocRspInPins",
        "NocRspOutPins",
        "CsrPins",
    )
    for bundle in expected_bundles:
        assert f"struct {bundle}" in text, f"missing pin bundle: {bundle}"
    assert "namespace pins" in text, "missing ni::pins namespace"
    assert "void reset_outputs()" in text, "missing reset_outputs() method"
