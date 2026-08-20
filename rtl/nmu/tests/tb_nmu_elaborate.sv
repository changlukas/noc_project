`timescale 1ns / 1ps

module tb_nmu_elaborate;

    logic ACLK;
    logic ARESETn;
    logic noc_clk;
    logic noc_rst_n;

    axi_if #(
        .DATA_W(ni_params_pkg::AXI_DATA_WIDTH_DFLT),
        .ADDR_W(ni_params_pkg::AXI_ADDR_WIDTH_DFLT),
        .ID_W(8),
        .AWUSER_EN(1'b1),
        .AWUSER_W(ni_params_pkg::AXI_AWUSER_WIDTH_DFLT)
    ) axi_if();

    logic tx_req_valid_o;
    logic [ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT-1:0] tx_req_flit_o;
    logic tx_req_ready_i;

    logic rx_rsp_valid_i;
    logic [ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT-1:0] rx_rsp_flit_i;
    logic rx_rsp_ready_o;

    logic tx_dat_valid_o;
    logic [ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT-1:0] tx_dat_flit_o;
    logic [ni_params_pkg::NOC_DAT_NUM_VC_DFLT-1:0] tx_dat_crdvalid_i;
    logic rx_dat_valid_i;
    logic [ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT-1:0] rx_dat_flit_i;
    logic rx_dat_ready_o;

    nmu #(
        .AXI_ID_WIDTH(8)
    ) dut (
        .ACLK               ( ACLK               ),
        .ARESETn            ( ARESETn            ),
        .noc_clk            ( noc_clk            ),
        .noc_rst_n          ( noc_rst_n          ),
        .axi_wr_i           ( axi_if.wr_slv      ),
        .axi_rd_i           ( axi_if.rd_slv      ),
        .tx_req_valid_o     ( tx_req_valid_o     ),
        .tx_req_flit_o      ( tx_req_flit_o      ),
        .tx_req_ready_i     ( tx_req_ready_i     ),
        .rx_rsp_valid_i     ( rx_rsp_valid_i     ),
        .rx_rsp_flit_i      ( rx_rsp_flit_i      ),
        .rx_rsp_ready_o     ( rx_rsp_ready_o     ),
        .tx_dat_valid_o     ( tx_dat_valid_o     ),
        .tx_dat_flit_o      ( tx_dat_flit_o      ),
        .tx_dat_crdvalid_i  ( tx_dat_crdvalid_i  ),
        .rx_dat_valid_i     ( rx_dat_valid_i     ),
        .rx_dat_flit_i      ( rx_dat_flit_i      ),
        .rx_dat_ready_o     ( rx_dat_ready_o     )
    );

    initial begin
        assert ($bits(axi_if.awid) == 8)
            else $fatal(1, "AXI request ID width mismatch");
        assert ($bits(axi_if.awaddr) == ni_params_pkg::AXI_ADDR_WIDTH_DFLT)
            else $fatal(1, "AXI request address width mismatch");
        assert ($bits(axi_if.wdata) == ni_params_pkg::AXI_DATA_WIDTH_DFLT)
            else $fatal(1, "AXI request data width mismatch");
        assert ($bits(axi_if.awuser) == ni_params_pkg::AXI_AWUSER_WIDTH_DFLT)
            else $fatal(1, "AWUSER width mismatch");
        assert ($bits(tx_req_flit_o) == ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT)
            else $fatal(1, "REQ flit width mismatch");
        assert ($bits(rx_rsp_flit_i) == ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT)
            else $fatal(1, "RSP flit width mismatch");
        assert ($bits(tx_dat_flit_o) == ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT)
            else $fatal(1, "DAT flit width mismatch");
        assert ($bits(tx_dat_crdvalid_i) == ni_params_pkg::NOC_DAT_NUM_VC_DFLT)
            else $fatal(1, "DAT credit width mismatch");
        $finish;
    end

endmodule
