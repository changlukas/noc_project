// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Ready/valid adapter for the production Gray-pointer CDC FIFO primitive. */
module axi_async_fifo #(
    parameter int unsigned AXI_FIFO_DEPTH = 8,
    parameter type         data_t         = logic
) (
    input  wire logic   src_clk_i,
    input  wire logic   src_rst_ni,
    input  wire logic   src_valid_i,
    output wire logic   src_ready_o,
    input  wire data_t  src_data_i,
    input  wire logic   dst_clk_i,
    input  wire logic   dst_rst_ni,
    output wire logic   dst_valid_o,
    input  wire logic   dst_ready_i,
    output wire data_t  dst_data_o
);

    localparam int unsigned FIFO_ADDR_W = $clog2(AXI_FIFO_DEPTH);

    if (AXI_FIFO_DEPTH < 2 || (AXI_FIFO_DEPTH & (AXI_FIFO_DEPTH - 1)) != 0) begin : gen_invalid_depth
        initial $fatal(0, "Error: AXI_FIFO_DEPTH must be a power of two and at least 2 (instance %m)");
    end

    cc_cdc_fifo_gray #(
        .data_t     (data_t     ),
        .LogDepth   (FIFO_ADDR_W),
        .SyncStages (2          )
    ) i_cc_cdc_fifo_gray (
        .src_rst_ni,
        .src_clk_i,
        .src_data_i,
        .src_valid_i,
        .src_ready_o,
        .dst_rst_ni,
        .dst_clk_i,
        .dst_data_o,
        .dst_valid_o,
        .dst_ready_i
    );

endmodule

`resetall
