// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Response decode, transaction ordering, and NoC-to-AXI CDC.
module nmu_response_path #(
    parameter int unsigned NOC_DAT_NUM_VC = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned NOC_DAT_VC_MODE = ni_params_pkg::NOC_DAT_VC_MODE_DFLT,
    parameter int unsigned NOC_NI_DAT_RX_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT,
    parameter int unsigned AXI_FIFO_DEPTH = ni_params_pkg::AXI_FIFO_DEPTH_DFLT,
    parameter int unsigned NMU_ROB_B_DEPTH = ni_params_pkg::NMU_ROB_B_DEPTH_DFLT,
    parameter int unsigned NMU_ROB_R_DEPTH = ni_params_pkg::NMU_ROB_R_DEPTH_DFLT,
    parameter bit READ_ROB_ENABLED = bit'(ni_params_pkg::NMU_READ_ROB_ENABLED_DFLT),
    parameter int unsigned NMU_MAX_TXNS_PER_ID = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT
) (
    input wire logic axi_clk_i,
    input wire logic axi_rst_ni,
    input wire logic noc_clk_i,
    input wire logic noc_rst_ni,
    input wire ni_child_types_pkg::nmu_sam_aw_result_t s_aw_i,
    input wire logic s_aw_valid_i,
    output wire logic s_aw_ready_o,
    input wire ni_signals_pkg::axi_w_t s_w_i,
    input wire logic s_w_valid_i,
    output wire logic s_w_ready_o,
    input wire ni_child_types_pkg::nmu_sam_ar_result_t s_ar_i,
    input wire logic s_ar_valid_i,
    output wire logic s_ar_ready_o,
    output wire ni_child_types_pkg::nmu_aw_request_t m_ordered_aw_o,
    output wire logic m_ordered_aw_valid_o,
    input wire logic m_ordered_aw_ready_i,
    output wire ni_signals_pkg::axi_w_t m_ordered_w_o,
    output wire logic m_ordered_w_valid_o,
    input wire logic m_ordered_w_ready_i,
    output wire ni_child_types_pkg::nmu_ar_request_t m_ordered_ar_o,
    output wire logic m_ordered_ar_valid_o,
    input wire logic m_ordered_ar_ready_i,
    output wire ni_signals_pkg::axi_b_t m_b_o,
    output wire logic m_b_valid_o,
    input wire logic m_b_ready_i,
    output wire ni_signals_pkg::axi_r_t m_r_o,
    output wire logic m_r_valid_o,
    input wire logic m_r_ready_i,
    input wire logic rx_rsp_valid_i,
    input wire logic [ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT-1:0] rx_rsp_flit_i,
    output wire logic rx_rsp_ready_o,
    input wire logic rx_dat_valid_i,
    input wire logic [ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT-1:0] rx_dat_flit_i,
    output wire logic [NOC_DAT_NUM_VC-1:0] rx_dat_crdvalid_o
);

    ni_child_types_pkg::nmu_b_response_t decoded_b;
    ni_child_types_pkg::nmu_r_response_t decoded_r;
    ni_signals_pkg::axi_b_t ordered_b;
    ni_signals_pkg::axi_r_t ordered_r;
    wire decoded_b_valid, decoded_b_ready, decoded_r_valid, decoded_r_ready;
    wire ordered_b_valid, ordered_b_ready, ordered_r_valid, ordered_r_ready;
    nmu_ordering #(
        .NMU_ROB_B_DEPTH(NMU_ROB_B_DEPTH), .NMU_ROB_R_DEPTH(NMU_ROB_R_DEPTH),
        .NMU_MAX_TXNS_PER_ID(NMU_MAX_TXNS_PER_ID), .READ_ROB_ENABLED(READ_ROB_ENABLED)
    ) i_ordering (
        .clk_i(noc_clk_i), .rst_i(!noc_rst_ni),
        .s_aw_i(s_aw_i), .s_aw_valid_i(s_aw_valid_i), .s_aw_ready_o(s_aw_ready_o),
        .m_aw_o(m_ordered_aw_o), .m_aw_valid_o(m_ordered_aw_valid_o), .m_aw_ready_i(m_ordered_aw_ready_i),
        .s_w_i(s_w_i), .s_w_valid_i(s_w_valid_i), .s_w_ready_o(s_w_ready_o),
        .m_w_o(m_ordered_w_o), .m_w_valid_o(m_ordered_w_valid_o), .m_w_ready_i(m_ordered_w_ready_i),
        .s_ar_i(s_ar_i), .s_ar_valid_i(s_ar_valid_i), .s_ar_ready_o(s_ar_ready_o),
        .m_ar_o(m_ordered_ar_o), .m_ar_valid_o(m_ordered_ar_valid_o), .m_ar_ready_i(m_ordered_ar_ready_i),
        .s_b_i(decoded_b), .s_b_valid_i(decoded_b_valid), .s_b_ready_o(decoded_b_ready),
        .m_b_o(ordered_b), .m_b_valid_o(ordered_b_valid), .m_b_ready_i(ordered_b_ready),
        .s_r_i(decoded_r), .s_r_valid_i(decoded_r_valid), .s_r_ready_o(decoded_r_ready),
        .m_r_o(ordered_r), .m_r_valid_o(ordered_r_valid), .m_r_ready_i(ordered_r_ready)
    );
    nmu_response_depacketize #(
        .DAT_NUM_VC(NOC_DAT_NUM_VC), .DAT_VC_MODE(NOC_DAT_VC_MODE),
        .DAT_RX_VC_DEPTH(NOC_NI_DAT_RX_VC_DEPTH)
    ) i_depacketize (
        .clk_i(noc_clk_i), .rst_i(!noc_rst_ni),
        .s_dat_i(ni_flit_pkg::dat_flit_t'(rx_dat_flit_i)),
        .s_dat_valid_i(rx_dat_valid_i), .dat_credit_return_o(rx_dat_crdvalid_o),
        .s_rsp_i(ni_flit_pkg::rsp_flit_t'(rx_rsp_flit_i)),
        .s_rsp_valid_i(rx_rsp_valid_i), .s_rsp_ready_o(rx_rsp_ready_o),
        .m_b_o(decoded_b), .m_b_valid_o(decoded_b_valid), .m_b_ready_i(decoded_b_ready),
        .m_r_o(decoded_r), .m_r_valid_o(decoded_r_valid), .m_r_ready_i(decoded_r_ready)
    );
    nmu_response_fifo #(
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH),
        .B_T (ni_signals_pkg::axi_b_t),
        .R_T (ni_signals_pkg::axi_r_t)
    ) i_response_fifo (
        .noc_clk_i, .noc_rst_ni, .axi_clk_i, .axi_rst_ni,
        .s_b_data_i (ordered_b), .s_b_valid_i (ordered_b_valid), .s_b_ready_o (ordered_b_ready),
        .m_b_data_o (m_b_o), .m_b_valid_o, .m_b_ready_i,
        .s_r_data_i (ordered_r), .s_r_valid_i (ordered_r_valid), .s_r_ready_o (ordered_r_ready),
        .m_r_data_o (m_r_o), .m_r_valid_o, .m_r_ready_i
    );
endmodule

`resetall
