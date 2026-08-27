// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* NMU request datapath sub-top. It owns the AXI-to-NoC CDC boundary and
 * address-map lookup; ordering and packetization attach to its outputs.
 */
module nmu_request_path #(
    parameter int unsigned AXI_FIFO_DEPTH = ni_params_pkg::AXI_FIFO_DEPTH_DFLT,
    parameter int unsigned AXI_ID_WIDTH = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned AW_SAM_REG_TYPE = 0,
    parameter int unsigned AR_SAM_REG_TYPE = 0,
    parameter int unsigned SAM_NUM_RULES,
    parameter type addr_t,
    parameter type sam_mask_sel_t,
    parameter type sam_idx_t,
    parameter type sam_rule_t,
    parameter sam_rule_t [SAM_NUM_RULES-1:0] SAM
) (
    input wire logic axi_clk_i,
    input wire logic axi_rst_ni,
    input wire logic noc_clk_i,
    input wire logic noc_rst_ni,
    input wire ni_signals_pkg::axi_aw_t s_aw_i,
    input wire logic s_aw_valid_i,
    output wire logic s_aw_ready_o,
    input wire ni_signals_pkg::axi_w_t s_w_i,
    input wire logic s_w_valid_i,
    output wire logic s_w_ready_o,
    input wire ni_signals_pkg::axi_ar_t s_ar_i,
    input wire logic s_ar_valid_i,
    output wire logic s_ar_ready_o,
    output wire ni_child_types_pkg::nmu_sam_aw_result_t m_aw_o,
    output wire logic m_aw_valid_o,
    input wire logic m_aw_ready_i,
    output wire ni_signals_pkg::axi_w_t m_w_o,
    output wire logic m_w_valid_o,
    input wire logic m_w_ready_i,
    output wire ni_child_types_pkg::nmu_sam_ar_result_t m_ar_o,
    output wire logic m_ar_valid_o,
    input wire logic m_ar_ready_i
);

    wire ni_signals_pkg::axi_aw_t fifo_aw;
    wire logic fifo_aw_valid;
    wire logic fifo_aw_ready;
    wire ni_signals_pkg::axi_ar_t fifo_ar;
    wire logic fifo_ar_valid;
    wire logic fifo_ar_ready;

    nmu_request_fifo #(
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH),
        .AXI_ID_WIDTH   (AXI_ID_WIDTH),
        .AW_T           (ni_signals_pkg::axi_aw_t),
        .W_T            (ni_signals_pkg::axi_w_t),
        .AR_T           (ni_signals_pkg::axi_ar_t)
    ) i_request_fifo (
        .axi_clk_i, .axi_rst_ni, .noc_clk_i, .noc_rst_ni,
        .s_aw_valid_i, .s_aw_ready_o, .s_aw_data_i (s_aw_i),
        .m_aw_valid_o (fifo_aw_valid), .m_aw_ready_i (fifo_aw_ready),
        .m_aw_data_o (fifo_aw),
        .s_w_valid_i, .s_w_ready_o, .s_w_data_i (s_w_i),
        .m_w_valid_o, .m_w_ready_i, .m_w_data_o (m_w_o),
        .s_ar_valid_i, .s_ar_ready_o, .s_ar_data_i (s_ar_i),
        .m_ar_valid_o (fifo_ar_valid), .m_ar_ready_i (fifo_ar_ready),
        .m_ar_data_o (fifo_ar)
    );

    nmu_sam #(
        .AW_SAM_REG_TYPE (AW_SAM_REG_TYPE),
        .AR_SAM_REG_TYPE (AR_SAM_REG_TYPE),
        .SAM_NUM_RULES   (SAM_NUM_RULES),
        .addr_t          (addr_t),
        .sam_mask_sel_t  (sam_mask_sel_t),
        .sam_idx_t       (sam_idx_t),
        .sam_rule_t      (sam_rule_t),
        .SAM             (SAM)
    ) i_sam (
        .noc_clk_i, .noc_rst_ni,
        .s_aw_valid_i (fifo_aw_valid), .s_aw_ready_o (fifo_aw_ready),
        .s_aw_i (fifo_aw), .m_aw_valid_o, .m_aw_ready_i, .m_aw_o,
        .s_ar_valid_i (fifo_ar_valid), .s_ar_ready_o (fifo_ar_ready),
        .s_ar_i (fifo_ar), .m_ar_valid_o, .m_ar_ready_i, .m_ar_o
    );

endmodule

`resetall
