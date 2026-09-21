// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

`include "axi/typedef.svh"

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
    // Generated address map, shared with the simulation topology.
    parameter int unsigned SAM_NUM_RULES = topology_pkg::SAM_NUM_RULES,
    parameter type addr_t = topology_pkg::sam_addr_t,
    parameter type sam_mask_sel_t = topology_pkg::sam_mask_sel_t,
    parameter type sam_idx_t = topology_pkg::sam_idx_t,
    parameter type sam_rule_t = topology_pkg::sam_rule_t,
    parameter sam_rule_t [SAM_NUM_RULES-1:0] SAM = topology_pkg::SAM,
    // Local network-interface identity.
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

    // External ID ownership belongs to NMU. This boundary stays in ACLK;
    // downstream request and response CDC carry only fixed-width NoC IDs.
    localparam int unsigned MAX_UNIQUE_IDS =
        1 << (AXI_ID_WIDTH < NOC_ID_WIDTH ? AXI_ID_WIDTH : NOC_ID_WIDTH);

    typedef logic [AXI_ID_WIDTH-1:0] external_id_t;
    typedef logic [NOC_ID_WIDTH-1:0] internal_id_t;
    typedef logic [AXI_ADDR_WIDTH-1:0] address_t;
    typedef logic [AXI_DATA_WIDTH-1:0] data_t;
    typedef logic [AXI_DATA_WIDTH/8-1:0] strobe_t;
    typedef logic [AXI_AWUSER_WIDTH-1:0] user_t;

    `AXI_TYPEDEF_ALL(external, address_t, external_id_t, data_t, strobe_t, user_t)
    `AXI_TYPEDEF_ALL(internal, address_t, internal_id_t, data_t, strobe_t, user_t)

    external_req_t external_req;
    external_resp_t external_rsp;
    internal_req_t internal_req;
    internal_resp_t internal_rsp;

    always_comb begin
        external_req = '0;
        external_req.aw.id = axi_wr_i.awid;
        external_req.aw.addr = axi_wr_i.awaddr;
        external_req.aw.len = axi_wr_i.awlen;
        external_req.aw.size = axi_wr_i.awsize;
        external_req.aw.burst = axi_wr_i.awburst;
        external_req.aw.lock = axi_wr_i.awlock;
        external_req.aw.cache = axi_wr_i.awcache;
        external_req.aw.prot = axi_wr_i.awprot;
        external_req.aw.qos = axi_wr_i.awqos;
        external_req.aw.region = axi_wr_i.awregion;
        external_req.aw.user = axi_wr_i.awuser;
        external_req.aw_valid = axi_wr_i.awvalid;
        external_req.w.data = axi_wr_i.wdata;
        external_req.w.strb = axi_wr_i.wstrb;
        external_req.w.last = axi_wr_i.wlast;
        external_req.w_valid = axi_wr_i.wvalid;
        external_req.ar.id = axi_rd_i.arid;
        external_req.ar.addr = axi_rd_i.araddr;
        external_req.ar.len = axi_rd_i.arlen;
        external_req.ar.size = axi_rd_i.arsize;
        external_req.ar.burst = axi_rd_i.arburst;
        external_req.ar.lock = axi_rd_i.arlock;
        external_req.ar.cache = axi_rd_i.arcache;
        external_req.ar.prot = axi_rd_i.arprot;
        external_req.ar.qos = axi_rd_i.arqos;
        external_req.ar.region = axi_rd_i.arregion;
        external_req.ar_valid = axi_rd_i.arvalid;
        external_req.b_ready = axi_wr_i.bready;
        external_req.r_ready = axi_rd_i.rready;
    end

    assign axi_wr_i.awready = external_rsp.aw_ready;
    assign axi_wr_i.wready = external_rsp.w_ready;
    assign axi_wr_i.bid = external_rsp.b_valid ? external_rsp.b.id : '0;
    assign axi_wr_i.bresp = external_rsp.b_valid ? external_rsp.b.resp : '0;
    assign axi_wr_i.buser = '0;
    assign axi_wr_i.bvalid = external_rsp.b_valid;
    assign axi_rd_i.arready = external_rsp.ar_ready;
    assign axi_rd_i.rid = external_rsp.r_valid ? external_rsp.r.id : '0;
    assign axi_rd_i.rdata = external_rsp.r_valid ? external_rsp.r.data : '0;
    assign axi_rd_i.rresp = external_rsp.r_valid ? external_rsp.r.resp : '0;
    assign axi_rd_i.rlast = external_rsp.r_valid ? external_rsp.r.last : '0;
    assign axi_rd_i.ruser = '0;
    assign axi_rd_i.rvalid = external_rsp.r_valid;

    axi_id_remap #(
        .AxiSlvPortIdWidth (AXI_ID_WIDTH),
        .AxiSlvPortMaxUniqIds (MAX_UNIQUE_IDS),
        .AxiMaxTxnsPerId (NMU_MAX_TXNS_PER_ID),
        .AxiMstPortIdWidth (NOC_ID_WIDTH),
        .slv_req_t (external_req_t),
        .slv_resp_t (external_resp_t),
        .mst_req_t (internal_req_t),
        .mst_resp_t (internal_resp_t)
    ) i_id_remap (
        .clk_i (ACLK),
        .rst_ni (ARESETn),
        .slv_req_i (external_req),
        .slv_resp_o (external_rsp),
        .mst_req_o (internal_req),
        .mst_resp_i (internal_rsp)
    );

    ni_signals_pkg::axi_aw_t axi_aw;
    ni_signals_pkg::axi_w_t axi_w, path_w, ordered_w;
    ni_signals_pkg::axi_ar_t axi_ar;
    ni_signals_pkg::axi_b_t ordered_b, axi_b;
    ni_signals_pkg::axi_r_t ordered_r, axi_r;
    ni_child_types_pkg::nmu_sam_aw_result_t path_aw;
    ni_child_types_pkg::nmu_sam_ar_result_t path_ar;
    ni_child_types_pkg::nmu_aw_request_t ordered_aw;
    ni_child_types_pkg::nmu_ar_request_t ordered_ar;
    ni_child_types_pkg::nmu_b_response_t decoded_b;
    ni_child_types_pkg::nmu_r_response_t decoded_r;
    wire axi_aw_ready, axi_w_ready, axi_ar_ready;
    wire path_aw_valid, path_aw_ready, path_w_valid, path_w_ready, path_ar_valid, path_ar_ready;
    wire ordered_aw_valid, ordered_aw_ready, ordered_w_valid, ordered_w_ready;
    wire ordered_ar_valid, ordered_ar_ready, ordered_b_valid, ordered_b_ready;
    wire ordered_r_valid, ordered_r_ready, decoded_b_valid, decoded_b_ready;
    wire decoded_r_valid, decoded_r_ready, axi_b_valid, axi_r_valid;
    ni_flit_pkg::req_flit_t tx_req;
    ni_flit_pkg::dat_flit_t tx_dat;
    always_comb begin
        axi_aw = '0;
        axi_ar = '0;
        axi_w = '0;
        axi_aw.awid = $bits(axi_aw.awid)'(internal_req.aw.id);
        axi_aw.awaddr = $bits(axi_aw.awaddr)'(internal_req.aw.addr);
        axi_aw.awlen = $bits(axi_aw.awlen)'(internal_req.aw.len);
        axi_aw.awsize = $bits(axi_aw.awsize)'(internal_req.aw.size);
        axi_aw.awburst = $bits(axi_aw.awburst)'(internal_req.aw.burst);
        axi_aw.awlock = $bits(axi_aw.awlock)'(internal_req.aw.lock);
        axi_aw.awcache = $bits(axi_aw.awcache)'(internal_req.aw.cache);
        axi_aw.awprot = $bits(axi_aw.awprot)'(internal_req.aw.prot);
        axi_aw.awqos = $bits(axi_aw.awqos)'(internal_req.aw.qos);
        axi_aw.awregion = $bits(axi_aw.awregion)'(internal_req.aw.region);
        axi_aw.awuser = $bits(axi_aw.awuser)'(internal_req.aw.user);
        axi_ar.arid = $bits(axi_ar.arid)'(internal_req.ar.id);
        axi_ar.araddr = $bits(axi_ar.araddr)'(internal_req.ar.addr);
        axi_ar.arlen = $bits(axi_ar.arlen)'(internal_req.ar.len);
        axi_ar.arsize = $bits(axi_ar.arsize)'(internal_req.ar.size);
        axi_ar.arburst = $bits(axi_ar.arburst)'(internal_req.ar.burst);
        axi_ar.arlock = $bits(axi_ar.arlock)'(internal_req.ar.lock);
        axi_ar.arcache = $bits(axi_ar.arcache)'(internal_req.ar.cache);
        axi_ar.arprot = $bits(axi_ar.arprot)'(internal_req.ar.prot);
        axi_ar.arqos = $bits(axi_ar.arqos)'(internal_req.ar.qos);
        axi_ar.arregion = $bits(axi_ar.arregion)'(internal_req.ar.region);
        axi_w.wdata = $bits(axi_w.wdata)'(internal_req.w.data);
        axi_w.wstrb = $bits(axi_w.wstrb)'(internal_req.w.strb);
        axi_w.wlast = $bits(axi_w.wlast)'(internal_req.w.last);
        internal_rsp = '0;
        internal_rsp.aw_ready = axi_aw_ready;
        internal_rsp.w_ready = axi_w_ready;
        internal_rsp.ar_ready = axi_ar_ready;
        internal_rsp.b_valid = axi_b_valid;
        internal_rsp.b.id = axi_b.bid;
        internal_rsp.b.resp = axi_b.bresp;
        internal_rsp.r_valid = axi_r_valid;
        internal_rsp.r.id = axi_r.rid;
        internal_rsp.r.data = AXI_DATA_WIDTH'(axi_r.rdata);
        internal_rsp.r.resp = axi_r.rresp;
        internal_rsp.r.last = axi_r.rlast;
    end
    nmu_request_path #(
        .AXI_FIFO_DEPTH(AXI_FIFO_DEPTH), .AXI_ID_WIDTH(NOC_ID_WIDTH),
        .AW_SAM_REG_TYPE(AW_SAM_REG_TYPE), .AR_SAM_REG_TYPE(AR_SAM_REG_TYPE),
        .SAM_NUM_RULES(SAM_NUM_RULES), .addr_t(addr_t), .sam_mask_sel_t(sam_mask_sel_t),
        .sam_idx_t(sam_idx_t), .sam_rule_t(sam_rule_t), .SAM(SAM)
    ) i_request_path (
        .axi_clk_i(ACLK), .axi_rst_ni(ARESETn), .noc_clk_i(noc_clk), .noc_rst_ni(noc_rst_n),
        .s_aw_i(axi_aw), .s_aw_valid_i(internal_req.aw_valid), .s_aw_ready_o(axi_aw_ready),
        .s_w_i(axi_w), .s_w_valid_i(internal_req.w_valid), .s_w_ready_o(axi_w_ready),
        .s_ar_i(axi_ar), .s_ar_valid_i(internal_req.ar_valid), .s_ar_ready_o(axi_ar_ready),
        .m_aw_o(path_aw), .m_aw_valid_o(path_aw_valid), .m_aw_ready_i(path_aw_ready),
        .m_w_o(path_w), .m_w_valid_o(path_w_valid), .m_w_ready_i(path_w_ready),
        .m_ar_o(path_ar), .m_ar_valid_o(path_ar_valid), .m_ar_ready_i(path_ar_ready)
    );
    nmu_ordering #(
        .NMU_ROB_B_DEPTH(NMU_ROB_B_DEPTH), .NMU_ROB_R_DEPTH(NMU_ROB_R_DEPTH),
        .NMU_MAX_TXNS_PER_ID(NMU_MAX_TXNS_PER_ID), .READ_ROB_ENABLED(READ_ROB_ENABLED)
    ) i_ordering (
        .clk_i(noc_clk), .rst_i(!noc_rst_n),
        .s_aw_i(path_aw), .s_aw_valid_i(path_aw_valid), .s_aw_ready_o(path_aw_ready),
        .m_aw_o(ordered_aw), .m_aw_valid_o(ordered_aw_valid), .m_aw_ready_i(ordered_aw_ready),
        .s_w_i(path_w), .s_w_valid_i(path_w_valid), .s_w_ready_o(path_w_ready),
        .m_w_o(ordered_w), .m_w_valid_o(ordered_w_valid), .m_w_ready_i(ordered_w_ready),
        .s_ar_i(path_ar), .s_ar_valid_i(path_ar_valid), .s_ar_ready_o(path_ar_ready),
        .m_ar_o(ordered_ar), .m_ar_valid_o(ordered_ar_valid), .m_ar_ready_i(ordered_ar_ready),
        .s_b_i(decoded_b), .s_b_valid_i(decoded_b_valid), .s_b_ready_o(decoded_b_ready),
        .m_b_o(ordered_b), .m_b_valid_o(ordered_b_valid), .m_b_ready_i(ordered_b_ready),
        .s_r_i(decoded_r), .s_r_valid_i(decoded_r_valid), .s_r_ready_o(decoded_r_ready),
        .m_r_o(ordered_r), .m_r_valid_o(ordered_r_valid), .m_r_ready_i(ordered_r_ready)
    );
    nmu_request_packetize #(
        .FIFO_DEPTH(NOC_FIFO_DEPTH), .DAT_NUM_VC(NOC_DAT_NUM_VC),
        .DAT_VC_MODE(NOC_DAT_VC_MODE), .ROUTER_VC_DEPTH(NOC_ROUTER_VC_DEPTH),
        .SRC_ID(SRC_ID), .SRC_PORT_ID(SRC_PORT_ID)
    ) i_packetize (
        .clk_i(noc_clk), .rst_i(!noc_rst_n),
        .s_aw_i(ordered_aw), .s_aw_valid_i(ordered_aw_valid), .s_aw_ready_o(ordered_aw_ready),
        .s_w_i(ordered_w), .s_w_valid_i(ordered_w_valid), .s_w_ready_o(ordered_w_ready),
        .s_ar_i(ordered_ar), .s_ar_valid_i(ordered_ar_valid), .s_ar_ready_o(ordered_ar_ready),
        .m_req_o(tx_req), .m_req_valid_o(tx_req_valid_o), .m_req_ready_i(tx_req_ready_i),
        .m_dat_o(tx_dat), .m_dat_valid_o(tx_dat_valid_o), .dat_credit_return_i(tx_dat_crdvalid_i)
    );
    assign tx_req_flit_o = tx_req_valid_o ? tx_req : '0;
    assign tx_dat_flit_o = tx_dat_valid_o ? tx_dat : '0;
    nmu_response_depacketize i_depacketize (
        .clk_i(noc_clk), .rst_i(!noc_rst_n),
        .s_rsp_i(ni_flit_pkg::rsp_flit_t'(rx_rsp_flit_i)),
        .s_rsp_valid_i(rx_rsp_valid_i), .s_rsp_ready_o(rx_rsp_ready_o),
        .m_b_o(decoded_b), .m_b_valid_o(decoded_b_valid), .m_b_ready_i(decoded_b_ready),
        .m_r_o(decoded_r), .m_r_valid_o(decoded_r_valid), .m_r_ready_i(decoded_r_ready)
    );
    nmu_response_path #(.AXI_FIFO_DEPTH(AXI_FIFO_DEPTH)) i_response_path (
        .noc_clk_i(noc_clk), .noc_rst_ni(noc_rst_n), .axi_clk_i(ACLK), .axi_rst_ni(ARESETn),
        .s_b_i(ordered_b), .s_b_valid_i(ordered_b_valid), .s_b_ready_o(ordered_b_ready),
        .s_r_i(ordered_r), .s_r_valid_i(ordered_r_valid), .s_r_ready_o(ordered_r_ready),
        .m_b_o(axi_b), .m_b_valid_o(axi_b_valid), .m_b_ready_i(internal_req.b_ready),
        .m_r_o(axi_r), .m_r_valid_o(axi_r_valid), .m_r_ready_i(internal_req.r_ready)
    );
    // DAT receive is outside this control-plane integration stage (#83).
    // Never acknowledge a DataR beat before the receive-VC path exists.
    assign rx_dat_ready_o = 1'b0;

endmodule

`resetall
