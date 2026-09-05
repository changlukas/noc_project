from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ENDPOINT = ROOT / "sim" / "tb" / "test" / "user_node_endpoint.sv"
MAKEFILE = ROOT / "sim" / "verilator" / "Makefile"
TOP = ROOT / "sim" / "tb" / "noc_tb_top.sv"
TOP_GENERATOR = ROOT / "sim" / "tools" / "gen_tb_top.py"
FABRIC = ROOT / "ref_model" / "top" / "noc_fabric.sv"
LINK_MONITOR = ROOT / "sim" / "tb" / "link_perf_monitor.sv"


def test_mode4_uses_bounded_write_admission_without_awready_to_w_dependency():
    source = ENDPOINT.read_text(encoding="utf-8")

    assert '$value$plusargs("source_outstanding_depth=%d", source_outstanding_depth)' in source
    assert "task automatic run_aw_outstanding();" in source
    assert "task automatic run_w_admitted();" in source
    assert "source_aw_admitted - source_b_returned < source_outstanding_depth" in source
    assert "source_aw_admitted += 1;" in source
    assert "source_aw_accepted <= source_aw_accepted + 1;" in source
    assert "source_b_returned <= source_b_returned + 1;" in source
    assert "!(mst_flat_rsp.bvalid && mst_flat_req.bready)" in source

    w_task = source.split("task automatic run_w_admitted();", 1)[1].split("endtask", 1)[0]
    assert "wait (source_aw_admitted > source_w_started);" in w_task
    assert "awready" not in w_task.lower()


def test_mode4_uses_ar_to_rlast_as_the_read_outstanding_window():
    source = ENDPOINT.read_text(encoding="utf-8")

    assert "task automatic run_ar_outstanding();" in source
    assert "source_ar_admitted - source_r_completed < source_outstanding_depth" in source
    assert "source_ar_accepted <= source_ar_accepted + 1;" in source
    assert "source_r_completed <= source_r_completed + 1;" in source
    assert "mst_flat_rsp.rlast" in source
    assert '"injection_mode=4 does not support mixed Read/Write stimulus"' in source

    mode4 = source.split("4: begin", 1)[1].split("end", 1)[0]
    assert "run_ar_outstanding();" in mode4
    assert "file_master.wait_r();" in mode4


def test_mode4_read_prefills_physical_memory_and_scoreboard_before_traffic():
    source = ENDPOINT.read_text(encoding="utf-8")

    assert "task automatic preload_read_memory();" in source
    assert "g_tile_mem[DATA_TARGET].i_mem.i_sim_mem.mem[addr + k]" in source
    assert "scoreboard.preload_byte(addr + k, read_data_pattern(addr + k));" in source
    assert "wait (scoreboard_ready);" in source
    assert '$value$plusargs("read_prefill_fault=%d", read_prefill_fault)' in source


def test_read_checker_requires_rlast_on_the_programmed_final_beat():
    source = ENDPOINT.read_text(encoding="utf-8")

    assert "mst_flat_rsp.rlast != (mcast_rd_beat[rid] == mcast_rd_active[rid].len)" in source
    assert "RLAST mismatch" in source


def test_make_exposes_read_prefill_fault_injection():
    source = MAKEFILE.read_text(encoding="utf-8")

    assert "READ_PREFILL_FAULT ?=" in source
    assert '$(if $(READ_PREFILL_FAULT),"+read_prefill_fault=$(READ_PREFILL_FAULT)")' in source


def test_make_exposes_mode4_as_a_separate_continuous_run_class():
    source = MAKEFILE.read_text(encoding="utf-8")

    assert "$(filter 1 4,$(INJECTION_MODE))" in source
    assert "SOURCE_OUTSTANDING_DEPTH ?=" in source
    assert '"+source_outstanding_depth=$(SOURCE_OUTSTANDING_DEPTH)"' in source
    assert "windowed_$(_TOPO_TAG)_$(_PATTERN_TAG)_od$(SOURCE_OUTSTANDING_DEPTH)_s$(SEED)" in source
    assert "INJECTION_MODE=4 requires SOURCE_OUTSTANDING_DEPTH=<positive integer>" in source
    assert 'if [ "$(INJECTION_MODE)" = "1" ] || [ "$(INJECTION_MODE)" = "4" ]; then' in source
    assert "--source-outstanding-depth $(SOURCE_OUTSTANDING_DEPTH)" in source
    assert "--injection-rate $(if $(_WINDOWED),N/A,$(INJECTION_RATE))" in source
    assert "$(_AI_META_ARGS) $(if $(filter broadcast,$(PATTERN)),--multicast-mode $(MULTICAST_MODE)) || exit 1;" in source


def test_make_selects_one_ai_traffic_direction_for_mode4():
    source = MAKEFILE.read_text(encoding="utf-8")

    assert "TRAFFIC_DIRECTION ?= write" in source
    assert "--direction $(TRAFFIC_DIRECTION)" in source
    assert '"+traffic_direction=$(TRAFFIC_DIRECTION)"' in source
    assert "$(if $(_WINDOWED),--traffic-direction $(TRAFFIC_DIRECTION))" in source
    assert '"+num_reads=$(_NUM_READS)" "+num_writes=$(_NUM_WRITES)"' in source
    assert "Broadcast Read is not supported" in source


def test_make_routes_run_artifacts_through_output_root():
    source = MAKEFILE.read_text(encoding="utf-8")

    assert "OUTPUT_ROOT    ?= output" in source
    assert "RUN_DIR        := $(OUTPUT_ROOT)/$(SIM_TAG)" in source
    assert "mkdir -p $(RUN_DIR)" in source
    assert '"+perf_out=$(RUN_DIR)/perf.json"' in source
    assert "> $(RUN_DIR)/run.log 2>&1" in source
    assert "--log $(RUN_DIR)/run.log" in source
    assert "--out $(RUN_DIR)/result.csv" in source
    assert "output/$(SIM_TAG)" not in source


def test_make_baseline_runs_approved_ai_cells_at_fixed_outstanding_32():
    source = MAKEFILE.read_text(encoding="utf-8")

    assert "sim-outstanding-ai-baseline:" in source
    assert "AI_ROUNDS=16" in source
    assert "STIM_SIZE=6" in source
    assert "BURST_LEN=63" in source
    assert "SOURCE_OUTSTANDING_DEPTH=32" in source
    assert "MAX_TXNS_PER_ID=32" in source
    assert "OUTPUT_ROOT=output/baseline" in source
    assert "broadcast_global" in source
    assert "gather_global" in source
    assert 'if [ "$$pattern" = "broadcast" ]; then directions="write";' in source


def test_make_tradeoff_sweeps_only_the_five_approved_stress_cells():
    source = MAKEFILE.read_text(encoding="utf-8")

    assert "sim-outstanding-ai-tradeoff:" in source
    assert "TRADEOFF_CONFIG_TAG" in source
    assert "broadcast_global_write" in source
    assert "gather_global_read" in source
    assert "many_to_many_read" in source
    assert "SOURCE_OUTSTANDING_DEPTH=32" in source
    assert "MAX_TXNS_PER_ID=32" in source
    assert "OUTPUT_ROOT=output/tradeoff/$(TRADEOFF_CONFIG_TAG)" in source


def test_mode4_traffic_meta_counts_the_selected_direction():
    source = TOP.read_text(encoding="utf-8")

    assert "if (injection_mode == 4) begin" in source
    assert "if (expected_txn_cnt[i] > 0) active_sources++;" in source
    assert "write_bursts += expected_txn_cnt[i];" in source
    assert '"[TrafficMeta] direction=%s axi_initiators=%0d source_requests=%0d"' in source


def test_top_and_generator_report_nsu_meta_buffer_hwm():
    for path in (TOP, TOP_GENERATOR):
        source = path.read_text(encoding="utf-8")
        assert "cmodel_nsu_meta_buffer_hwm" in source
        assert "[NSU_HWM]" in source


def test_mode4_uses_common_start_and_completion_window():
    endpoint = ENDPOINT.read_text(encoding="utf-8")
    top = TOP.read_text(encoding="utf-8")

    assert "source_done_o" in endpoint
    assert "compare_ready_o = 1'b1;" in endpoint
    assert "wait (compare_start_i === 1'b1);" in endpoint
    assert "mst_flat_rsp.bvalid && mst_flat_req.bready" in endpoint
    assert "mst_flat_rsp.rvalid && mst_flat_req.rready && mst_flat_rsp.rlast" in endpoint
    assert "cmodel_perf_begin" in top
    assert "cmodel_perf_end" in top
    assert "[MeasurementWindow] start_cycle=%0d completion_cycle=%0d" in top
    assert "stimulus_start_cycle[i] != measurement_start_cycle" in top
    assert "measurement_end_cycle != round_completion" in top
    assert "repeat (SETTLE_CYCLES)" in top


def test_mode3_cannot_enter_mode4_measurement_lifecycle():
    top = TOP.read_text(encoding="utf-8")

    assert ("else if (injection_mode == 4 && !mode4_start && "
            "all_barrier_ready && any_active_source) begin") in top
    assert ("else if (injection_mode == 4 && !mode4_ended && "
            "all_active_sources_done) begin") in top


def test_mode3_channel_compare_uses_the_same_fixed_source_window():
    endpoint = ENDPOINT.read_text(encoding="utf-8")

    mode3 = endpoint.rsplit("3: begin", 1)[1].split("4: begin", 1)[0]
    assert "+source_outstanding_depth=<positive integer>" in endpoint
    assert "run_aw_outstanding();" in mode3
    assert "run_w_admitted();" in mode3
    assert "run_ar_outstanding();" in mode3


def test_make_passes_channel_round_count_to_mode3_scheduler():
    source = MAKEFILE.read_text(encoding="utf-8")

    assert '"+channel_rounds=$(AI_ROUNDS)"' in source


def test_link_counters_are_gated_by_the_top_measurement_window():
    monitor = LINK_MONITOR.read_text(encoding="utf-8")
    fabric = FABRIC.read_text(encoding="utf-8")
    top = TOP.read_text(encoding="utf-8")

    assert "input logic measure_en" in monitor
    assert "if (!measure_en)" in monitor
    assert "flit_count <= 0;" in monitor
    assert "stall_cyc  <= 0;" in monitor
    assert ".measure_en(measure_en)" in fabric
    assert ".measure_en(perf_measure_en)" in top


def test_fabric_monitors_every_local_injection_and_ejection_resource():
    source = FABRIC.read_text(encoding="utf-8")

    expected = {
        'LINK_NAME($sformatf("req_inject_%0d", i))':
            ".valid(rx_req_valid[i][RP_LOCAL])",
        'LINK_NAME($sformatf("rsp_inject_%0d", i))':
            ".valid(rx_rsp_valid[i][RP_LOCAL])",
        'LINK_NAME($sformatf("dat_inject_%0d", i))':
            ".valid(rx_dat_valid[i][RP_LOCAL])",
        'LINK_NAME($sformatf("req_eject_%0d", i))':
            ".valid(tx_req_valid[i][RP_LOCAL])",
        'LINK_NAME($sformatf("rsp_eject_%0d", i))':
            ".valid(tx_rsp_valid[i][RP_LOCAL])",
        'LINK_NAME($sformatf("dat_eject_%0d", i))':
            ".valid(tx_dat_valid[i][RP_LOCAL])",
    }
    for name, valid in expected.items():
        assert source.count(name) == 1
        assert valid in source
