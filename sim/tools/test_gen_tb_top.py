import gen_tb_top as g
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_user_endpoint_exports_loaded_stimulus_count():
    endpoint = (ROOT / "sim/tb/test/user_node_endpoint.sv").read_text(encoding="utf-8")
    generated = g.emit_tb_top(g.load_topology("mesh_4x4"))

    assert "output int unsigned                expected_txn_cnt_o" in endpoint
    assert "expected_txn_cnt_o = int'(file_master.num_writes + file_master.num_reads);" in endpoint
    assert "int unsigned expected_txn_cnt [16];" in generated
    assert ".expected_txn_cnt_o(expected_txn_cnt[i])" in generated


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
