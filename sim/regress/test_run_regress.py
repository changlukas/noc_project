import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_regress

MATRIX = {
    "tiers": {"t": {"topologies": ["mesh_4x4_vc1", "mesh_4x4_vc4"],
                    "rob_modes": ["disabled", "enabled"],
                    "stimuli": [{"from": "AX4-BAS-003", "patterns": ["neighbor", "hotspot"]},
                                {"from": ["AX4-BUR-003"], "patterns": ["neighbor"],
                                 "preserve_addr": True}]}},
    "exclusions": [{"when": {"rob_mode": "enabled", "from": "AX4-BUR-003"},
                    "reason": "len256>ROB"}],
}

def test_expand_counts_cells():
    cells = run_regress.expand_tier(MATRIX, "t")
    # 2 topo x 2 rob x (2 patterns for BAS-003 + 1 for BUR-003) = 2*2*3 = 12
    assert len(cells) == 12

def test_rob_topology_suffix():
    cells = run_regress.expand_tier(MATRIX, "t")
    rob_cell = next(c for c in cells if c.rob_mode == "enabled")
    assert rob_cell.effective_topology().endswith("_rob")

def test_exclusion_marks_reason():
    cells = run_regress.expand_tier(MATRIX, "t")
    ex = [c for c in cells
          if run_regress.is_excluded(c, MATRIX["exclusions"])]
    # only the rob-enabled x BUR-003 cells (2 topo) are excluded
    assert len(ex) == 2
    assert all(run_regress.is_excluded(c, MATRIX["exclusions"]) == "len256>ROB" for c in ex)

def test_preserve_addr_flag_carried():
    cells = run_regress.expand_tier(MATRIX, "t")
    assert any(c.preserve_addr for c in cells)
    assert any(not c.preserve_addr for c in cells)


def test_wire_verifiable_filter():
    rsp_read = run_regress.resolve_scenario("AX4-RSP-001")   # category: response (decerr read)
    rsp_write = run_regress.resolve_scenario("AX4-RSP-002")  # write-only OOB (0 reads)
    data = run_regress.resolve_scenario("AX4-BAS-003")       # write+read data scenario
    assert run_regress.is_wire_verifiable(rsp_read) is False
    assert run_regress.is_wire_verifiable(rsp_write) is False
    assert run_regress.is_wire_verifiable(data) is True
