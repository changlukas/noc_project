import pytest

import run_vc_buffer_tradeoff as campaign


def test_tradeoff_matrix_is_fixed_depth_plus_equal_entry_slices():
    assert campaign.CONFIGS == (
        (1, 8), (4, 8), (8, 8),
        (1, 32), (2, 16), (2, 32), (4, 16),
    )


def test_set_default_changes_only_the_named_parameter():
    source = """  DAT_NUM_VC:\n    type: int\n    default: 2\n  ROUTER_VC_DEPTH:\n    type: int\n    default: 8\n  NI_DAT_RX_VC_DEPTH:\n    type: int\n    default: 8\n"""
    changed = campaign.set_default(source, "DAT_NUM_VC", 4)
    assert "DAT_NUM_VC:\n    type: int\n    default: 4" in changed
    assert "ROUTER_VC_DEPTH:\n    type: int\n    default: 8" in changed
    assert "NI_DAT_RX_VC_DEPTH:\n    type: int\n    default: 8" in changed
    with pytest.raises(ValueError):
        campaign.set_default(source, "MISSING", 1)


def test_regenerate_params_calls_both_generated_languages(monkeypatch):
    commands = []
    monkeypatch.setattr(campaign, "run", commands.append)
    campaign.regenerate_params()
    assert commands == [
        ["python3", "specgen/tools/codegen.py", "--target", "cpp", "--domain", "params"],
        ["python3", "specgen/tools/codegen.py", "--target", "sv", "--domain", "params"],
    ]


def test_candidate_configuration_couples_router_and_ni_rx_depth():
    original = """  DAT_NUM_VC:\n    default: 2\n  ROUTER_VC_DEPTH:\n    default: 8\n  NI_DAT_RX_VC_DEPTH:\n    default: 8\n"""
    configured = campaign.configure_candidate(original, 4, 16)

    assert "DAT_NUM_VC:\n    default: 4" in configured
    assert "ROUTER_VC_DEPTH:\n    default: 16" in configured
    assert "NI_DAT_RX_VC_DEPTH:\n    default: 16" in configured


def test_tradeoff_has_five_fixed_outstanding_results_per_candidate():
    assert campaign.RESULTS_PER_CONFIG == 5
    assert campaign.SOURCE_OUTSTANDING_DEPTH == 32
    assert campaign.MAX_TXNS_PER_ID == 32


def test_candidate_command_records_configuration_application_and_run():
    command = campaign.candidate_repro_command(
        vc=4,
        depth=16,
        seed=7,
        build_root="/home/lucas/noc_build/verilator",
    )

    assert command.startswith(
        "python3 sim/tools/run_vc_buffer_tradeoff.py --apply-only "
        "--vc 4 --depth 16 --build-root /home/lucas/noc_build/verilator && "
    )
    assert "make -C sim/verilator sim-outstanding-ai-tradeoff" in command
    assert "CONFIG=mesh_4x4" in command
    assert "SEED=7" in command
    assert "TRADEOFF_CONFIG_TAG=v4_b16" in command
    assert "SOURCE_OUTSTANDING_DEPTH=32" in command
    assert "MAX_TXNS_PER_ID=32" in command


def test_tradeoff_make_command_passes_reproduction_command_to_cells():
    command = campaign.tradeoff_make_command(
        vc=1,
        depth=8,
        seed=3,
        build_root="/home/lucas/noc_build/verilator",
    )

    repro = next(arg for arg in command if arg.startswith("REPRO_COMMAND="))
    assert "--apply-only --vc 1 --depth 8" in repro
    assert "sim-outstanding-ai-tradeoff" in repro


@pytest.mark.parametrize("vc,depth,tag", (
    (1, 8, "v1_b8"),
    (1, 32, "v1_b32"),
    (2, 32, "v2_b32"),
))
def test_completed_pareto_candidate_replay_refreshes_exact_command(vc, depth, tag):
    command = campaign.candidate_manifest_refresh_command(
        vc=vc,
        depth=depth,
        seed=1,
        build_root="/home/lucas/noc_build/verilator",
    )

    exact = next(arg for arg in command if arg.startswith("--command="))
    assert f"--apply-only --vc {vc} --depth {depth}" in exact
    assert f"TRADEOFF_CONFIG_TAG={tag}" in exact
    assert "SOURCE_OUTSTANDING_DEPTH=32" in exact
    assert "MAX_TXNS_PER_ID=32" in exact
