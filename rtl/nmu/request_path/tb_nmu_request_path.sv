`timescale 1ns / 1ps

module tb_nmu_request_path;
    import topology_pkg::*;

    logic axi_clk_i = 0, noc_clk_i = 0;
    logic axi_rst_ni = 0, noc_rst_ni = 0;
    ni_signals_pkg::axi_aw_t s_aw_i;
    ni_signals_pkg::axi_w_t s_w_i, m_w_o;
    ni_signals_pkg::axi_ar_t s_ar_i;
    ni_child_types_pkg::nmu_sam_aw_result_t m_aw_o;
    ni_child_types_pkg::nmu_sam_ar_result_t m_ar_o;
    logic s_aw_valid_i, s_aw_ready_o, s_w_valid_i, s_w_ready_o;
    logic s_ar_valid_i, s_ar_ready_o;
    logic m_aw_valid_o, m_aw_ready_i = 1, m_w_valid_o, m_w_ready_i = 1;
    logic m_ar_valid_o, m_ar_ready_i = 1;

    nmu_request_path #(
        .AXI_FIFO_DEPTH (4),
        .SAM_NUM_RULES (SAM_NUM_RULES), .addr_t (sam_addr_t),
        .sam_mask_sel_t (sam_mask_sel_t), .sam_idx_t (sam_idx_t),
        .sam_rule_t (sam_rule_t), .SAM (SAM)
    ) dut (.*);

    always #3ns axi_clk_i = !axi_clk_i;
    always #5ns noc_clk_i = !noc_clk_i;

    initial begin
        s_aw_i = '0; s_w_i = '0; s_ar_i = '0;
        s_aw_valid_i = 0; s_w_valid_i = 0; s_ar_valid_i = 0;
        repeat (3) @(posedge axi_clk_i);
        axi_rst_ni = 1;
        repeat (3) @(posedge noc_clk_i);
        noc_rst_ni = 1;
        s_aw_i.awaddr = 48'h0000_01ff_ffe0;
        s_aw_i.awsize = 3'd3; s_aw_i.awlen = 8'd1; s_aw_i.awburst = 2'd1;
        s_w_i.wdata = 'h1234; s_w_i.wlast = 1;
        s_ar_i.araddr = 48'h0000_0200_0fe0;
        s_ar_i.arsize = 3'd3; s_ar_i.arlen = 8'd1; s_ar_i.arburst = 2'd1;
        s_aw_valid_i = 1; s_w_valid_i = 1; s_ar_valid_i = 1;
        fork
            begin do @(posedge axi_clk_i); while (!s_aw_ready_o); s_aw_valid_i = 0; end
            begin do @(posedge axi_clk_i); while (!s_w_ready_o); s_w_valid_i = 0; end
            begin do @(posedge axi_clk_i); while (!s_ar_ready_o); s_ar_valid_i = 0; end
        join
        fork
            begin do @(posedge noc_clk_i); while (!m_aw_valid_o); if (m_aw_o.axi != s_aw_i) $fatal(1, "AW changed across request path"); end
            begin do @(posedge noc_clk_i); while (!m_w_valid_o); if (m_w_o != s_w_i) $fatal(1, "W changed across request path"); end
            begin do @(posedge noc_clk_i); while (!m_ar_valid_o); if (m_ar_o.axi != s_ar_i) $fatal(1, "AR changed across request path"); end
        join
        $finish;
    end

    initial begin
        #10us;
        $fatal(1, "request path integration timeout");
    end
endmodule
