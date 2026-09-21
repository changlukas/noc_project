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

    axi_if #(.ID_W(3), .ADDR_W(48), .DATA_W(512), .AWUSER_W(58)) axi();
    assign axi.awid = s_aw_i.awid;
    assign axi.awaddr = s_aw_i.awaddr;
    assign axi.awlen = s_aw_i.awlen;
    assign axi.awsize = s_aw_i.awsize;
    assign axi.awburst = s_aw_i.awburst;
    assign axi.awlock = s_aw_i.awlock;
    assign axi.awcache = s_aw_i.awcache;
    assign axi.awprot = s_aw_i.awprot;
    assign axi.awqos = s_aw_i.awqos;
    assign axi.awregion = s_aw_i.awregion;
    assign axi.awuser = s_aw_i.awuser;
    assign axi.awvalid = s_aw_valid_i;
    assign s_aw_ready_o = axi.awready;
    assign axi.wdata = s_w_i.wdata;
    assign axi.wstrb = s_w_i.wstrb;
    assign axi.wlast = s_w_i.wlast;
    assign axi.wvalid = s_w_valid_i;
    assign s_w_ready_o = axi.wready;
    assign axi.arid = s_ar_i.arid;
    assign axi.araddr = s_ar_i.araddr;
    assign axi.arlen = s_ar_i.arlen;
    assign axi.arsize = s_ar_i.arsize;
    assign axi.arburst = s_ar_i.arburst;
    assign axi.arlock = s_ar_i.arlock;
    assign axi.arcache = s_ar_i.arcache;
    assign axi.arprot = s_ar_i.arprot;
    assign axi.arqos = s_ar_i.arqos;
    assign axi.arregion = s_ar_i.arregion;
    assign axi.arvalid = s_ar_valid_i;
    assign s_ar_ready_o = axi.arready;
    assign axi.bready = 1'b1;
    assign axi.rready = 1'b1;

    nmu_request_path #(
        .AXI_ID_WIDTH(3),
        .AXI_FIFO_DEPTH (4),
        .SAM_NUM_RULES (SAM_NUM_RULES), .addr_t (sam_addr_t),
        .sam_mask_sel_t (sam_mask_sel_t), .sam_idx_t (sam_idx_t),
        .sam_rule_t (sam_rule_t), .SAM (SAM)
    ) dut (
        .axi_clk_i, .axi_rst_ni, .noc_clk_i, .noc_rst_ni,
        .axi_wr_i(axi), .axi_rd_i(axi),
        .m_aw_o, .m_aw_valid_o, .m_aw_ready_i,
        .m_w_o, .m_w_valid_o, .m_w_ready_i,
        .m_ar_o, .m_ar_valid_o, .m_ar_ready_i,
        .s_ordered_aw_i('0), .s_ordered_aw_valid_i(1'b0), .s_ordered_aw_ready_o(),
        .s_ordered_w_i('0), .s_ordered_w_valid_i(1'b0), .s_ordered_w_ready_o(),
        .s_ordered_ar_i('0), .s_ordered_ar_valid_i(1'b0), .s_ordered_ar_ready_o(),
        .s_b_i('0), .s_b_valid_i(1'b0), .s_b_ready_o(),
        .s_r_i('0), .s_r_valid_i(1'b0), .s_r_ready_o(),
        .tx_req_valid_o(), .tx_req_flit_o(), .tx_req_ready_i(1'b1),
        .tx_dat_valid_o(), .tx_dat_flit_o(), .tx_dat_crdvalid_i('0)
    );

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
