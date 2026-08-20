// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Network Master Unit production top-level interface and parameter contract.
module nmu #(
    // External AXI interface configuration.
    parameter int unsigned AXI_ID_WIDTH = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned NOC_ID_WIDTH = ni_params_pkg::NOC_ID_WIDTH_DFLT,
    parameter int unsigned AXI_ADDR_WIDTH = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned AXI_DATA_WIDTH = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    parameter int unsigned AXI_AWUSER_WIDTH = ni_params_pkg::AXI_AWUSER_WIDTH_DFLT,
    parameter int unsigned AXI_FIFO_DEPTH = ni_params_pkg::AXI_FIFO_DEPTH_DFLT,
    // NoC interface configuration.
    parameter int unsigned NOC_DAT_NUM_VC = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned NOC_DAT_VC_MODE = ni_params_pkg::NOC_DAT_VC_MODE_DFLT,
    parameter int unsigned NOC_FIFO_DEPTH = ni_params_pkg::NOC_FIFO_DEPTH_DFLT,
    parameter int unsigned NOC_ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT,
    // NMU transaction ordering configuration.
    parameter int unsigned NMU_ROB_B_DEPTH = ni_params_pkg::NMU_ROB_B_DEPTH_DFLT,
    parameter int unsigned NMU_ROB_R_DEPTH = ni_params_pkg::NMU_ROB_R_DEPTH_DFLT,
    parameter bit READ_ROB_ENABLED = bit'(ni_params_pkg::NMU_READ_ROB_ENABLED_DFLT),
    parameter int unsigned NMU_MAX_TXNS_PER_ID = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT,
    parameter int unsigned AW_SAM_REG_TYPE = 0,
    parameter int unsigned AR_SAM_REG_TYPE = 0,
    // Local network-interface identity.
    parameter logic [ni_flit_pkg::SRC_ID_WIDTH-1:0] SRC_ID = '0,
    parameter logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] SRC_PORT_ID = '0
) (
    input  wire logic                                                   ACLK,
    input  wire logic                                                   ARESETn,
    input  wire logic                                                   noc_clk,
    input  wire logic                                                   noc_rst_n,

    taxi_axi_if.wr_slv                                                  axi_wr_i,
    taxi_axi_if.rd_slv                                                  axi_rd_i,

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
    output wire logic                                                   rx_dat_ready_o
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

endmodule

`resetall
