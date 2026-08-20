`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_sam;

    import topology_pkg::*;

    localparam int unsigned NUM_MODE_PAIRS = 9;
    localparam int unsigned TRANSACTION_COUNT = 12;
    localparam int unsigned MAX_CYCLES = 100;

    logic clk;
    logic rst_ni;

    logic [NUM_MODE_PAIRS-1:0] s_aw_valid;
    logic [NUM_MODE_PAIRS-1:0] s_aw_ready;
    ni_signals_pkg::axi_aw_t [NUM_MODE_PAIRS-1:0] s_aw;
    logic [NUM_MODE_PAIRS-1:0] m_aw_valid;
    logic [NUM_MODE_PAIRS-1:0] m_aw_ready;
    ni_child_types_pkg::nmu_sam_aw_result_t [NUM_MODE_PAIRS-1:0] m_aw;

    logic [NUM_MODE_PAIRS-1:0] s_ar_valid;
    logic [NUM_MODE_PAIRS-1:0] s_ar_ready;
    ni_signals_pkg::axi_ar_t [NUM_MODE_PAIRS-1:0] s_ar;
    logic [NUM_MODE_PAIRS-1:0] m_ar_valid;
    logic [NUM_MODE_PAIRS-1:0] m_ar_ready;
    ni_child_types_pkg::nmu_sam_ar_result_t [NUM_MODE_PAIRS-1:0] m_ar;

    logic [NUM_MODE_PAIRS-1:0] sampled_s_aw_ready;
    logic [NUM_MODE_PAIRS-1:0] sampled_m_aw_valid;
    logic [NUM_MODE_PAIRS-1:0] sampled_m_aw_ready;
    ni_child_types_pkg::nmu_sam_aw_result_t [NUM_MODE_PAIRS-1:0] sampled_m_aw;
    logic [NUM_MODE_PAIRS-1:0] sampled_s_ar_ready;
    logic [NUM_MODE_PAIRS-1:0] sampled_m_ar_valid;
    logic [NUM_MODE_PAIRS-1:0] sampled_m_ar_ready;
    ni_child_types_pkg::nmu_sam_ar_result_t [NUM_MODE_PAIRS-1:0] sampled_m_ar;

    for (genvar n = 0; n < NUM_MODE_PAIRS; n++) begin : gen_mode_pairs
        nmu_sam #(
            .AW_SAM_REG_TYPE ( n / 3 ),
            .AR_SAM_REG_TYPE ( n % 3 ),
            .SAM_NUM_RULES   ( SAM_NUM_RULES  ),
            .addr_t          ( sam_addr_t     ),
            .sam_mask_sel_t  ( sam_mask_sel_t ),
            .sam_idx_t       ( sam_idx_t      ),
            .sam_rule_t      ( sam_rule_t     ),
            .SAM             ( SAM            )
        ) dut (
            .noc_clk_i       ( clk            ),
            .noc_rst_ni      ( rst_ni         ),
            .s_aw_valid_i    ( s_aw_valid[n]  ),
            .s_aw_ready_o    ( s_aw_ready[n]  ),
            .s_aw_i          ( s_aw[n]        ),
            .m_aw_valid_o    ( m_aw_valid[n]  ),
            .m_aw_ready_i    ( m_aw_ready[n]  ),
            .m_aw_o          ( m_aw[n]        ),
            .s_ar_valid_i    ( s_ar_valid[n]  ),
            .s_ar_ready_o    ( s_ar_ready[n]  ),
            .s_ar_i          ( s_ar[n]        ),
            .m_ar_valid_o    ( m_ar_valid[n]  ),
            .m_ar_ready_i    ( m_ar_ready[n]  ),
            .m_ar_o          ( m_ar[n]        )
        );
    end

    // verilator lint_off BLKSEQ
    always #5ns clk = !clk;
    // verilator lint_on BLKSEQ

    always_ff @(posedge clk) begin
        sampled_s_aw_ready <= s_aw_ready;
        sampled_m_aw_valid <= m_aw_valid;
        sampled_m_aw_ready <= m_aw_ready;
        sampled_m_aw <= m_aw;
        sampled_s_ar_ready <= s_ar_ready;
        sampled_m_ar_valid <= m_ar_valid;
        sampled_m_ar_ready <= m_ar_ready;
        sampled_m_ar <= m_ar;
    end

    function automatic ni_signals_pkg::axi_aw_t make_aw(input int unsigned index);
        ni_signals_pkg::axi_aw_t value;

        value = '0;
        value.awid = index[ni_params_pkg::NOC_ID_WIDTH_DFLT-1:0];
        value.awlen = index[0] ? 8'd3 : 8'd1;
        value.awsize = 3'd3;
        value.awburst = 2'(index % 3);
        value.awuser[7:0] = 8'h80 + 8'(index);
        value.awaddr = 48'h0000_01ff_ffe0;
        if (!index[0]) begin
            value.awuser[9:8] = 2'd1;
            value.awuser[57:10] = 48'h0003_0000_0000;
        end
        return value;
    endfunction

    function automatic ni_signals_pkg::axi_ar_t make_ar(input int unsigned index);
        ni_signals_pkg::axi_ar_t value;

        value = '0;
        value.arid = index[ni_params_pkg::NOC_ID_WIDTH_DFLT-1:0];
        value.arlen = index[0] ? 8'd3 : 8'd1;
        value.arsize = 3'd3;
        value.arburst = 2'(index % 3);
        value.araddr = 48'h0000_0200_0fe0;
        return value;
    endfunction

    task automatic drive_aw(input int unsigned n, input int unsigned index);
        ni_signals_pkg::axi_aw_t value;

        value = make_aw(index);
        s_aw[n].awid = value.awid;
        s_aw[n].awaddr = value.awaddr;
        s_aw[n].awlen = value.awlen;
        s_aw[n].awsize = value.awsize;
        s_aw[n].awburst = value.awburst;
        s_aw[n].awcache = value.awcache;
        s_aw[n].awlock = value.awlock;
        s_aw[n].awprot = value.awprot;
        s_aw[n].awregion = value.awregion;
        s_aw[n].awqos = value.awqos;
        s_aw[n].awuser = value.awuser;
    endtask

    task automatic drive_ar(input int unsigned n, input int unsigned index);
        ni_signals_pkg::axi_ar_t value;

        value = make_ar(index);
        s_ar[n].arid = value.arid;
        s_ar[n].araddr = value.araddr;
        s_ar[n].arlen = value.arlen;
        s_ar[n].arsize = value.arsize;
        s_ar[n].arburst = value.arburst;
        s_ar[n].arcache = value.arcache;
        s_ar[n].arlock = value.arlock;
        s_ar[n].arprot = value.arprot;
        s_ar[n].arregion = value.arregion;
        s_ar[n].arqos = value.arqos;
    endtask

    function automatic ni_child_types_pkg::nmu_sam_aw_result_t expected_aw(
        input int unsigned index
    );
        ni_child_types_pkg::nmu_sam_aw_result_t value;
        ni_signals_pkg::axi_aw_t input_value;
        int unsigned rule_index;

        value = '0;
        input_value = make_aw(index);
        rule_index = 7;
        value.axi = input_value;
        value.route.route.domain.dst_id = SAM[rule_index].idx.dst_id;
        value.route.route.domain.dst_port_id = SAM[rule_index].idx.dst_port_id;
        value.route.route.domain.is_data = SAM[rule_index].idx.is_data;
        value.route.user = input_value.awuser[7:0];
        value.route.collective_op = input_value.awuser[9:8];
        value.route.collective_mask = input_value.awuser[9:8] == 2'd1 ? 8'h11 : '0;
        return value;
    endfunction

    function automatic ni_child_types_pkg::nmu_sam_ar_result_t expected_ar(
        input int unsigned index
    );
        ni_child_types_pkg::nmu_sam_ar_result_t value;
        int unsigned rule_index;

        value = '0;
        rule_index = 3;
        value.axi = make_ar(index);
        value.route.domain.dst_id = SAM[rule_index].idx.dst_id;
        value.route.domain.dst_port_id = SAM[rule_index].idx.dst_port_id;
        value.route.domain.is_data = SAM[rule_index].idx.is_data;
        return value;
    endfunction

    initial begin
        int unsigned cycle_count;
        int unsigned aw_sent [NUM_MODE_PAIRS];
        int unsigned aw_received [NUM_MODE_PAIRS];
        int unsigned ar_sent [NUM_MODE_PAIRS];
        int unsigned ar_received [NUM_MODE_PAIRS];
        logic [NUM_MODE_PAIRS-1:0] aw_was_stalled;
        logic [NUM_MODE_PAIRS-1:0] ar_was_stalled;
        ni_child_types_pkg::nmu_sam_aw_result_t [NUM_MODE_PAIRS-1:0] stalled_aw;
        ni_child_types_pkg::nmu_sam_ar_result_t [NUM_MODE_PAIRS-1:0] stalled_ar;
        logic all_done;

        clk = 1'b0;
        rst_ni = 1'b0;
        s_aw_valid = '1;
        s_ar_valid = '1;
        m_aw_ready = '1;
        m_ar_ready = '1;
        aw_was_stalled = '0;
        ar_was_stalled = '0;
        stalled_aw = '{default: '0};
        stalled_ar = '{default: '0};
        cycle_count = 0;
        for (int unsigned n = 0; n < NUM_MODE_PAIRS; n++) begin
            aw_sent[n] = 0;
            aw_received[n] = 0;
            ar_sent[n] = 0;
            ar_received[n] = 0;
            drive_aw(n, 0);
            drive_ar(n, 0);
        end

        repeat (3) @(posedge clk);
        #1ps;
        assert (s_aw_ready == '0 && s_ar_ready == '0 && m_aw_valid == '0 && m_ar_valid == '0)
            else $fatal(1, "SAM slice exposed a handshake or output valid during reset");

        @(negedge clk);
        rst_ni = 1'b1;

        while (cycle_count < MAX_CYCLES) begin
            @(posedge clk);
            #1ps;

            for (int unsigned n = 0; n < NUM_MODE_PAIRS; n++) begin
                if (aw_was_stalled[n]) begin
                    assert (sampled_m_aw_valid[n] && sampled_m_aw[n] == stalled_aw[n])
                        else $fatal(1, "AW mode pair %0d changed a stalled transaction", n);
                end
                if (ar_was_stalled[n]) begin
                    assert (sampled_m_ar_valid[n] && sampled_m_ar[n] == stalled_ar[n])
                        else $fatal(1, "AR mode pair %0d changed a stalled transaction", n);
                end

                if (sampled_m_aw_valid[n] && sampled_m_aw_ready[n]) begin
                    if (aw_received[n] >= TRANSACTION_COUNT ||
                            sampled_m_aw[n] != expected_aw(aw_received[n])) begin
                        $fatal(1, "AW mode pair %0d changed transaction %0d: got=%h expected=%h",
                            n, aw_received[n], sampled_m_aw[n], expected_aw(aw_received[n]));
                    end
                    aw_received[n]++;
                end
                if (sampled_m_ar_valid[n] && sampled_m_ar_ready[n]) begin
                    if (ar_received[n] >= TRANSACTION_COUNT ||
                            sampled_m_ar[n] != expected_ar(ar_received[n])) begin
                        $fatal(1, "AR mode pair %0d changed transaction %0d: got=%h expected=%h",
                            n, ar_received[n], sampled_m_ar[n], expected_ar(ar_received[n]));
                    end
                    ar_received[n]++;
                end

                if (aw_sent[n] < TRANSACTION_COUNT && sampled_s_aw_ready[n]) begin
                    aw_sent[n]++;
                end
                if (ar_sent[n] < TRANSACTION_COUNT && sampled_s_ar_ready[n]) begin
                    ar_sent[n]++;
                end

                if (cycle_count > 1 && cycle_count < 6 && n / 3 == 2) begin
                    assert (sampled_m_aw_valid[n])
                        else $fatal(1, "AW full-skid mode inserted an avoidable throughput bubble");
                end
                if (cycle_count > 1 && cycle_count < 6 && n % 3 == 2) begin
                    assert (sampled_m_ar_valid[n])
                        else $fatal(1, "AR full-skid mode inserted an avoidable throughput bubble");
                end

                aw_was_stalled[n] = sampled_m_aw_valid[n] && !sampled_m_aw_ready[n];
                ar_was_stalled[n] = sampled_m_ar_valid[n] && !sampled_m_ar_ready[n];
                if (aw_was_stalled[n]) begin
                    stalled_aw[n] = sampled_m_aw[n];
                end
                if (ar_was_stalled[n]) begin
                    stalled_ar[n] = sampled_m_ar[n];
                end
            end

            all_done = 1'b1;
            for (int unsigned n = 0; n < NUM_MODE_PAIRS; n++) begin
                all_done &= aw_received[n] == TRANSACTION_COUNT &&
                    ar_received[n] == TRANSACTION_COUNT;
            end
            if (all_done) begin
                break;
            end

            @(negedge clk);
            cycle_count++;
            for (int unsigned n = 0; n < NUM_MODE_PAIRS; n++) begin
                s_aw_valid[n] = aw_sent[n] < TRANSACTION_COUNT;
                s_ar_valid[n] = ar_sent[n] < TRANSACTION_COUNT;
                drive_aw(n, aw_sent[n]);
                drive_ar(n, ar_sent[n]);
                if (cycle_count < 6) begin
                    m_aw_ready[n] = 1'b1;
                    m_ar_ready[n] = 1'b1;
                end else begin
                    m_aw_ready[n] = (cycle_count + n) % 4 != 0;
                    m_ar_ready[n] = (cycle_count + 2 * n + 1) % 5 != 0;
                end
            end
        end

        assert (cycle_count < MAX_CYCLES)
            else $fatal(1, "Timed out draining all AW/AR mode pairs");
        for (int unsigned n = 0; n < NUM_MODE_PAIRS; n++) begin
            assert (aw_sent[n] == TRANSACTION_COUNT && aw_received[n] == TRANSACTION_COUNT &&
                    ar_sent[n] == TRANSACTION_COUNT && ar_received[n] == TRANSACTION_COUNT)
                else $fatal(1, "Mode pair %0d conservation mismatch: AW %0d/%0d AR %0d/%0d",
                            n, aw_sent[n], aw_received[n], ar_sent[n], ar_received[n]);
        end

        @(negedge clk);
        s_aw_valid = '0;
        s_ar_valid = '0;
        m_aw_ready = '1;
        m_ar_ready = '1;
        @(posedge clk);
        #1ps;
        assert (m_aw_valid == '0 && m_ar_valid == '0)
            else $fatal(1, "SAM slice produced an extra transaction after drain");

        $display("PASS: nmu_sam decode, conservation, and independent timing cuts");
        $finish;
    end

endmodule

`resetall
