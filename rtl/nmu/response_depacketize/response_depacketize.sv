// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// Combinational response reconstruction with optional output registers.
module nmu_response_depacketize #(
    parameter int unsigned B_REG_TYPE = 0,
    parameter int unsigned R_REG_TYPE = 0
) (
    input  wire logic                          clk_i,
    input  wire logic                          rst_n_i,
    input  wire ni_flit_pkg::rsp_flit_t        s_b_i,
    input  wire logic                          s_b_valid_i,
    output wire logic                          s_b_ready_o,
    input  wire ni_flit_pkg::dat_flit_t        s_r_i,
    input  wire logic                          s_r_valid_i,
    output wire logic                          s_r_ready_o,
    output wire ni_types_pkg::nmu_b_response_t m_b_o,
    output wire logic                          m_b_valid_o,
    input  wire logic                          m_b_ready_i,
    output wire ni_types_pkg::nmu_r_response_t m_r_o,
    output wire logic                          m_r_valid_o,
    input  wire logic                          m_r_ready_i
);
    import ni_flit_pkg::*;
    localparam int unsigned DATA_W = $bits(m_r_o.axi.rdata);
    ni_types_pkg::nmu_b_response_t b;
    wire ni_types_pkg::nmu_b_response_t b_output;
    wire ni_types_pkg::nmu_r_response_t r_output;
    assign m_b_o = m_b_valid_o ? b_output : '0;
    assign m_r_o = m_r_valid_o ? r_output : '0;
    ni_types_pkg::nmu_r_response_t r;
    always_comb begin
        b = '0;
        r = '0;
        if (s_b_valid_i) begin
            b.axi.bid           = s_b_i.payload[B_BID_LSB +: B_BID_WIDTH];
            b.axi.bresp         = s_b_i.payload[B_BRESP_LSB +: B_BRESP_WIDTH];
            b.meta.is_data      = s_b_i.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_DataB);
            b.meta.ordering_req = s_b_i.header[ORDERING_REQ_LSB];
            b.meta.ordering_tag = s_b_i.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
        end
        if (s_r_valid_i) begin
            r.meta.is_data      = s_r_i.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_DataR);
            r.meta.ordering_req = s_r_i.header[ORDERING_REQ_LSB];
            r.meta.ordering_tag = s_r_i.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
            if (r.meta.is_data) begin
                r.axi.rid   = s_r_i.payload[DATA_R_RID_LSB +: DATA_R_RID_WIDTH];
                r.axi.rresp = s_r_i.payload[DATA_R_RRESP_LSB +: DATA_R_RRESP_WIDTH];
                r.axi.rlast = s_r_i.payload[DATA_R_RLAST_LSB];
                r.axi.rdata = DATA_W'(s_r_i.payload[DATA_R_RDATA_LSB +: DATA_R_RDATA_WIDTH]);
            end else begin
                r.axi.rid   = s_r_i.payload[NARROW_R_RID_LSB +: NARROW_R_RID_WIDTH];
                r.axi.rresp = s_r_i.payload[NARROW_R_RRESP_LSB +: NARROW_R_RRESP_WIDTH];
                r.axi.rlast = s_r_i.payload[NARROW_R_RLAST_LSB];
                r.axi.rdata = DATA_W'(s_r_i.payload[NARROW_R_RDATA_LSB +: NARROW_R_RDATA_WIDTH]);
            end
        end
    end
    stream_register #(
        .REG_TYPE (B_REG_TYPE                    ),
        .data_t   (ni_types_pkg::nmu_b_response_t)
    ) i_b_reg (
        .clk_i     (clk_i                 ),
        .rst_n_i   (rst_n_i               ),
        .s_data_i  (b                     ),
        .s_valid_i (rst_n_i && s_b_valid_i),
        .s_ready_o (s_b_ready_o           ),
        .m_data_o  (b_output              ),
        .m_valid_o (m_b_valid_o           ),
        .m_ready_i (m_b_ready_i           )
    );
    stream_register #(
        .REG_TYPE (R_REG_TYPE                    ),
        .data_t   (ni_types_pkg::nmu_r_response_t)
    ) i_r_reg (
        .clk_i     (clk_i                 ),
        .rst_n_i   (rst_n_i               ),
        .s_data_i  (r                     ),
        .s_valid_i (rst_n_i && s_r_valid_i),
        .s_ready_o (s_r_ready_o           ),
        .m_data_o  (r_output              ),
        .m_valid_o (m_r_valid_o           ),
        .m_ready_i (m_r_ready_i           )
    );
endmodule
`resetall
