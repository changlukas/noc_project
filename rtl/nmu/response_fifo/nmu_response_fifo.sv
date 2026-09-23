// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Independent NoC-clock to AXI-clock response-channel CDC FIFO bank. */
module nmu_response_fifo #(
    parameter int unsigned AXI_FIFO_DEPTH = ni_params_pkg::AXI_FIFO_DEPTH,
    parameter type         b_t            = ni_signals_pkg::axi_b_t,
    parameter type         r_t            = ni_signals_pkg::axi_r_t
) (
    input  wire logic  noc_clk_i,
    input  wire logic  noc_rst_ni,
    input  wire logic  axi_clk_i,
    input  wire logic  axi_rst_ni,
    input  wire b_t    s_b_data_i,
    input  wire logic  s_b_valid_i,
    output wire logic  s_b_ready_o,
    output wire b_t    m_b_data_o,
    output wire logic  m_b_valid_o,
    input  wire logic  m_b_ready_i,
    input  wire r_t    s_r_data_i,
    input  wire logic  s_r_valid_i,
    output wire logic  s_r_ready_o,
    output wire r_t    m_r_data_o,
    output wire logic  m_r_valid_o,
    input  wire logic  m_r_ready_i
);

    if (AXI_FIFO_DEPTH < 2 || (AXI_FIFO_DEPTH & (AXI_FIFO_DEPTH - 1)) != 0) begin : gen_invalid_depth
        initial $fatal(0, "Error: AXI_FIFO_DEPTH must be a power of two and at least 2 (instance %m)");
    end

    axi_async_fifo #(
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH),
        .data_t         (b_t           )
    ) i_b_fifo (
        .src_clk_i   (noc_clk_i  ),
        .src_rst_ni  (noc_rst_ni ),
        .src_valid_i (s_b_valid_i),
        .src_ready_o (s_b_ready_o),
        .src_data_i  (s_b_data_i ),
        .dst_clk_i   (axi_clk_i  ),
        .dst_rst_ni  (axi_rst_ni ),
        .dst_valid_o (m_b_valid_o),
        .dst_ready_i (m_b_ready_i),
        .dst_data_o  (m_b_data_o )
    );

    axi_async_fifo #(
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH),
        .data_t         (r_t           )
    ) i_r_fifo (
        .src_clk_i   (noc_clk_i  ),
        .src_rst_ni  (noc_rst_ni ),
        .src_valid_i (s_r_valid_i),
        .src_ready_o (s_r_ready_o),
        .src_data_i  (s_r_data_i ),
        .dst_clk_i   (axi_clk_i  ),
        .dst_rst_ni  (axi_rst_ni ),
        .dst_valid_o (m_r_valid_o),
        .dst_ready_i (m_r_ready_i),
        .dst_data_o  (m_r_data_o )
    );
endmodule

`resetall
