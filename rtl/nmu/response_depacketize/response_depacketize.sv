// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// Network ejection: buffer/select flits, then reconstruct AXI responses.
module nmu_response_depacketize #(
    parameter int unsigned RSP_FIFO_DEPTH  = ni_params_pkg::NMU_DEPKT_Q_DEPTH,
    parameter int unsigned NUM_DAT_VC      = ni_params_pkg::NUM_DAT_VC,
    parameter int unsigned DAT_VC_MODE     = ni_params_pkg::NOC_DAT_VC_MODE,
    parameter int unsigned DAT_RX_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH
) (
    input  wire logic                                           clk_i,
    input  wire logic                                           rst_n_i,
    input  wire ni_flit_pkg::rsp_flit_t                         s_rsp_i,
    input  wire logic                                           s_rsp_valid_i,
    output wire logic                                           s_rsp_ready_o,
    input  wire ni_flit_pkg::dat_flit_t                         s_dat_i,
    input  wire logic                                           s_dat_valid_i,
    output wire logic                          [NUM_DAT_VC-1:0] dat_credit_return_o,
    output wire ni_types_pkg::nmu_b_response_t                  m_b_o,
    output wire logic                                           m_b_valid_o,
    input  wire logic                                           m_b_ready_i,
    output wire ni_types_pkg::nmu_r_response_t                  m_r_o,
    output wire logic                                           m_r_valid_o,
    input  wire logic                                           m_r_ready_i
);
    import ni_flit_pkg::*;
    localparam int unsigned DATA_W = $bits(m_r_o.axi.rdata);
    wire rsp_flit_t                b_flit;
    wire dat_flit_t                r_flit;
    ni_types_pkg::nmu_b_response_t b;
    ni_types_pkg::nmu_r_response_t r;
    nmu_response_buffer #(
        .RSP_FIFO_DEPTH  (RSP_FIFO_DEPTH ),
        .NUM_DAT_VC      (NUM_DAT_VC     ),
        .DAT_VC_MODE     (DAT_VC_MODE    ),
        .DAT_RX_VC_DEPTH (DAT_RX_VC_DEPTH)
    ) i_buffer (
        .clk_i               (clk_i              ),
        .rst_n_i             (rst_n_i            ),
        .s_rsp_i             (s_rsp_i            ),
        .s_rsp_valid_i       (s_rsp_valid_i      ),
        .s_rsp_ready_o       (s_rsp_ready_o      ),
        .s_dat_i             (s_dat_i            ),
        .s_dat_valid_i       (s_dat_valid_i      ),
        .dat_credit_return_o (dat_credit_return_o),
        .m_b_o               (b_flit             ),
        .m_b_valid_o         (m_b_valid_o        ),
        .m_b_ready_i         (m_b_ready_i        ),
        .m_r_o               (r_flit             ),
        .m_r_valid_o         (m_r_valid_o        ),
        .m_r_ready_i         (m_r_ready_i        )
    );
    always_comb begin
        b = '0;
        r = '0;
        if (m_b_valid_o) begin
            b.axi.bid           = b_flit.payload[B_BID_LSB +: B_BID_WIDTH];
            b.axi.bresp         = b_flit.payload[B_BRESP_LSB +: B_BRESP_WIDTH];
            b.meta.is_data      = b_flit.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_DataB);
            b.meta.ordering_req = b_flit.header[ORDERING_REQ_LSB];
            b.meta.ordering_tag = b_flit.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
        end
        if (m_r_valid_o) begin
            r.meta.is_data      = r_flit.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_DataR);
            r.meta.ordering_req = r_flit.header[ORDERING_REQ_LSB];
            r.meta.ordering_tag = r_flit.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
            if (r.meta.is_data) begin
                r.axi.rid   = r_flit.payload[DATA_R_RID_LSB +: DATA_R_RID_WIDTH];
                r.axi.rresp = r_flit.payload[DATA_R_RRESP_LSB +: DATA_R_RRESP_WIDTH];
                r.axi.rlast = r_flit.payload[DATA_R_RLAST_LSB];
                r.axi.rdata = DATA_W'(r_flit.payload[DATA_R_RDATA_LSB +: DATA_R_RDATA_WIDTH]);
            end else begin
                r.axi.rid   = r_flit.payload[NARROW_R_RID_LSB +: NARROW_R_RID_WIDTH];
                r.axi.rresp = r_flit.payload[NARROW_R_RRESP_LSB +: NARROW_R_RRESP_WIDTH];
                r.axi.rlast = r_flit.payload[NARROW_R_RLAST_LSB];
                r.axi.rdata = DATA_W'(r_flit.payload[NARROW_R_RDATA_LSB +: NARROW_R_RDATA_WIDTH]);
            end
        end
    end
    assign m_b_o = b;
    assign m_r_o = r;
endmodule
`resetall
