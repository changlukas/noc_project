import gen_tb_top as g
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_user_endpoint_exports_loaded_stimulus_count():
    endpoint = (ROOT / "sim/tb/test/user_node_endpoint.sv").read_text(encoding="utf-8")
    generated = g.emit_tb_top(g.load_topology("mesh_4x4"))

    assert "output int unsigned                expected_txn_cnt_o" in endpoint
    assert "output int unsigned                expected_write_cnt_o" in endpoint
    assert "output longint unsigned            stimulus_start_cycle_o" in endpoint
    assert "output longint unsigned            stimulus_done_cycle_o" in endpoint
    assert "expected_txn_cnt_o = int'(file_master.num_writes + file_master.num_reads);" in endpoint
    assert "expected_write_cnt_o = int'(file_master.num_writes);" in endpoint
    assert "int unsigned expected_txn_cnt [16];" in generated
    assert "int unsigned expected_write_cnt [16];" in generated
    assert ".expected_txn_cnt_o(expected_txn_cnt[i])" in generated
    assert ".expected_write_cnt_o(expected_write_cnt[i])" in generated


def test_generated_top_emits_strict_round_measurement_evidence():
    text = g.emit_tb_top(g.load_topology("mesh_4x4"))

    assert "longint unsigned stimulus_start_cycle [16];" in text
    assert "longint unsigned stimulus_done_cycle [16];" in text
    assert 'void\'($value$plusargs("round_perf=%d", round_perf));' in text
    assert '"[TrafficMeta] active_sources=%0d write_bursts=%0d"' in text
    assert '"[RoundPerf] start_cycle=%0d completion_cycle=%0d round_cycles=%0d active_sources=%0d write_bursts=%0d"' in text
    assert "if (injection_mode != 0)" in text
    assert "expected_txn_cnt[i] != expected_write_cnt[i]" in text
    assert "stimulus_start_cycle[i] != round_start" in text


def test_make_wires_round_perf_and_transfer_geometry_to_csv():
    makefile = (ROOT / "sim/verilator/Makefile").read_text(encoding="utf-8")

    assert '"+round_perf=$(ROUND_PERF)"' in makefile
    assert "--require-round-perf" in makefile
    assert "--stim-size $(STIM_SIZE)" in makefile


def test_non_vacuity_uses_loaded_stimulus_counts():
    text = g.emit_tb_top(g.load_topology("mesh_4x4"))

    assert "expected_total += expected_txn_cnt[i];" in text
    assert "expected_txn_cnt[i] > 0 && txn_cnt[i] == 0" in text
    assert 'if (expected_total == 0) $fatal(1, "tb_top: empty stimulus");' in text


def test_watchdog_uses_loaded_stimulus_count():
    text = g.emit_tb_top(g.load_topology("mesh_4x4"))

    assert "expected_total" in text
    assert "tb_num_reads + tb_num_writes" not in text


def test_channel_compare_barrier_is_wired_into_generated_top():
    text = g.emit_tb_top(g.load_topology("mesh_4x4"))

    assert "logic compare_ready [16];" in text
    assert "logic compare_start;" in text
    assert "logic compare_done [16];" in text
    assert ".compare_ready_o(compare_ready[i])" in text
    assert ".compare_start_i(compare_start)" in text
    assert ".compare_done_o(compare_done[i])" in text
    assert "compare_start &= compare_ready[i];" in text
    assert "all_done &= injection_mode == 3 ? compare_done[i] : end_of_sim[i];" in text
    assert "if (expected_txn_cnt[i] > 0 && txn_cnt[i] == 0)" in text
