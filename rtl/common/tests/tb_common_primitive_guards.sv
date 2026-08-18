`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_common_primitive_guards #(
    parameter int unsigned INVALID_CASE = 0
);

    logic clk_i = 1'b0;
    logic rst_ni = 1'b0;
    logic src_clk_i = 1'b0;
    logic src_rst_ni = 1'b0;
    logic dst_clk_i = 1'b0;
    logic dst_rst_ni = 1'b0;
    logic guard_s_ready;
    logic guard_m_valid;
    logic guard_data;

    if (INVALID_CASE == 0) begin : gen_invalid_noc_fifo_depth
        noc_sync_fifo #(
            .NOC_FIFO_DEPTH ( 3 )
        ) i_noc_sync_fifo (
            .clk_i,
            .rst_ni,
            .s_valid_i ( 1'b0 ),
            .s_ready_o ( guard_s_ready ),
            .s_data_i  ( 1'b0 ),
            .m_valid_o ( guard_m_valid ),
            .m_ready_i ( 1'b0 ),
            .m_data_o  ( guard_data )
        );
    end else if (INVALID_CASE == 1) begin : gen_invalid_axi_fifo_depth
        axi_async_fifo #(
            .AXI_FIFO_DEPTH ( 3 )
        ) i_axi_async_fifo (
            .src_clk_i,
            .src_rst_ni,
            .src_valid_i ( 1'b0 ),
            .src_ready_o ( guard_s_ready ),
            .src_data_i  ( 1'b0 ),
            .dst_clk_i,
            .dst_rst_ni,
            .dst_valid_o ( guard_m_valid ),
            .dst_ready_i ( 1'b0 ),
            .dst_data_o  ( guard_data )
        );
    end else if (INVALID_CASE == 2) begin : gen_invalid_reg_type
        noc_reg_slice #(
            .REG_TYPE ( 3 )
        ) i_noc_reg_slice (
            .clk_i,
            .rst_ni,
            .s_valid_i ( 1'b0 ),
            .s_ready_o ( guard_s_ready ),
            .s_data_i  ( 1'b0 ),
            .m_valid_o ( guard_m_valid ),
            .m_ready_i ( 1'b0 ),
            .m_data_o  ( guard_data )
        );
    end else begin : gen_invalid_test_case
        initial $fatal(0, "Error: INVALID_CASE must select one adapter guard (instance %m)");
    end

    initial begin
        #10ns;
        $finish;
    end

endmodule

`resetall
