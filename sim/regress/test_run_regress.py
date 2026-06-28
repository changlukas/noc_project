import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_regress

MATRIX = {
    "topologies": ["mesh_4x4_vc1", "mesh_4x4_vc4"],
    "rob_modes": ["disabled", "enabled"],
    "stimuli": [
        {"from": "AX4-BAS-003", "patterns": ["neighbor", "hotspot"]},
        {"from": ["AX4-BUR-003"], "patterns": ["neighbor"], "preserve_addr": True},
    ],
    "exclusions": [{"when": {"rob_mode": "enabled", "from": "AX4-BUR-003"},
                    "reason": "len256>ROB"}],
}

def test_expand_counts_cells():
    cells = run_regress.expand(MATRIX)
    # 2 topo x 2 rob x (2 + 1) = 12
    assert len(cells) == 12

def test_build_filter():
    cells = [c for c in run_regress.expand(MATRIX)
             if c.effective_topology() == "mesh_4x4_vc1"]
    assert cells and all(c.effective_topology() == "mesh_4x4_vc1" for c in cells)

def test_is_xfail_matches():
    xfails = [{"when": {"from": "AX4-ORD-002"}, "reason": "known multi-id hang"}]
    hit = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-ORD-002", "neighbor", False)
    miss = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-BAS-003", "neighbor", False)
    assert run_regress.is_xfail(hit, xfails) == "known multi-id hang"
    assert run_regress.is_xfail(miss, xfails) is None

def test_self_checking_filter():
    rsp_read = run_regress.resolve_scenario("AX4-RSP-001")   # category: response (decerr read)
    rsp_write = run_regress.resolve_scenario("AX4-RSP-002")  # write-only OOB (0 reads)
    data = run_regress.resolve_scenario("AX4-BAS-003")       # write+read data scenario
    assert run_regress.is_self_checking(rsp_read) is False
    assert run_regress.is_self_checking(rsp_write) is False
    assert run_regress.is_self_checking(data) is True


def test_interior_hotspot_4x4():
    # 4x4 interior linear id = (y//2)*x + (x//2) = 2*4 + 2 = 10
    assert run_regress._interior_hotspot("mesh_4x4_vc1") == 10
    assert run_regress._interior_hotspot("mesh_4x4_vc8_rob") == 10  # _rob suffix stripped


def test_hotspot_cell_emits_default_target():
    cell = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-BAS-003", "hotspot", False)
    captured = {}
    run_regress.run_cell(cell, pathlib.Path("out"),
                         run_cmd=lambda a: (captured.setdefault("args", a), True)[1])
    assert "--hotspot" in captured["args"]
    idx = captured["args"].index("--hotspot")
    assert captured["args"][idx + 1] == "10"


def test_unique_addr_count():
    assert run_regress.unique_addr_count(
        run_regress.resolve_scenario("AX4-STR-002")) == 8
    assert run_regress.unique_addr_count(
        run_regress.resolve_scenario("AX4-BAS-003")) == 1


def test_str002_classified_dependent():
    # 8 unique addrs > 4-slot bound -> must NOT be in the independent set
    assert "AX4-STR-002" not in run_regress._ax4_by_address_mode("independent")
    assert "AX4-STR-002" in run_regress._ax4_by_address_mode("dependent")


def test_independent_set_all_fit_capacity():
    for sid in run_regress._ax4_by_address_mode("independent"):
        n = run_regress.unique_addr_count(run_regress.resolve_scenario(sid))
        assert n <= 4, f"{sid} has {n} unique addrs but is tagged independent"


def test_id_policy_forwarded_and_labeled():
    cell = run_regress.Cell("mesh_4x4_vc1", "disabled", "AX4-BAS-005",
                            "neighbor", False, "round_robin:4")
    assert "round_robin4" in cell.label()
    captured = {}
    run_regress.run_cell(cell, pathlib.Path("out"),
                         run_cmd=lambda a: (captured.setdefault("args", a), True)[1])
    assert "--id-policy" in captured["args"]
    assert captured["args"][captured["args"].index("--id-policy") + 1] == "round_robin:4"


import pathlib as _pl
def test_real_matrix_expands():
    m = run_regress.yaml.safe_load(
        (_pl.Path(run_regress.__file__).parent / "matrix.yaml").read_text())
    cells = run_regress.expand(m)
    # independent set runs 4 patterns; dependent set runs neighbor only
    assert any(c.pattern == "uniform_random" for c in cells)
    assert any(c.pattern == "neighbor" and c.preserve_addr for c in cells)
