// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* NMU response datapath sub-top. Depacketization and ordering drive the
 * NoC-domain B/R inputs; this block owns their CDC into the AXI domain.
 */
module nmu_response_path #(
    parameter int unsigned AXI_FIFO_DEPTH = ni_params_pkg::AXI_FIFO_DEPTH_DFLT
) (
    input wire logic noc_clk_i,
    input wire logic noc_rst_ni,
    input wire logic axi_clk_i,
    input wire logic axi_rst_ni,
    input wire ni_signals_pkg::axi_b_t s_b_i,
    input wire logic s_b_valid_i,
    output wire logic s_b_ready_o,
    input wire ni_signals_pkg::axi_r_t s_r_i,
    input wire logic s_r_valid_i,
    output wire logic s_r_ready_o,
    output wire ni_signals_pkg::axi_b_t m_b_o,
    output wire logic m_b_valid_o,
    input wire logic m_b_ready_i,
    output wire ni_signals_pkg::axi_r_t m_r_o,
    output wire logic m_r_valid_o,
    input wire logic m_r_ready_i
);

    nmu_response_fifo #(
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH),
        .B_T (ni_signals_pkg::axi_b_t),
        .R_T (ni_signals_pkg::axi_r_t)
    ) i_response_fifo (
        .noc_clk_i, .noc_rst_ni, .axi_clk_i, .axi_rst_ni,
        .s_b_data_i (s_b_i), .s_b_valid_i, .s_b_ready_o,
        .m_b_data_o (m_b_o), .m_b_valid_o, .m_b_ready_i,
        .s_r_data_i (s_r_i), .s_r_valid_i, .s_r_ready_o,
        .m_r_data_o (m_r_o), .m_r_valid_o, .m_r_ready_i
    );
endmodule

`resetall
