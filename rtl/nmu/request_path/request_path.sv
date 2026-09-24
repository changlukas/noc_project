// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

`include "axi/typedef.svh"

// Request transport and ID ownership. Ordering is supplied by the response path.
module nmu_request_path #(
    parameter int unsigned AXI_ID_WIDTH                              = ni_params_pkg::AXI_ID_WIDTH,
    parameter int unsigned NOC_ID_WIDTH                              = ni_params_pkg::NOC_ID_WIDTH,
    parameter int unsigned MAX_ACTIVE_IDS                            = 1 << (AXI_ID_WIDTH < NOC_ID_WIDTH ? AXI_ID_WIDTH : NOC_ID_WIDTH),
    parameter int unsigned AXI_ADDR_WIDTH                            = ni_params_pkg::AXI_ADDR_WIDTH,
    parameter int unsigned AXI_DATA_WIDTH                            = ni_params_pkg::AXI_DATA_WIDTH,
    parameter int unsigned AXI_AWUSER_WIDTH                          = ni_params_pkg::AXI_AWUSER_WIDTH,
    parameter int unsigned AXI_FIFO_DEPTH                            = 32,
    parameter int unsigned NUM_DAT_VC                                = ni_params_pkg::NUM_DAT_VC,
    parameter int unsigned NOC_DAT_VC_MODE                           = ni_params_pkg::NOC_DAT_VC_MODE,
    parameter int unsigned REQ_FIFO_DEPTH                            = 32,
    parameter int unsigned AW_FIFO_DEPTH                             = AXI_FIFO_DEPTH,
    parameter int unsigned W_FIFO_DEPTH                              = AXI_FIFO_DEPTH,
    parameter int unsigned AR_FIFO_DEPTH                             = AXI_FIFO_DEPTH,
    parameter int unsigned DAT_TX_FIFO_DEPTH                         = 32,
    parameter int unsigned REQ_AW_REG_TYPE                           = 0,
    parameter int unsigned REQ_W_REG_TYPE                            = 0,
    parameter int unsigned REQ_AR_REG_TYPE                           = 0,
    parameter int unsigned DAT_AW_REG_TYPE                           = 0,
    parameter int unsigned DAT_W_REG_TYPE                            = 0,
    parameter int unsigned NOC_ROUTER_VC_DEPTH                       = ni_params_pkg::NOC_ROUTER_VC_DEPTH,
    parameter int unsigned MAX_OUTSTANDING_PER_ID                    = ni_params_pkg::NMU_MAX_OUTSTANDING_PER_ID,
    parameter int unsigned AW_SAM_REG_TYPE                           = 0,
    parameter int unsigned AR_SAM_REG_TYPE                           = 0,
    parameter int unsigned SAM_NUM_RULES,
    parameter type addr_t,
    parameter type sam_mask_sel_t,
    parameter type sam_result_t,
    parameter type sam_rule_t,
    parameter sam_rule_t [SAM_NUM_RULES-1:0] SAM,
    parameter logic [ni_flit_pkg::SRC_ID_WIDTH-1:0]      SRC_ID      = '0,
    parameter logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] SRC_PORT_ID = '0
) (
    input  wire logic                                                                     axi_clk_i,
    input  wire logic                                                                     axi_rst_n_i,
    input  wire logic                                                                     noc_clk_i,
    input  wire logic                                                                     noc_rst_n_i,
    axi_if.wr_slv                                                                         axi_wr_i,
    axi_if.rd_slv                                                                         axi_rd_i,
    output wire ni_types_pkg::nmu_sam_aw_result_t                                         m_aw_o,
    output wire logic                                                                     m_aw_valid_o,
    input  wire logic                                                                     m_aw_ready_i,
    output wire ni_signals_pkg::noc_axi_w_t                                               m_w_o,
    output wire logic                                                                     m_w_valid_o,
    input  wire logic                                                                     m_w_ready_i,
    output wire ni_types_pkg::nmu_sam_ar_result_t                                         m_ar_o,
    output wire logic                                                                     m_ar_valid_o,
    input  wire logic                                                                     m_ar_ready_i,
    input  wire ni_types_pkg::nmu_aw_request_t                                            s_ordered_aw_i,
    input  wire logic                                                                     s_ordered_aw_valid_i,
    output wire logic                                                                     s_ordered_aw_ready_o,
    input  wire ni_signals_pkg::noc_axi_w_t                                               s_ordered_w_i,
    input  wire logic                                                                     s_ordered_w_valid_i,
    output wire logic                                                                     s_ordered_w_ready_o,
    input  wire ni_types_pkg::nmu_ar_request_t                                            s_ordered_ar_i,
    input  wire logic                                                                     s_ordered_ar_valid_i,
    output wire logic                                                                     s_ordered_ar_ready_o,
    input  wire ni_signals_pkg::noc_axi_b_t                                               s_b_i,
    input  wire logic                                                                     s_b_valid_i,
    output wire logic                                                                     s_b_ready_o,
    input  wire ni_signals_pkg::noc_axi_r_t                                               s_r_i,
    input  wire logic                                                                     s_r_valid_i,
    output wire logic                                                                     s_r_ready_o,
    output wire logic                                                                     tx_req_valid_o,
    output wire logic                             [ni_params_pkg::NOC_REQ_FLIT_WIDTH-1:0] tx_req_flit_o,
    input  wire logic                                                                     tx_req_ready_i,
    output wire logic                                                                     tx_dat_valid_o,
    output wire logic                             [ni_params_pkg::NOC_DAT_FLIT_WIDTH-1:0] tx_dat_flit_o,
    input  wire logic                                                    [NUM_DAT_VC-1:0] tx_dat_crdvalid_i
);
    import ni_types_pkg::*;

    // External ID ownership belongs to NMU. This boundary stays in axi_clk_i;
    // downstream request and response CDC carry only fixed-width NoC IDs.

    typedef logic [AXI_ID_WIDTH-1:0] external_id_t;
    typedef logic [NOC_ID_WIDTH-1:0] internal_id_t;
    typedef logic [AXI_ADDR_WIDTH-1:0] address_t;
    typedef logic [AXI_DATA_WIDTH-1:0] data_t;
    typedef logic [AXI_DATA_WIDTH/8-1:0] strobe_t;
    typedef logic [AXI_AWUSER_WIDTH-1:0] user_t;

    `AXI_TYPEDEF_ALL(external, address_t, external_id_t, data_t, strobe_t, user_t)
    `AXI_TYPEDEF_ALL(internal, address_t, internal_id_t, data_t, strobe_t, user_t)

    external_req_t  external_req;
    external_resp_t external_rsp;
    internal_req_t  internal_req;
    internal_resp_t internal_rsp;

    ni_signals_pkg::noc_axi_aw_t axi_aw;
    ni_signals_pkg::noc_axi_w_t  axi_w;
    ni_signals_pkg::noc_axi_ar_t axi_ar;
    wire                     axi_aw_ready, axi_w_ready, axi_ar_ready;
    ni_flit_pkg::req_flit_t  tx_req;
    ni_flit_pkg::dat_flit_t  tx_dat;

    wire ni_signals_pkg::noc_axi_aw_t fifo_aw;
    wire logic                    fifo_aw_valid;
    wire logic                    fifo_aw_ready;
    wire ni_signals_pkg::noc_axi_ar_t fifo_ar;
    wire logic                    fifo_ar_valid;
    wire logic                    fifo_ar_ready;

    always_comb begin
        external_req           = '0;
        external_req.aw.id     = axi_wr_i.awid;
        external_req.aw.addr   = axi_wr_i.awaddr;
        external_req.aw.len    = axi_wr_i.awlen;
        external_req.aw.size   = axi_wr_i.awsize;
        external_req.aw.burst  = axi_wr_i.awburst;
        external_req.aw.lock   = axi_wr_i.awlock;
        external_req.aw.cache  = axi_wr_i.awcache;
        external_req.aw.prot   = axi_wr_i.awprot;
        external_req.aw.qos    = axi_wr_i.awqos;
        external_req.aw.region = axi_wr_i.awregion;
        external_req.aw.user   = axi_wr_i.awuser;
        external_req.aw_valid  = axi_wr_i.awvalid;
        external_req.w.data    = axi_wr_i.wdata;
        external_req.w.strb    = axi_wr_i.wstrb;
        external_req.w.last    = axi_wr_i.wlast;
        external_req.w_valid   = axi_wr_i.wvalid;
        external_req.ar.id     = axi_rd_i.arid;
        external_req.ar.addr   = axi_rd_i.araddr;
        external_req.ar.len    = axi_rd_i.arlen;
        external_req.ar.size   = axi_rd_i.arsize;
        external_req.ar.burst  = axi_rd_i.arburst;
        external_req.ar.lock   = axi_rd_i.arlock;
        external_req.ar.cache  = axi_rd_i.arcache;
        external_req.ar.prot   = axi_rd_i.arprot;
        external_req.ar.qos    = axi_rd_i.arqos;
        external_req.ar.region = axi_rd_i.arregion;
        external_req.ar_valid  = axi_rd_i.arvalid;
        external_req.b_ready   = axi_wr_i.bready;
        external_req.r_ready   = axi_rd_i.rready;
    end

    assign axi_wr_i.awready = external_rsp.aw_ready;
    assign axi_wr_i.wready  = external_rsp.w_ready;
    assign axi_wr_i.bid     = external_rsp.b_valid ? external_rsp.b.id : '0;
    assign axi_wr_i.bresp   = external_rsp.b_valid ? external_rsp.b.resp : '0;
    assign axi_wr_i.buser   = '0;
    assign axi_wr_i.bvalid  = external_rsp.b_valid;
    assign axi_rd_i.arready = external_rsp.ar_ready;
    assign axi_rd_i.rid     = external_rsp.r_valid ? external_rsp.r.id : '0;
    assign axi_rd_i.rdata   = external_rsp.r_valid ? external_rsp.r.data : '0;
    assign axi_rd_i.rresp   = external_rsp.r_valid ? external_rsp.r.resp : '0;
    assign axi_rd_i.rlast   = external_rsp.r_valid ? external_rsp.r.last : '0;
    assign axi_rd_i.ruser   = '0;
    assign axi_rd_i.rvalid  = external_rsp.r_valid;

    nmu_id_remap #(
        .AXI_ID_WIDTH           (AXI_ID_WIDTH          ),
        .MAX_ACTIVE_IDS         (MAX_ACTIVE_IDS        ),
        .MAX_OUTSTANDING_PER_ID (MAX_OUTSTANDING_PER_ID),
        .NOC_ID_WIDTH           (NOC_ID_WIDTH          ),
        .slv_req_t              (external_req_t        ),
        .slv_resp_t             (external_resp_t       ),
        .mst_req_t              (internal_req_t        ),
        .mst_resp_t             (internal_resp_t       )
    ) i_id_remap (
        .clk_i      (axi_clk_i   ),
        .rst_n_i    (axi_rst_n_i ),
        .slv_req_i  (external_req),
        .slv_resp_o (external_rsp),
        .mst_req_o  (internal_req),
        .mst_resp_i (internal_rsp)
    );

    always_comb begin
        axi_aw                = '0;
        axi_ar                = '0;
        axi_w                 = '0;
        axi_aw.awid           = $bits(axi_aw.awid)'(internal_req.aw.id);
        axi_aw.awaddr         = $bits(axi_aw.awaddr)'(internal_req.aw.addr);
        axi_aw.awlen          = $bits(axi_aw.awlen)'(internal_req.aw.len);
        axi_aw.awsize         = $bits(axi_aw.awsize)'(internal_req.aw.size);
        axi_aw.awburst        = $bits(axi_aw.awburst)'(internal_req.aw.burst);
        axi_aw.awlock         = $bits(axi_aw.awlock)'(internal_req.aw.lock);
        axi_aw.awcache        = $bits(axi_aw.awcache)'(internal_req.aw.cache);
        axi_aw.awprot         = $bits(axi_aw.awprot)'(internal_req.aw.prot);
        axi_aw.awqos          = $bits(axi_aw.awqos)'(internal_req.aw.qos);
        axi_aw.awregion       = $bits(axi_aw.awregion)'(internal_req.aw.region);
        axi_aw.awuser         = $bits(axi_aw.awuser)'(internal_req.aw.user);
        axi_ar.arid           = $bits(axi_ar.arid)'(internal_req.ar.id);
        axi_ar.araddr         = $bits(axi_ar.araddr)'(internal_req.ar.addr);
        axi_ar.arlen          = $bits(axi_ar.arlen)'(internal_req.ar.len);
        axi_ar.arsize         = $bits(axi_ar.arsize)'(internal_req.ar.size);
        axi_ar.arburst        = $bits(axi_ar.arburst)'(internal_req.ar.burst);
        axi_ar.arlock         = $bits(axi_ar.arlock)'(internal_req.ar.lock);
        axi_ar.arcache        = $bits(axi_ar.arcache)'(internal_req.ar.cache);
        axi_ar.arprot         = $bits(axi_ar.arprot)'(internal_req.ar.prot);
        axi_ar.arqos          = $bits(axi_ar.arqos)'(internal_req.ar.qos);
        axi_ar.arregion       = $bits(axi_ar.arregion)'(internal_req.ar.region);
        axi_w.wdata           = $bits(axi_w.wdata)'(internal_req.w.data);
        axi_w.wstrb           = $bits(axi_w.wstrb)'(internal_req.w.strb);
        axi_w.wlast           = $bits(axi_w.wlast)'(internal_req.w.last);
        internal_rsp          = '0;
        internal_rsp.aw_ready = axi_aw_ready;
        internal_rsp.w_ready  = axi_w_ready;
        internal_rsp.ar_ready = axi_ar_ready;
        internal_rsp.b_valid  = s_b_valid_i;
        internal_rsp.b.id     = s_b_i.bid;
        internal_rsp.b.resp   = s_b_i.bresp;
        internal_rsp.r_valid  = s_r_valid_i;
        internal_rsp.r.id     = s_r_i.rid;
        internal_rsp.r.data   = AXI_DATA_WIDTH'(s_r_i.rdata);
        internal_rsp.r.resp   = s_r_i.rresp;
        internal_rsp.r.last   = s_r_i.rlast;
    end
    assign s_b_ready_o = internal_req.b_ready;
    assign s_r_ready_o = internal_req.r_ready;
    nmu_request_fifo #(
        .AXI_FIFO_DEPTH (AXI_FIFO_DEPTH          ),
        .AXI_ID_WIDTH   (NOC_ID_WIDTH            ),
        .aw_t           (ni_signals_pkg::noc_axi_aw_t),
        .w_t            (ni_signals_pkg::noc_axi_w_t ),
        .ar_t           (ni_signals_pkg::noc_axi_ar_t),
        .AW_FIFO_DEPTH  (AW_FIFO_DEPTH           ),
        .W_FIFO_DEPTH   (W_FIFO_DEPTH            ),
        .AR_FIFO_DEPTH  (AR_FIFO_DEPTH           )
    ) i_request_fifo (
        .axi_clk_i    (axi_clk_i            ),
        .axi_rst_n_i  (axi_rst_n_i          ),
        .noc_clk_i    (noc_clk_i            ),
        .noc_rst_n_i  (noc_rst_n_i          ),
        .s_aw_valid_i (internal_req.aw_valid),
        .s_aw_ready_o (axi_aw_ready         ),
        .s_aw_data_i  (axi_aw               ),
        .m_aw_valid_o (fifo_aw_valid        ),
        .m_aw_ready_i (fifo_aw_ready        ),
        .m_aw_data_o  (fifo_aw              ),
        .s_w_valid_i  (internal_req.w_valid ),
        .s_w_ready_o  (axi_w_ready          ),
        .s_w_data_i   (axi_w                ),
        .m_w_valid_o  (m_w_valid_o          ),
        .m_w_ready_i  (m_w_ready_i          ),
        .m_w_data_o   (m_w_o                ),
        .s_ar_valid_i (internal_req.ar_valid),
        .s_ar_ready_o (axi_ar_ready         ),
        .s_ar_data_i  (axi_ar               ),
        .m_ar_valid_o (fifo_ar_valid        ),
        .m_ar_ready_i (fifo_ar_ready        ),
        .m_ar_data_o  (fifo_ar              )
    );

    nmu_sam #(
        .AW_SAM_REG_TYPE (AW_SAM_REG_TYPE),
        .AR_SAM_REG_TYPE (AR_SAM_REG_TYPE),
        .SAM_NUM_RULES   (SAM_NUM_RULES  ),
        .addr_t          (addr_t         ),
        .sam_mask_sel_t  (sam_mask_sel_t ),
        .sam_result_t    (sam_result_t   ),
        .sam_rule_t      (sam_rule_t     ),
        .SAM             (SAM            )
    ) i_sam (
        .noc_clk_i    (noc_clk_i    ),
        .noc_rst_n_i  (noc_rst_n_i  ),
        .s_aw_valid_i (fifo_aw_valid),
        .s_aw_ready_o (fifo_aw_ready),
        .s_aw_i       (fifo_aw      ),
        .m_aw_valid_o (m_aw_valid_o ),
        .m_aw_ready_i (m_aw_ready_i ),
        .m_aw_o       (m_aw_o       ),
        .s_ar_valid_i (fifo_ar_valid),
        .s_ar_ready_o (fifo_ar_ready),
        .s_ar_i       (fifo_ar      ),
        .m_ar_valid_o (m_ar_valid_o ),
        .m_ar_ready_i (m_ar_ready_i ),
        .m_ar_o       (m_ar_o       )
    );

    wire ni_flit_pkg::req_flit_t [NUM_NMU_REQ_CH-1:0] req_flit;
    wire ni_flit_pkg::dat_flit_t [NUM_NMU_DAT_CH-1:0] dat_flit;
    wire                         [NUM_NMU_REQ_CH-1:0] req_valid, req_ready;
    wire                         [NUM_NMU_DAT_CH-1:0] dat_valid, dat_ready;
    wire ni_types_pkg::nmu_aw_request_t packet_aw, packet_w_aw;
    wire ni_signals_pkg::noc_axi_w_t packet_w;
    wire logic packet_aw_valid, packet_aw_ready, packet_w_valid, packet_w_ready;
    wire logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0] packet_w_beat;
    nmu_write_context i_write_context (
        .clk_i        (noc_clk_i           ),
        .rst_n_i      (noc_rst_n_i         ),
        .s_aw_i       (s_ordered_aw_i      ),
        .s_aw_valid_i (s_ordered_aw_valid_i),
        .s_aw_ready_o (s_ordered_aw_ready_o),
        .m_aw_o       (packet_aw           ),
        .m_aw_valid_o (packet_aw_valid     ),
        .m_aw_ready_i (packet_aw_ready     ),
        .s_w_i        (s_ordered_w_i       ),
        .s_w_valid_i  (s_ordered_w_valid_i ),
        .s_w_ready_o  (s_ordered_w_ready_o ),
        .m_w_o        (packet_w            ),
        .m_w_valid_o  (packet_w_valid      ),
        .m_w_ready_i  (packet_w_ready      ),
        .m_w_aw_o     (packet_w_aw         ),
        .m_w_beat_o   (packet_w_beat       )
    );
    nmu_request_packetize #(
        .REQ_AW_REG_TYPE (REQ_AW_REG_TYPE),
        .REQ_W_REG_TYPE  (REQ_W_REG_TYPE ),
        .REQ_AR_REG_TYPE (REQ_AR_REG_TYPE),
        .DAT_AW_REG_TYPE (DAT_AW_REG_TYPE),
        .DAT_W_REG_TYPE  (DAT_W_REG_TYPE ),
        .SRC_ID          (SRC_ID         ),
        .SRC_PORT_ID     (SRC_PORT_ID    )
    ) i_packetize (
        .clk_i         (noc_clk_i           ),
        .rst_n_i       (noc_rst_n_i         ),
        .s_aw_i        (packet_aw           ),
        .s_aw_valid_i  (packet_aw_valid     ),
        .s_aw_ready_o  (packet_aw_ready     ),
        .s_w_aw_i      (packet_w_aw         ),
        .s_w_beat_i    (packet_w_beat       ),
        .s_w_i         (packet_w            ),
        .s_w_valid_i   (packet_w_valid      ),
        .s_w_ready_o   (packet_w_ready      ),
        .s_ar_i        (s_ordered_ar_i      ),
        .s_ar_valid_i  (s_ordered_ar_valid_i),
        .s_ar_ready_o  (s_ordered_ar_ready_o),
        .m_req_o       (req_flit            ),
        .m_req_valid_o (req_valid           ),
        .m_req_ready_i (req_ready           ),
        .m_dat_o       (dat_flit            ),
        .m_dat_valid_o (dat_valid           ),
        .m_dat_ready_i (dat_ready           )
    );
    wire ni_flit_pkg::req_flit_t assigned_req;
    wire ni_flit_pkg::dat_flit_t assigned_dat;
    wire assigned_req_valid, assigned_req_ready, assigned_dat_valid;
    wire [NUM_DAT_VC-1:0] dat_fifo_ready;
    nmu_channel_assign #(
        .NUM_DAT_VC  (NUM_DAT_VC     ),
        .DAT_VC_MODE (NOC_DAT_VC_MODE)
    ) i_channel_assign (
        .clk_i         (noc_clk_i         ),
        .rst_n_i       (noc_rst_n_i       ),
        .s_req_i       (req_flit          ),
        .s_req_valid_i (req_valid         ),
        .s_req_ready_o (req_ready         ),
        .s_dat_i       (dat_flit          ),
        .s_dat_valid_i (dat_valid         ),
        .s_dat_ready_o (dat_ready         ),
        .m_req_o       (assigned_req      ),
        .m_req_valid_o (assigned_req_valid),
        .m_req_ready_i (assigned_req_ready),
        .m_dat_o       (assigned_dat      ),
        .m_dat_valid_o (assigned_dat_valid),
        .dat_ready_i   (dat_fifo_ready    )
    );
    nmu_request_buffer #(
        .REQ_FIFO_DEPTH  (REQ_FIFO_DEPTH     ),
        .DAT_FIFO_DEPTH  (DAT_TX_FIFO_DEPTH  ),
        .NUM_DAT_VC      (NUM_DAT_VC         ),
        .DAT_VC_MODE     (NOC_DAT_VC_MODE    ),
        .ROUTER_VC_DEPTH (NOC_ROUTER_VC_DEPTH)
    ) i_tx_buffer (
        .clk_i               (noc_clk_i         ),
        .rst_n_i             (noc_rst_n_i       ),
        .s_req_i             (assigned_req      ),
        .s_req_valid_i       (assigned_req_valid),
        .s_req_ready_o       (assigned_req_ready),
        .m_req_o             (tx_req            ),
        .m_req_valid_o       (tx_req_valid_o    ),
        .m_req_ready_i       (tx_req_ready_i    ),
        .s_dat_i             (assigned_dat      ),
        .s_dat_valid_i       (assigned_dat_valid),
        .dat_ready_o         (dat_fifo_ready    ),
        .m_dat_o             (tx_dat            ),
        .m_dat_valid_o       (tx_dat_valid_o    ),
        .dat_credit_return_i (tx_dat_crdvalid_i )
    );
    assign tx_req_flit_o = tx_req_valid_o ? tx_req : '0;
    assign tx_dat_flit_o = tx_dat_valid_o ? tx_dat : '0;

endmodule

`resetall
