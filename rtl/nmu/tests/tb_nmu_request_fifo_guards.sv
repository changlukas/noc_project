`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_request_fifo_guards #(
    parameter int unsigned INVALID_CASE = 0
);

    logic axi_clk_i = 1'b0;
    logic axi_rst_ni = 1'b0;
    logic noc_clk_i = 1'b0;
    logic noc_rst_ni = 1'b0;
    logic s_ready;
    logic m_valid;

    if (INVALID_CASE == 0) begin : gen_invalid_depth
        nmu_request_fifo #(
            .AXI_FIFO_DEPTH ( 3 )
        ) dut (
            .axi_clk_i,
            .axi_rst_ni,
            .noc_clk_i,
            .noc_rst_ni,
            .s_aw_valid_i ( 1'b0 ),
            .s_aw_ready_o ( s_ready ),
            .s_aw_data_i  ( '0 ),
            .m_aw_valid_o ( m_valid ),
            .m_aw_ready_i ( 1'b0 ),
            .m_aw_data_o  ( ),
            .s_w_valid_i  ( 1'b0 ),
            .s_w_ready_o  ( ),
            .s_w_data_i   ( '0 ),
            .m_w_valid_o  ( ),
            .m_w_ready_i  ( 1'b0 ),
            .m_w_data_o   ( ),
            .s_ar_valid_i ( 1'b0 ),
            .s_ar_ready_o ( ),
            .s_ar_data_i  ( '0 ),
            .m_ar_valid_o ( ),
            .m_ar_ready_i ( 1'b0 ),
            .m_ar_data_o  ( )
        );
    end else if (INVALID_CASE == 1) begin : gen_invalid_axi_id_width_low
        nmu_request_fifo #(
            .AXI_ID_WIDTH ( 0 )
        ) dut (
            .axi_clk_i,
            .axi_rst_ni,
            .noc_clk_i,
            .noc_rst_ni,
            .s_aw_valid_i ( 1'b0 ), .s_aw_ready_o ( s_ready ), .s_aw_data_i ( '0 ),
            .m_aw_valid_o ( m_valid ), .m_aw_ready_i ( 1'b0 ), .m_aw_data_o ( ),
            .s_w_valid_i ( 1'b0 ), .s_w_ready_o ( ), .s_w_data_i ( '0 ),
            .m_w_valid_o ( ), .m_w_ready_i ( 1'b0 ), .m_w_data_o ( ),
            .s_ar_valid_i ( 1'b0 ), .s_ar_ready_o ( ), .s_ar_data_i ( '0 ),
            .m_ar_valid_o ( ), .m_ar_ready_i ( 1'b0 ), .m_ar_data_o ( )
        );
    end else if (INVALID_CASE == 2) begin : gen_invalid_axi_id_width_high
        nmu_request_fifo #(
            .AXI_ID_WIDTH ( 9 )
        ) dut (
            .axi_clk_i,
            .axi_rst_ni,
            .noc_clk_i,
            .noc_rst_ni,
            .s_aw_valid_i ( 1'b0 ), .s_aw_ready_o ( s_ready ), .s_aw_data_i ( '0 ),
            .m_aw_valid_o ( m_valid ), .m_aw_ready_i ( 1'b0 ), .m_aw_data_o ( ),
            .s_w_valid_i ( 1'b0 ), .s_w_ready_o ( ), .s_w_data_i ( '0 ),
            .m_w_valid_o ( ), .m_w_ready_i ( 1'b0 ), .m_w_data_o ( ),
            .s_ar_valid_i ( 1'b0 ), .s_ar_ready_o ( ), .s_ar_data_i ( '0 ),
            .m_ar_valid_o ( ), .m_ar_ready_i ( 1'b0 ), .m_ar_data_o ( )
        );
    end else begin : gen_invalid_case
        initial $fatal(0, "Error: INVALID_CASE must select one request FIFO guard (instance %m)");
    end

    initial begin
        #10ns;
        $finish;
    end

endmodule

`resetall
