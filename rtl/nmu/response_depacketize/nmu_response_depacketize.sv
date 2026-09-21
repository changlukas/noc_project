// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// RSP-network response decode. B and NarrowR have independent class queues.
// DAT ingress is a separate integration package and does not enter this block.
module nmu_response_depacketize #(
    parameter int unsigned FIFO_DEPTH = ni_params_pkg::NMU_DEPKT_Q_DEPTH_DFLT
) (
    input wire logic clk_i,
    input wire logic rst_i,
    input wire ni_flit_pkg::rsp_flit_t s_rsp_i,
    input wire logic s_rsp_valid_i,
    output wire logic s_rsp_ready_o,
    output wire ni_child_types_pkg::nmu_b_response_t m_b_o,
    output wire logic m_b_valid_o,
    input wire logic m_b_ready_i,
    output wire ni_child_types_pkg::nmu_r_response_t m_r_o,
    output wire logic m_r_valid_o,
    input wire logic m_r_ready_i
);
    import ni_flit_pkg::*;
    localparam int unsigned DATA_WIDTH = $bits(m_r_o.axi.rdata);
    if (FIFO_DEPTH < 1 || FIFO_DEPTH > 1024) begin : gen_invalid_depth
        initial $fatal(0, "Error: response FIFO_DEPTH must be in [1, 1024] (instance %m)");
    end
    wire [AXI_CH_WIDTH-1:0] channel = s_rsp_i.header[AXI_CH_LSB +: AXI_CH_WIDTH];
    wire is_b = channel == AXI_CH_WIDTH'(AXI_CH_NarrowB) ||
                channel == AXI_CH_WIDTH'(AXI_CH_DataB);
    wire is_r = channel == AXI_CH_WIDTH'(AXI_CH_NarrowR);
    ni_child_types_pkg::nmu_b_response_t decoded_b;
    ni_child_types_pkg::nmu_r_response_t decoded_r;
    wire b_full, b_empty, r_full, r_empty;
    always_comb begin
        decoded_b = '0;
        decoded_b.axi.bid = s_rsp_i.payload[B_BID_LSB +: B_BID_WIDTH];
        decoded_b.axi.bresp = s_rsp_i.payload[B_BRESP_LSB +: B_BRESP_WIDTH];
        decoded_b.meta.is_data = channel == AXI_CH_WIDTH'(AXI_CH_DataB);
        decoded_b.meta.ordering_req = s_rsp_i.header[ORDERING_REQ_LSB];
        decoded_b.meta.ordering_tag = s_rsp_i.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
        decoded_r = '0;
        decoded_r.axi.rid = s_rsp_i.payload[NARROW_R_RID_LSB +: NARROW_R_RID_WIDTH];
        decoded_r.axi.rresp = s_rsp_i.payload[NARROW_R_RRESP_LSB +: NARROW_R_RRESP_WIDTH];
        decoded_r.axi.rlast = s_rsp_i.payload[NARROW_R_RLAST_LSB];
        decoded_r.axi.rdata = DATA_WIDTH'(s_rsp_i.payload[NARROW_R_RDATA_LSB +: NARROW_R_RDATA_WIDTH]);
        decoded_r.meta.ordering_req = s_rsp_i.header[ORDERING_REQ_LSB];
        decoded_r.meta.ordering_tag = s_rsp_i.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
    end
    assign s_rsp_ready_o = !rst_i && ((is_b && !b_full) || (is_r && !r_full));
    assign m_b_valid_o = !rst_i && !b_empty;
    assign m_r_valid_o = !rst_i && !r_empty;
    cc_fifo #(.Depth(FIFO_DEPTH), .FallThrough(1'b0),
        .data_t(ni_child_types_pkg::nmu_b_response_t)) i_b_fifo (
        .clk_i, .rst_ni(1'b1), .flush_i(rst_i), .clr_i(1'b0),
        .full_o(b_full), .empty_o(b_empty), .usage_o(),
        .data_i(decoded_b), .push_i(s_rsp_valid_i && s_rsp_ready_o && is_b),
        .data_o(m_b_o), .pop_i(m_b_valid_o && m_b_ready_i)
    );
    cc_fifo #(.Depth(FIFO_DEPTH), .FallThrough(1'b0),
        .data_t(ni_child_types_pkg::nmu_r_response_t)) i_r_fifo (
        .clk_i, .rst_ni(1'b1), .flush_i(rst_i), .clr_i(1'b0),
        .full_o(r_full), .empty_o(r_empty), .usage_o(),
        .data_i(decoded_r), .push_i(s_rsp_valid_i && s_rsp_ready_o && is_r),
        .data_o(m_r_o), .pop_i(m_r_valid_o && m_r_ready_i)
    );
    // synthesis translate_off
    always @(posedge clk_i) begin
        if (!rst_i && s_rsp_valid_i && !is_b && !is_r)
            $fatal(1, "invalid channel on NMU RSP ingress");
    end
    // synthesis translate_on
endmodule
`resetall
