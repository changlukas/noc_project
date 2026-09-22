// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Network Master Unit production top-level interface and parameter contract.
module nmu #(
    parameter int unsigned AXI_ID_WIDTH = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned NOC_ID_WIDTH = ni_params_pkg::NOC_ID_WIDTH_DFLT,
    parameter int unsigned AXI_ADDR_WIDTH = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned AXI_DATA_WIDTH = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    parameter int unsigned AXI_AWUSER_WIDTH = ni_params_pkg::AXI_AWUSER_WIDTH_DFLT,
    parameter int unsigned AXI_FIFO_DEPTH = ni_params_pkg::AXI_FIFO_DEPTH_DFLT,
    parameter int unsigned NOC_DAT_NUM_VC = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned NOC_DAT_VC_MODE = ni_params_pkg::NOC_DAT_VC_MODE_DFLT,
    parameter int unsigned NOC_FIFO_DEPTH = ni_params_pkg::NOC_FIFO_DEPTH_DFLT,
    parameter int unsigned NOC_ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT,
    parameter int unsigned NOC_NI_DAT_RX_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT,
    parameter int unsigned NMU_ROB_B_DEPTH = ni_params_pkg::NMU_ROB_B_DEPTH_DFLT,
    parameter int unsigned NMU_ROB_R_DEPTH = ni_params_pkg::NMU_ROB_R_DEPTH_DFLT,
    parameter bit READ_ROB_ENABLED = bit'(ni_params_pkg::NMU_READ_ROB_ENABLED_DFLT),
    parameter int unsigned NMU_MAX_TXNS_PER_ID = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT,
    parameter int unsigned AW_SAM_REG_TYPE = 0,
    parameter int unsigned AR_SAM_REG_TYPE = 0,
    // Generated address map, shared with the simulation topology.
    parameter int unsigned SAM_NUM_RULES = topology_pkg::SAM_NUM_RULES,
    parameter type addr_t = topology_pkg::sam_addr_t,
    parameter type sam_mask_sel_t = topology_pkg::sam_mask_sel_t,
    parameter type sam_idx_t = topology_pkg::sam_idx_t,
    parameter type sam_rule_t = topology_pkg::sam_rule_t,
    parameter sam_rule_t [SAM_NUM_RULES-1:0] SAM = topology_pkg::SAM,
    parameter logic [ni_flit_pkg::SRC_ID_WIDTH-1:0] SRC_ID = '0,
    parameter logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] SRC_PORT_ID = '0
) (
    input  wire logic                                                   ACLK,
    input  wire logic                                                   ARESETn,
    input  wire logic                                                   noc_clk,
    input  wire logic                                                   noc_rst_n,

    axi_if.wr_slv                                                  axi_wr_i,
    axi_if.rd_slv                                                  axi_rd_i,

    output wire logic                                                   tx_req_valid_o,
    output wire logic [ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT-1:0]    tx_req_flit_o,
    input  wire logic                                                   tx_req_ready_i,

    input  wire logic                                                   rx_rsp_valid_i,
    input  wire logic [ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT-1:0]    rx_rsp_flit_i,
    output wire logic                                                   rx_rsp_ready_o,

    output wire logic                                                   tx_dat_valid_o,
    output wire logic [ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT-1:0]    tx_dat_flit_o,
    input  wire logic [NOC_DAT_NUM_VC-1:0]                             tx_dat_crdvalid_i,
    input  wire logic                                                   rx_dat_valid_i,
    input  wire logic [ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT-1:0]    rx_dat_flit_i,
    output wire logic [NOC_DAT_NUM_VC-1:0]                             rx_dat_crdvalid_o
);

    localparam int unsigned REQ_FLIT_W = $bits(ni_flit_pkg::req_flit_t);
    localparam int unsigned RSP_FLIT_W = $bits(ni_flit_pkg::rsp_flit_t);
    localparam int unsigned DAT_FLIT_W = $bits(ni_flit_pkg::dat_flit_t);

    if (AXI_ID_WIDTH < 1 || AXI_ID_WIDTH > 8) begin : gen_invalid_axi_id_width
        initial $fatal(0, "Error: AXI_ID_WIDTH must be in [1, 8] (instance %m)");
    end

    if (NOC_ID_WIDTH != ni_params_pkg::NOC_ID_WIDTH_DFLT) begin : gen_invalid_noc_id_width
        initial $fatal(0, "Error: NOC_ID_WIDTH must match the generated fixed width (instance %m)");
    end

    if (AXI_ADDR_WIDTH < 1 || AXI_ADDR_WIDTH > 64) begin : gen_invalid_axi_addr_width
        initial $fatal(0, "Error: AXI_ADDR_WIDTH must be in [1, 64] (instance %m)");
    end

    if (AXI_DATA_WIDTH != 32 && AXI_DATA_WIDTH != 64 && AXI_DATA_WIDTH != 128 &&
            AXI_DATA_WIDTH != 256 && AXI_DATA_WIDTH != 512 && AXI_DATA_WIDTH != 1024) begin : gen_invalid_axi_data_width
        initial $fatal(0, "Error: AXI_DATA_WIDTH must be 32, 64, 128, 256, 512, or 1024 (instance %m)");
    end

    if (AXI_AWUSER_WIDTH < 10 || AXI_AWUSER_WIDTH > 64) begin : gen_invalid_axi_awuser_width
        initial $fatal(0, "Error: AXI_AWUSER_WIDTH must be in [10, 64] (instance %m)");
    end

    if (NOC_DAT_NUM_VC < 1 || NOC_DAT_NUM_VC > 8) begin : gen_invalid_dat_num_vc
        initial $fatal(0, "Error: NOC_DAT_NUM_VC must be in [1, 8] (instance %m)");
    end

    if (NOC_DAT_VC_MODE != ni_params_pkg::NOC_DAT_VC_MODE_SHARED &&
            NOC_DAT_VC_MODE != ni_params_pkg::NOC_DAT_VC_MODE_READ_WRITE_SPLIT) begin : gen_invalid_dat_vc_mode
        initial $fatal(0, "Error: NOC_DAT_VC_MODE is invalid (instance %m)");
    end

    if (NOC_DAT_VC_MODE == ni_params_pkg::NOC_DAT_VC_MODE_READ_WRITE_SPLIT &&
            !(NOC_DAT_NUM_VC == 2 || NOC_DAT_NUM_VC == 4 ||
                NOC_DAT_NUM_VC == 6 || NOC_DAT_NUM_VC == 8)) begin : gen_invalid_dat_vc_split
        initial $fatal(0, "Error: READ_WRITE_SPLIT requires NOC_DAT_NUM_VC of 2, 4, 6, or 8 (instance %m)");
    end

    if (AXI_FIFO_DEPTH < 2 || (AXI_FIFO_DEPTH & (AXI_FIFO_DEPTH - 1)) != 0) begin : gen_invalid_axi_fifo_depth
        initial $fatal(0, "Error: AXI_FIFO_DEPTH must be a power of two and at least 2 (instance %m)");
    end

    if (NOC_FIFO_DEPTH == 0 || (NOC_FIFO_DEPTH & (NOC_FIFO_DEPTH - 1)) != 0) begin : gen_invalid_noc_fifo_depth
        initial $fatal(0, "Error: NOC_FIFO_DEPTH must be a positive power of two (instance %m)");
    end

    if (NOC_ROUTER_VC_DEPTH < 2 ||
            (NOC_ROUTER_VC_DEPTH & (NOC_ROUTER_VC_DEPTH - 1)) != 0) begin : gen_invalid_router_vc_depth
        initial $fatal(0, "Error: NOC_ROUTER_VC_DEPTH must be a power of two and at least 2 (instance %m)");
    end

    if (NMU_ROB_B_DEPTH < 1 || NMU_ROB_B_DEPTH > 256) begin : gen_invalid_rob_b_depth
        initial $fatal(0, "Error: NMU_ROB_B_DEPTH must be in [1, 256] (instance %m)");
    end

    if (NMU_ROB_R_DEPTH < 1 || NMU_ROB_R_DEPTH > 256) begin : gen_invalid_rob_r_depth
        initial $fatal(0, "Error: NMU_ROB_R_DEPTH must be in [1, 256] (instance %m)");
    end

    if (NMU_MAX_TXNS_PER_ID < 1 || NMU_MAX_TXNS_PER_ID > 256) begin : gen_invalid_max_txns_per_id
        initial $fatal(0, "Error: NMU_MAX_TXNS_PER_ID must be in [1, 256] (instance %m)");
    end

    if (AW_SAM_REG_TYPE > 2) begin : gen_invalid_aw_sam_reg_type
        initial $fatal(0, "Error: AW_SAM_REG_TYPE must be 0, 1, or 2 (instance %m)");
    end

    if (AR_SAM_REG_TYPE > 2) begin : gen_invalid_ar_sam_reg_type
        initial $fatal(0, "Error: AR_SAM_REG_TYPE must be 0, 1, or 2 (instance %m)");
    end

    if (REQ_FLIT_W != ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT ||
            RSP_FLIT_W != ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT ||
            DAT_FLIT_W != ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT) begin : gen_invalid_flit_width
        initial $fatal(0, "Error: generated flit widths do not match the parameter package (instance %m)");
    end

    wire ni_child_types_pkg::nmu_sam_aw_result_t path_aw;
    wire logic path_aw_valid;
    wire logic path_aw_ready;
    wire ni_signals_pkg::axi_w_t path_w;
    wire logic path_w_valid;
    wire logic path_w_ready;
    wire ni_child_types_pkg::nmu_sam_ar_result_t path_ar;
    wire logic path_ar_valid;
    wire logic path_ar_ready;
    wire ni_child_types_pkg::nmu_aw_request_t ordered_aw;
    wire logic ordered_aw_valid;
    wire logic ordered_aw_ready;
    wire ni_signals_pkg::axi_w_t ordered_w;
    wire logic ordered_w_valid;
    wire logic ordered_w_ready;
    wire ni_child_types_pkg::nmu_ar_request_t ordered_ar;
    wire logic ordered_ar_valid;
    wire logic ordered_ar_ready;
    wire ni_signals_pkg::axi_b_t axi_b;
    wire logic axi_b_valid;
    wire logic axi_b_ready;
    wire ni_signals_pkg::axi_r_t axi_r;
    wire logic axi_r_valid;
    wire logic axi_r_ready;

    nmu_request_path #(
        .AXI_ID_WIDTH (AXI_ID_WIDTH),
        .NOC_ID_WIDTH (NOC_ID_WIDTH),
        .AXI_ADDR_WIDTH (AXI_ADDR_WIDTH),
        .AXI_DATA_WIDTH (AXI_DATA_WIDTH),
        .AXI_AWUSER_WIDTH (AXI_AWUSER_WIDTH),
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH),
        .NOC_DAT_NUM_VC (NOC_DAT_NUM_VC),
        .NOC_DAT_VC_MODE (NOC_DAT_VC_MODE),
        .NOC_FIFO_DEPTH (NOC_FIFO_DEPTH),
        .NOC_ROUTER_VC_DEPTH (NOC_ROUTER_VC_DEPTH),
        .NMU_MAX_TXNS_PER_ID (NMU_MAX_TXNS_PER_ID),
        .AW_SAM_REG_TYPE (AW_SAM_REG_TYPE),
        .AR_SAM_REG_TYPE (AR_SAM_REG_TYPE),
        .SAM_NUM_RULES (SAM_NUM_RULES),
        .addr_t (addr_t),
        .sam_mask_sel_t (sam_mask_sel_t),
        .sam_idx_t (sam_idx_t),
        .sam_rule_t (sam_rule_t),
        .SAM (SAM),
        .SRC_ID (SRC_ID),
        .SRC_PORT_ID (SRC_PORT_ID)
    ) i_request_path (
        .axi_clk_i (ACLK), .axi_rst_ni (ARESETn),
        .noc_clk_i (noc_clk), .noc_rst_ni (noc_rst_n),
        .m_aw_o (path_aw),
        .m_aw_valid_o (path_aw_valid),
        .m_aw_ready_i (path_aw_ready),
        .m_w_o (path_w),
        .m_w_valid_o (path_w_valid),
        .m_w_ready_i (path_w_ready),
        .m_ar_o (path_ar),
        .m_ar_valid_o (path_ar_valid),
        .m_ar_ready_i (path_ar_ready),
        .s_ordered_aw_i (ordered_aw),
        .s_ordered_aw_valid_i (ordered_aw_valid),
        .s_ordered_aw_ready_o (ordered_aw_ready),
        .s_ordered_w_i (ordered_w),
        .s_ordered_w_valid_i (ordered_w_valid),
        .s_ordered_w_ready_o (ordered_w_ready),
        .s_ordered_ar_i (ordered_ar),
        .s_ordered_ar_valid_i (ordered_ar_valid),
        .s_ordered_ar_ready_o (ordered_ar_ready),
        .s_b_i (axi_b),
        .s_b_valid_i (axi_b_valid),
        .s_b_ready_o (axi_b_ready),
        .s_r_i (axi_r),
        .s_r_valid_i (axi_r_valid),
        .s_r_ready_o (axi_r_ready),
        .axi_wr_i, .axi_rd_i,
        .tx_req_valid_o, .tx_req_flit_o, .tx_req_ready_i,
        .tx_dat_valid_o, .tx_dat_flit_o, .tx_dat_crdvalid_i
    );
    nmu_response_path #(
        .NOC_DAT_NUM_VC (NOC_DAT_NUM_VC),
        .NOC_DAT_VC_MODE (NOC_DAT_VC_MODE),
        .NOC_NI_DAT_RX_VC_DEPTH (NOC_NI_DAT_RX_VC_DEPTH),
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH),
        .NMU_ROB_B_DEPTH (NMU_ROB_B_DEPTH),
        .NMU_ROB_R_DEPTH (NMU_ROB_R_DEPTH),
        .READ_ROB_ENABLED (READ_ROB_ENABLED),
        .NMU_MAX_TXNS_PER_ID (NMU_MAX_TXNS_PER_ID)
    ) i_response_path (
        .axi_clk_i (ACLK), .axi_rst_ni (ARESETn),
        .noc_clk_i (noc_clk), .noc_rst_ni (noc_rst_n),
        .s_aw_i (path_aw),
        .s_aw_valid_i (path_aw_valid),
        .s_aw_ready_o (path_aw_ready),
        .s_w_i (path_w),
        .s_w_valid_i (path_w_valid),
        .s_w_ready_o (path_w_ready),
        .s_ar_i (path_ar),
        .s_ar_valid_i (path_ar_valid),
        .s_ar_ready_o (path_ar_ready),
        .m_ordered_aw_o (ordered_aw),
        .m_ordered_aw_valid_o (ordered_aw_valid),
        .m_ordered_aw_ready_i (ordered_aw_ready),
        .m_ordered_w_o (ordered_w),
        .m_ordered_w_valid_o (ordered_w_valid),
        .m_ordered_w_ready_i (ordered_w_ready),
        .m_ordered_ar_o (ordered_ar),
        .m_ordered_ar_valid_o (ordered_ar_valid),
        .m_ordered_ar_ready_i (ordered_ar_ready),
        .m_b_o (axi_b),
        .m_b_valid_o (axi_b_valid),
        .m_b_ready_i (axi_b_ready),
        .m_r_o (axi_r),
        .m_r_valid_o (axi_r_valid),
        .m_r_ready_i (axi_r_ready),
        .rx_rsp_valid_i, .rx_rsp_flit_i, .rx_rsp_ready_o,
        .rx_dat_valid_i, .rx_dat_flit_i, .rx_dat_crdvalid_o
    );

endmodule

`resetall
