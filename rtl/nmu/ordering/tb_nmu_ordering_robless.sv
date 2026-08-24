`timescale 1ns / 1ps

module tb_nmu_ordering_robless;
    logic clk_i = 0, rst_i = 1;
    ni_child_types_pkg::nmu_sam_aw_result_t s_aw_i;
    ni_child_types_pkg::nmu_aw_request_t m_aw_o;
    ni_signals_pkg::axi_w_t s_w_i, m_w_o;
    ni_child_types_pkg::nmu_sam_ar_result_t s_ar_i;
    ni_child_types_pkg::nmu_ar_request_t m_ar_o;
    ni_child_types_pkg::nmu_b_response_t s_b_i;
    ni_signals_pkg::axi_b_t m_b_o;
    ni_child_types_pkg::nmu_r_response_t s_r_i;
    ni_signals_pkg::axi_r_t m_r_o;
    logic s_aw_valid_i, s_aw_ready_o, m_aw_valid_o, m_aw_ready_i;
    logic s_w_valid_i, s_w_ready_o, m_w_valid_o, m_w_ready_i;
    logic s_ar_valid_i, s_ar_ready_o, m_ar_valid_o, m_ar_ready_i;
    logic s_b_valid_i, s_b_ready_o, m_b_valid_o, m_b_ready_i;
    logic s_r_valid_i, s_r_ready_o, m_r_valid_o, m_r_ready_i;

    nmu_ordering #(
        .NMU_ROB_B_DEPTH (8), .NMU_ROB_R_DEPTH (8),
        .NMU_MAX_TXNS_PER_ID (4), .READ_ROB_ENABLED (1'b0)
    ) dut (.*);

    always #5ns clk_i = !clk_i;

    task automatic send_ar(input int dst);
        s_ar_i = '0; s_ar_i.axi.arid = 3'd1;
        s_ar_i.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(dst);
        s_ar_valid_i = 1;
        do @(posedge clk_i); while (!s_ar_ready_o);
        if (m_ar_o.meta.ordering_req) $fatal(1, "RoB-less AR requested storage");
        s_ar_valid_i = 0;
    endtask

    task automatic send_r;
        s_r_i = '0; s_r_i.axi.rid = 3'd1; s_r_i.axi.rlast = 1;
        s_r_valid_i = 1;
        do @(posedge clk_i); while (!s_r_ready_o);
        s_r_valid_i = 0;
    endtask

    initial begin
        s_aw_i = '0; s_w_i = '0; s_ar_i = '0; s_b_i = '0; s_r_i = '0;
        s_aw_valid_i = 0; s_w_valid_i = 0; s_ar_valid_i = 0;
        s_b_valid_i = 0; s_r_valid_i = 0;
        m_aw_ready_i = 1; m_w_ready_i = 1; m_ar_ready_i = 1;
        m_b_ready_i = 1; m_r_ready_i = 1;
        repeat (3) @(posedge clk_i); rst_i = 0;
        send_ar(1);
        send_ar(1);
        s_ar_i.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(2);
        s_ar_valid_i = 1;
        repeat (3) begin @(posedge clk_i); if (s_ar_ready_o) $fatal(1, "cross-domain RoB-less AR accepted"); end
        s_ar_valid_i = 0;
        send_r(); send_r();
        send_ar(2);
        $finish;
    end

    initial begin #10us; $fatal(1, "RoB-less ordering timeout"); end
endmodule
