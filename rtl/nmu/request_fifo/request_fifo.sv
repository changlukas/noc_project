// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* AXI-clock to NoC-clock request-channel FIFO bank. */
module nmu_request_fifo #(
    parameter int unsigned AXI_FIFO_DEPTH = 32,
    parameter int unsigned AW_FIFO_DEPTH  = AXI_FIFO_DEPTH,
    parameter int unsigned W_FIFO_DEPTH   = AXI_FIFO_DEPTH,
    parameter int unsigned AR_FIFO_DEPTH  = AXI_FIFO_DEPTH,
    // External AXI ID width; request records retain this width across CDC.
    parameter int unsigned AXI_ID_WIDTH   = 3,
    parameter type         aw_t           = logic [AXI_ID_WIDTH-1:0],
    parameter type         w_t            = logic,
    parameter type         ar_t           = logic [AXI_ID_WIDTH-1:0]
) (
    input  wire logic axi_clk_i,
    input  wire logic axi_rst_n_i,
    input  wire logic noc_clk_i,
    input  wire logic noc_rst_n_i,

    input  wire logic s_aw_valid_i,
    output wire logic s_aw_ready_o,
    input  wire aw_t  s_aw_data_i,
    output wire logic m_aw_valid_o,
    input  wire logic m_aw_ready_i,
    output wire aw_t  m_aw_data_o,

    input  wire logic s_w_valid_i,
    output wire logic s_w_ready_o,
    input  wire w_t   s_w_data_i,
    output wire logic m_w_valid_o,
    input  wire logic m_w_ready_i,
    output wire w_t   m_w_data_o,

    input  wire logic s_ar_valid_i,
    output wire logic s_ar_ready_o,
    input  wire ar_t  s_ar_data_i,
    output wire logic m_ar_valid_o,
    input  wire logic m_ar_ready_i,
    output wire ar_t  m_ar_data_o
);

    if (AXI_FIFO_DEPTH < 2 || (AXI_FIFO_DEPTH & (AXI_FIFO_DEPTH - 1)) != 0) begin : gen_invalid_depth
        initial $fatal(0, "Error: AXI_FIFO_DEPTH must be a power of two and at least 2 (instance %m)");
    end

    if (AXI_ID_WIDTH < 1 || AXI_ID_WIDTH > 8) begin : gen_invalid_axi_id_width
        initial $fatal(0, "Error: AXI_ID_WIDTH must be between 1 and 8 (instance %m)");
    end

    axi_async_fifo #(
        .AXI_FIFO_DEPTH (AW_FIFO_DEPTH),
        .data_t         (aw_t         )
    ) i_aw_fifo (
        .src_clk_i   (axi_clk_i   ),
        .src_rst_n_i (axi_rst_n_i ),
        .src_valid_i (s_aw_valid_i),
        .src_ready_o (s_aw_ready_o),
        .src_data_i  (s_aw_data_i ),
        .dst_clk_i   (noc_clk_i   ),
        .dst_rst_n_i (noc_rst_n_i ),
        .dst_valid_o (m_aw_valid_o),
        .dst_ready_i (m_aw_ready_i),
        .dst_data_o  (m_aw_data_o )
    );

    axi_async_fifo #(
        .AXI_FIFO_DEPTH (W_FIFO_DEPTH),
        .data_t         (w_t         )
    ) i_w_fifo (
        .src_clk_i   (axi_clk_i  ),
        .src_rst_n_i (axi_rst_n_i),
        .src_valid_i (s_w_valid_i),
        .src_ready_o (s_w_ready_o),
        .src_data_i  (s_w_data_i ),
        .dst_clk_i   (noc_clk_i  ),
        .dst_rst_n_i (noc_rst_n_i),
        .dst_valid_o (m_w_valid_o),
        .dst_ready_i (m_w_ready_i),
        .dst_data_o  (m_w_data_o )
    );

    axi_async_fifo #(
        .AXI_FIFO_DEPTH (AR_FIFO_DEPTH),
        .data_t         (ar_t         )
    ) i_ar_fifo (
        .src_clk_i   (axi_clk_i   ),
        .src_rst_n_i (axi_rst_n_i ),
        .src_valid_i (s_ar_valid_i),
        .src_ready_o (s_ar_ready_o),
        .src_data_i  (s_ar_data_i ),
        .dst_clk_i   (noc_clk_i   ),
        .dst_rst_n_i (noc_rst_n_i ),
        .dst_valid_o (m_ar_valid_o),
        .dst_ready_i (m_ar_ready_i),
        .dst_data_o  (m_ar_data_o )
    );

endmodule

`resetall
