from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ENDPOINT = ROOT / "sim" / "tb" / "test" / "user_node_endpoint.sv"
TOP = ROOT / "sim" / "tb" / "noc_tb_top.sv"


def test_channel_compare_fault_shortens_only_mode3_data_burst():
    source = ENDPOINT.read_text(encoding="utf-8")

    assert '$value$plusargs("channel_compare_fault=%d", channel_compare_fault)' in source
    assert "if (get_injection_mode() == 3) begin" in source
    assert "if (compare_is_background && channel_compare_fault) begin" in source
    assert "file_master.aw_queue[$].ax_len = 8'd254;" in source
    assert "file_master.ar_queue[$].ax_len = 8'd254;" in source
    assert "file_master.w_queue[$-1].w_last = 1'b1;" in source
    assert "file_master.w_queue.pop_back()" in source


def test_mode3_derives_background_counts_from_loaded_stimulus():
    source = ENDPOINT.read_text(encoding="utf-8")

    assert "compare_expected_bursts" in source
    assert "compare_expected_beats" in source
    assert "file_master.ar_queue.size()" in source
    assert "file_master.aw_queue.size()" in source
    mode3 = source.split("3: begin", 1)[1].split("4: begin", 1)[0]
    assert "16384" not in mode3
    assert "bursts=64" not in mode3
    assert "NODE_ID == 1 || NODE_ID == 2" not in mode3


def test_mode3_marks_and_proves_every_background_contains_control_interval():
    endpoint = ENDPOINT.read_text(encoding="utf-8")
    top = TOP.read_text(encoding="utf-8")

    assert "compare_background_o" in endpoint
    assert "compare_work_done_o" in endpoint
    assert "wait (compare_finish_i === 1'b1);" in endpoint
    assert "stimulus_start_cycle_o = cycle_cnt;" in endpoint
    assert "[ChannelCompareInterval] role=background node=%0d start_cycle=%0d completion_cycle=%0d" in endpoint
    assert "[ChannelCompareInterval] role=control node=%0d start_cycle=%0d completion_cycle=%0d" in endpoint
    assert "all_compare_work_done" in top
    assert "compare_finish = injection_mode == 3 && all_compare_work_done;" in top
    assert "compare_start = injection_mode == 3 ? mode3_start : mode4_start;" in top
    assert "!mode3_started && all_barrier_ready" in top
    assert "stimulus_start_cycle[i] > stimulus_start_cycle[0]" in top
    assert "stimulus_done_cycle[i] < stimulus_done_cycle[0]" in top
    assert "[ChannelCompareOverlap] control_node=0 background_nodes=%0d status=PASS" in top


def test_mode3_interval_ends_at_real_response_before_lifetime_barrier():
    endpoint = ENDPOINT.read_text(encoding="utf-8")
    top = TOP.read_text(encoding="utf-8")

    mode3 = endpoint.rsplit("3: begin", 1)[1].split("4: begin", 1)[0]
    assert mode3.rindex("stimulus_done_cycle_o = cycle_cnt;") < mode3.rindex(
        "compare_work_done_o = 1'b1;")
    assert mode3.rindex("stimulus_done_cycle_o = cycle_cnt;") < mode3.rindex(
        "wait (compare_finish_i === 1'b1);")
    assert "[ChannelCompareBarrier] release_cycle=%0d" in top


def test_mode3_background_issues_loaded_transactions_in_real_rounds():
    endpoint = ENDPOINT.read_text(encoding="utf-8")

    assert '$value$plusargs("channel_rounds=%d", channel_rounds)' in endpoint
    assert "compare_expected_bursts % channel_rounds" in endpoint
    assert "task automatic run_aw_channel_rounds();" in endpoint
    assert "task automatic run_ar_channel_rounds();" in endpoint
    mode3 = endpoint.rsplit("3: begin", 1)[1].split("4: begin", 1)[0]
    assert "if (compare_is_background) run_ar_channel_rounds();" in mode3
    assert "if (compare_is_background) run_aw_channel_rounds();" in mode3


def test_mode3_control_records_valid_to_ready_wait_for_read_and_write():
    endpoint = ENDPOINT.read_text(encoding="utf-8")
    aw = endpoint.split("task automatic run_aw_outstanding();", 1)[1].split(
        "endtask", 1)[0]
    ar = endpoint.split("task automatic run_ar_outstanding();", 1)[1].split(
        "endtask", 1)[0]

    assert aw.index("admitted_cycle = cycle_cnt + 1;") < aw.index(
        "file_master.drv.send_aw") < aw.index("srcq_w_sum += cycle_cnt - admitted_cycle;")
    assert ar.index("admitted_cycle = cycle_cnt + 1;") < ar.index(
        "file_master.drv.send_ar") < ar.index("srcq_r_sum += cycle_cnt - admitted_cycle;")
    assert aw.index("stimulus_start_cycle_o = admitted_cycle;") < aw.index(
        "file_master.drv.send_aw")
    assert ar.index("stimulus_start_cycle_o = admitted_cycle;") < ar.index(
        "file_master.drv.send_ar")
