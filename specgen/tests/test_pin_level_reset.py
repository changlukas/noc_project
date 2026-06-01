"""Pin-level reset cross-merge — Task 3."""
from pathlib import Path
import pytest
from ni_spec import loader, constants, invariants

SPEC_VALIDATE = Path(__file__).resolve().parent.parent


def test_every_signal_has_pin_name():
    spec = loader.load_doc(SPEC_VALIDATE / "generated" / "ni_signals.json")
    for iface in spec["interfaces"]:
        for ch in iface.get("channels", []):
            for sig in ch["signals"]:
                assert sig["pin_name"] is not None, f"AXI signal in {iface['name']}/{ch['name']} pin_name still null"
        for sig in iface.get("signals", []):
            assert sig["pin_name"] is not None, f"NoC signal {sig.get('pin_name','<no pin_name>')} pin_name still null"


def test_every_signal_has_reset_behavior():
    spec = loader.load_doc(SPEC_VALIDATE / "generated" / "ni_signals.json")
    for iface in spec["interfaces"]:
        for ch in iface.get("channels", []):
            for sig in ch["signals"]:
                rb = sig["reset_behavior"]
                assert rb is not None, f"signal {sig['pin_name']} reset_behavior null"
                assert rb["kind"] in ("async-active-low", "sync-active-high", "external_driven")
        for sig in iface.get("signals", []):
            rb = sig["reset_behavior"]
            assert rb is not None, f"NoC {sig['pin_name']} reset_behavior null"
            assert rb["kind"] in ("async-active-low", "sync-active-high", "external_driven")


def test_input_wires_are_external_driven():
    """At least one AXI input wire (e.g. axi_awid_i) must be external_driven with no value."""
    spec = loader.load_doc(SPEC_VALIDATE / "generated" / "ni_signals.json")
    sig = constants.signals_signal_by_pin(spec, "axi_awid_i")
    assert sig is not None, "axi_awid_i not found"
    assert sig["reset_behavior"]["kind"] == "external_driven"
    assert "value" not in sig["reset_behavior"]


def test_pin_name_uniqueness():
    spec = loader.load_doc(SPEC_VALIDATE / "generated" / "ni_signals.json")
    pins = constants.signals_pin_names(spec)
    assert len(pins) == len(set(pins)), f"duplicate pin_name detected: {[p for p in pins if pins.count(p) > 1]}"


def test_validator_catches_unknown_reset_domain():
    """L2 check: reset_behavior.domain must be in meta.reset_signals."""
    bogus = {
        "meta": {"reset_signals": ["arst_ni"]},
        "interfaces": [{
            "channels": [{
                "signals": [{
                    "pin_name": "bogus_o", "direction": "output",
                    "reset_behavior": {"kind": "async-active-low", "value": "0", "domain": "fake_rst"}
                }]
            }]
        }]
    }
    issues = invariants.check_signals_reset_domains(bogus)
    assert any("fake_rst" in i.message for i in issues), "should catch fake_rst not in whitelist"
