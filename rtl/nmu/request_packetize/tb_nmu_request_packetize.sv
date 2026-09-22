`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_request_packetize;

    localparam int unsigned DAT_NUM_VC = 2;
    localparam int unsigned ROUTER_VC_DEPTH = 2;

    logic clk = 1'b0;
    logic rst = 1'b1;
    ni_child_types_pkg::nmu_aw_request_t s_aw;
    logic s_aw_valid, s_aw_ready;
    ni_signals_pkg::axi_w_t s_w;
    logic s_w_valid, s_w_ready;
    ni_child_types_pkg::nmu_ar_request_t s_ar;
    logic s_ar_valid, s_ar_ready;
    ni_flit_pkg::req_flit_t m_req;
    logic m_req_valid, m_req_ready;
    ni_flit_pkg::dat_flit_t m_dat;
    logic m_dat_valid;
    logic [DAT_NUM_VC-1:0] dat_credit_return;

    always #5 clk = ~clk;

    nmu_request_inject_tb_dut #(
        .FIFO_DEPTH       (4),
        .DAT_NUM_VC       (DAT_NUM_VC),
        .ROUTER_VC_DEPTH  (ROUTER_VC_DEPTH),
        .SRC_ID           (8'h12),
        .SRC_PORT_ID      (2'h2)
    ) dut (
        .clk_i                  (clk),
        .rst_i                  (rst),
        .s_aw_i                 (s_aw),
        .s_aw_valid_i           (s_aw_valid),
        .s_aw_ready_o           (s_aw_ready),
        .s_w_i                  (s_w),
        .s_w_valid_i            (s_w_valid),
        .s_w_ready_o            (s_w_ready),
        .s_ar_i                 (s_ar),
        .s_ar_valid_i           (s_ar_valid),
        .s_ar_ready_o           (s_ar_ready),
        .m_req_o                (m_req),
        .m_req_valid_o          (m_req_valid),
        .m_req_ready_i          (m_req_ready),
        .m_dat_o                (m_dat),
        .m_dat_valid_o          (m_dat_valid),
        .dat_credit_return_i    (dat_credit_return)
    );

    task automatic push_aw(input logic is_data, input logic [7:0] dst,
                           input logic [2:0] id, input logic [47:0] addr);
        s_aw = '0;
        s_aw.axi.awid = id;
        s_aw.axi.awaddr = addr;
        s_aw.axi.awsize = is_data ? 3'd6 : 3'd3;
        s_aw.axi.awburst = 2'b01;
        s_aw.axi.awuser = ni_params_pkg::AXI_AWUSER_WIDTH_DFLT'(8'h5a);
        s_aw.meta.route.domain = '{dst_id: dst, dst_port_id: 2'h1, is_data: is_data};
        s_aw.user = 8'h5a;
        s_aw_valid = 1'b1;
        do @(posedge clk); while (!s_aw_ready);
        @(negedge clk);
        s_aw_valid = 1'b0;
    endtask

    task automatic push_w(input logic [511:0] data);
        s_w = '0;
        s_w.wdata = data;
        s_w.wstrb = '1;
        s_w.wlast = 1'b1;
        s_w_valid = 1'b1;
        do @(posedge clk); while (!s_w_ready);
        @(negedge clk);
        s_w_valid = 1'b0;
    endtask

    initial begin
        logic [511:0] narrow_data;
        s_aw = '0;
        s_w = '0;
        s_ar = '0;
        s_aw_valid = 1'b0;
        s_w_valid = 1'b0;
        s_ar_valid = 1'b0;
        m_req_ready = 1'b0;
        dat_credit_return = '0;
        repeat (3) @(posedge clk);
        rst = 1'b0;
        @(negedge clk);

        // Queue one write for each physical network.  REQ is stalled until
        // both are ready, proving the two schedulers can transfer together.
        push_aw(1'b0, 8'h21, 3'h1, 48'h18);
        narrow_data = '0;
        narrow_data[3*64 +: 64] = 64'h0123_4567_89ab_cdef;
        push_w(narrow_data);
        push_aw(1'b1, 8'h32, 3'h2, 48'h1000);
        push_w(512'hfeed_face);

        m_req_ready = 1'b1;
        #1;
        assert (m_req_valid && m_dat_valid)
            else $fatal(1, "REQ and DAT AW were not independently available");
        assert (m_req.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB] ==
                ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowAw));
        assert (m_dat.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB] ==
                ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataAw));
        assert (m_req.header[ni_flit_pkg::SRC_ID_MSB:ni_flit_pkg::SRC_ID_LSB] == 8'h12);
        assert (m_dat.header[ni_flit_pkg::DST_ID_MSB:ni_flit_pkg::DST_ID_LSB] == 8'h32);
        @(posedge clk);
        #1;
        assert (m_req_valid && m_dat_valid)
            else $fatal(1, "REQ and DAT W did not transfer in parallel");
        assert (m_req.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB] ==
                ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowW));
        assert (m_dat.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB] ==
                ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataW));
        assert (m_req.payload[ni_flit_pkg::NARROW_W_WDATA_MSB:
                              ni_flit_pkg::NARROW_W_WDATA_LSB] ==
                64'h0123_4567_89ab_cdef)
            else $fatal(1, "narrow W did not extract the AW-addressed lane");
        assert (m_req.header[ni_flit_pkg::FLIT_TAIL_LSB] &&
                m_dat.header[ni_flit_pkg::FLIT_TAIL_LSB]);
        @(posedge clk);

        // A Data-class AR always rides REQ and is a single-flit packet.
        s_ar = '0;
        s_ar.axi.arid = 3'h3;
        s_ar.axi.araddr = 48'h2000;
        s_ar.meta.route.domain = '{dst_id: 8'h43, dst_port_id: 2'h3, is_data: 1'b1};
        s_ar_valid = 1'b1;
        do @(posedge clk); while (!s_ar_ready);
        @(negedge clk);
        s_ar_valid = 1'b0;
        #1;
        assert (m_req_valid);
        assert (m_req.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB] ==
                ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataAr));
        assert (m_req.header[ni_flit_pkg::FLIT_TAIL_LSB]);
        @(posedge clk);

        // The first Data packet consumed both VC0 credits.  A same-domain AW
        // must reuse VC0 and may consume a returned credit in the same cycle.
        @(negedge clk);
        push_aw(1'b1, 8'h32, 3'h2, 48'h1080);
        push_w(512'h1234_5678);
        #1;
        assert (!m_dat_valid)
            else $fatal(1, "fixed VC packet ignored exhausted credit");
        dat_credit_return[0] = 1'b1;
        #1;
        assert (m_dat_valid);
        assert (m_dat.header[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB] == 0);
        @(posedge clk);
        @(negedge clk);
        dat_credit_return = '0;
        #1;
        assert (!m_dat_valid);
        dat_credit_return[0] = 1'b1;
        #1;
        assert (m_dat_valid);
        assert (m_dat.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB] ==
                ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataW));
        @(posedge clk);

        $display("PASS: NMU request packetization and independent REQ/DAT scheduling");
        $finish;
    end

endmodule

`default_nettype wire
