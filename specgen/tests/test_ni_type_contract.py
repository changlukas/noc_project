"""Generated NI channel/flit type contract and focused SV elaboration."""
from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest


SPECGEN_ROOT = Path(__file__).resolve().parent.parent
SV_DIR = SPECGEN_ROOT / "generated" / "sv"
HARNESS = Path(__file__).resolve().parent / "sv" / "tb_ni_type_contract.sv"
CHILD_TYPES = SPECGEN_ROOT.parent / "rtl" / "common" / "ni_child_types_pkg.sv"


def _typedef_fields(text: str, type_name: str) -> list[str]:
    blocks = re.finditer(
        r"typedef struct packed \{\n(?P<body>.*?)\n\s+\} (?P<name>[a-z][a-z0-9_]*);",
        text,
        flags=re.DOTALL,
    )
    for block in blocks:
        if block.group("name") == type_name:
            return re.findall(r"\b([a-z][a-z0-9_]*)\s*;", block.group("body"))
    raise AssertionError(f"missing packed typedef {type_name}")


def test_axi_channel_payload_fields_and_packed_order():
    text = (SV_DIR / "ni_signals_pkg.sv").read_text(encoding="ascii")
    expected_msb_to_lsb = {
        "axi_aw_t": [
            "awuser", "awqos", "awregion", "awprot", "awlock", "awcache",
            "awburst", "awsize", "awlen", "awaddr", "awid",
        ],
        "axi_w_t": ["wdata", "wstrb", "wlast"],
        "axi_b_t": ["bresp", "bid"],
        "axi_ar_t": [
            "arqos", "arregion", "arprot", "arlock", "arcache", "arburst",
            "arsize", "arlen", "araddr", "arid",
        ],
        "axi_r_t": ["rdata", "rresp", "rid", "rlast"],
    }
    for type_name, fields in expected_msb_to_lsb.items():
        emitted = _typedef_fields(text, type_name)
        assert emitted == fields, f"{type_name} packed field order drifted"
        assert not any(name.endswith(("valid", "ready")) for name in emitted)


def test_flit_container_fields_and_packed_order():
    text = (SV_DIR / "ni_flit_pkg.sv").read_text(encoding="ascii")
    for type_name in ("req_flit_t", "rsp_flit_t", "dat_flit_t"):
        assert _typedef_fields(text, type_name) == ["payload", "header"]


def test_child_record_field_sets():
    text = CHILD_TYPES.read_text(encoding="ascii")
    expected = {
        "nmu_ordering_domain_t": ["dst_id", "dst_port_id", "is_data"],
        "nmu_route_t": ["domain"],
        "nmu_aw_route_t": ["route", "user", "collective_op", "collective_mask"],
        "nmu_request_t": ["route", "ordering_req", "ordering_tag"],
        "nmu_response_t": ["is_data", "ordering_req", "ordering_tag"],
        "nmu_sam_aw_result_t": ["axi", "route"],
        "nmu_sam_ar_result_t": ["axi", "route"],
        "nmu_aw_request_t": ["axi", "meta", "user", "collective_op", "collective_mask"],
        "nmu_ar_request_t": ["axi", "meta"],
        "nmu_b_response_t": ["axi", "meta"],
        "nmu_r_response_t": ["axi", "meta"],
        "nmu_rob_order_entry_t": ["base", "beat_count", "ordering_req", "collective"],
        "nmu_b_rob_entry_t": ["occupied", "complete", "beat"],
        "nmu_r_rob_entry_t": ["occupied", "complete", "narrow_lane", "beat"],
        "nmu_read_context_t": ["local_addr", "len", "size", "burst", "beat_index"],
        "response_entry_t": [
            "src_id", "src_port_id", "noc_id", "ordering_req", "ordering_tag",
            "is_data", "local_addr", "len", "size", "burst", "collective_op",
            "collective_mask",
        ],
        "nsu_aw_request_t": ["axi", "response"],
        "nsu_ar_request_t": ["axi", "response"],
        "nsu_b_response_t": ["axi", "response"],
        "nsu_r_response_t": ["axi", "response"],
    }
    for type_name, fields in expected.items():
        assert _typedef_fields(text, type_name) == fields


def test_generated_type_harness_elaborates_and_runs(tmp_path: Path):
    verilator = shutil.which("verilator")
    if verilator is None:
        pytest.skip("verilator not in PATH")

    obj_dir = tmp_path / "obj"
    compile_result = subprocess.run(
        [
            verilator,
            "--binary",
            "-Wno-fatal",
            "--Mdir", str(obj_dir),
            "--top-module", "tb_ni_type_contract",
            str(SV_DIR / "ni_params_pkg.sv"),
            str(SV_DIR / "ni_signals_pkg.sv"),
            str(SV_DIR / "ni_flit_pkg.sv"),
            str(CHILD_TYPES),
            str(HARNESS),
        ],
        capture_output=True,
        text=True,
        cwd=str(tmp_path),
    )
    assert compile_result.returncode == 0, compile_result.stderr

    run_result = subprocess.run(
        [str(obj_dir / "Vtb_ni_type_contract")],
        capture_output=True,
        text=True,
        cwd=str(tmp_path),
    )
    assert run_result.returncode == 0, run_result.stderr
    assert "PASS: generated NI type contract" in run_result.stdout
