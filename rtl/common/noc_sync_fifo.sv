// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Ready/valid adapter for the production synchronous FIFO primitive. */
module noc_sync_fifo #(
    // NoC class FIFO depth in entries.
    parameter int unsigned NOC_FIFO_DEPTH = 8,
    // Complete transaction record type.
    parameter type T = logic
) (
    input  wire logic  clk_i,
    input  wire logic  rst_ni,
    input  wire logic  s_valid_i,
    output wire logic  s_ready_o,
    input  wire T      s_data_i,
    output wire logic  m_valid_o,
    input  wire logic  m_ready_i,
    output wire T      m_data_o
);

    localparam int unsigned CL_NOC_FIFO_DEPTH =
        NOC_FIFO_DEPTH > 1 ? $clog2(NOC_FIFO_DEPTH) : 1;

    if (NOC_FIFO_DEPTH != 4 && NOC_FIFO_DEPTH != 8 && NOC_FIFO_DEPTH != 16) begin : gen_invalid_depth
        initial $fatal(0, "Error: NOC_FIFO_DEPTH must be 4, 8, or 16 (instance %m)");
    end

    logic fifo_full;
    logic fifo_empty;
    logic [CL_NOC_FIFO_DEPTH-1:0] fifo_usage;

    fifo_v3 #(
        .FALL_THROUGH ( 1'b0           ),
        .DEPTH        ( NOC_FIFO_DEPTH ),
        .dtype        ( T              )
    ) i_fifo_v3 (
        .clk_i,
        .rst_ni,
        .flush_i    ( 1'b0                 ),
        .testmode_i ( 1'b0                 ),
        .full_o     ( fifo_full            ),
        .empty_o    ( fifo_empty           ),
        .usage_o    ( fifo_usage           ),
        .data_i     ( s_data_i             ),
        .push_i     ( s_valid_i & s_ready_o ),
        .data_o     ( m_data_o             ),
        .pop_i      ( m_valid_o & m_ready_i )
    );

    // The non-fall-through primitive owns all queue state.
    assign s_ready_o = !fifo_full;
    assign m_valid_o = !fifo_empty;

endmodule

`resetall
