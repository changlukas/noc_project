// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// RSP-network response decode. B and NarrowR have independent class queues.
// DAT uses receiver-owned per-VC credits and merges at R-beat granularity.
module nmu_response_depacketize #(
    parameter int unsigned FIFO_DEPTH = ni_params_pkg::NMU_DEPKT_Q_DEPTH_DFLT,
    parameter int unsigned DAT_NUM_VC = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned DAT_VC_MODE = ni_params_pkg::NOC_DAT_VC_MODE_DFLT,
    parameter int unsigned DAT_RX_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT
) (
    input wire logic clk_i,
    input wire logic rst_i,
    input wire ni_flit_pkg::rsp_flit_t s_rsp_i,
    input wire logic s_rsp_valid_i,
    output wire logic s_rsp_ready_o,
    input wire ni_flit_pkg::dat_flit_t s_dat_i,
    input wire logic s_dat_valid_i,
    output wire logic [DAT_NUM_VC-1:0] dat_credit_return_o,
    output wire ni_child_types_pkg::nmu_b_response_t m_b_o,
    output wire logic m_b_valid_o,
    input wire logic m_b_ready_i,
    output wire ni_child_types_pkg::nmu_r_response_t m_r_o,
    output wire logic m_r_valid_o,
    input wire logic m_r_ready_i
);
    import ni_flit_pkg::*;
    localparam int unsigned DATA_W = $bits(m_r_o.axi.rdata);
    if (FIFO_DEPTH < 1 || FIFO_DEPTH > 1024) begin : gen_invalid_depth
        initial $fatal(0, "Error: response FIFO_DEPTH must be in [1, 1024] (instance %m)");
    end
    if (DAT_NUM_VC < 1 || DAT_NUM_VC > (1 << VC_ID_WIDTH)) begin : gen_invalid_vcs
        initial $fatal(0, "DAT_NUM_VC is outside the encoded VC range");
    end
    if (DAT_VC_MODE > 1 || (DAT_VC_MODE == 1 && (DAT_NUM_VC < 2 || DAT_NUM_VC % 2 != 0))) begin : gen_invalid_mode
        initial $fatal(0, "DAT_VC_MODE split requires a positive even VC count");
    end
    if (DAT_RX_VC_DEPTH < 2 || (DAT_RX_VC_DEPTH & (DAT_RX_VC_DEPTH-1)) != 0) begin : gen_invalid_dat_depth
        initial $fatal(0, "DAT_RX_VC_DEPTH must be a power of two and at least 2");
    end
    localparam int unsigned CL_VC = DAT_NUM_VC > 1 ? $clog2(DAT_NUM_VC) : 1;
    localparam int unsigned FIRST_RD_VC = DAT_VC_MODE == 1 ? DAT_NUM_VC/2 : 0;
    wire [VC_ID_WIDTH-1:0] dat_vc = s_dat_i.header[VC_ID_LSB +: VC_ID_WIDTH];
    wire [AXI_CH_WIDTH-1:0] dat_channel = s_dat_i.header[AXI_CH_LSB +: AXI_CH_WIDTH];
    ni_child_types_pkg::nmu_r_response_t decoded_dat_r;
    wire ni_child_types_pkg::nmu_r_response_t [DAT_NUM_VC:0] r_data;
    wire ni_child_types_pkg::nmu_r_response_t r_sel_data;
    wire [DAT_NUM_VC:0] r_valid, r_ready;
    wire [DAT_NUM_VC-1:0] dat_full, dat_empty, dat_push, dat_pop;
    wire r_sel_valid;
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
        decoded_r.axi.rdata = DATA_W'(s_rsp_i.payload[NARROW_R_RDATA_LSB +: NARROW_R_RDATA_WIDTH]);
        decoded_r.meta.ordering_req = s_rsp_i.header[ORDERING_REQ_LSB];
        decoded_r.meta.ordering_tag = s_rsp_i.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
    end
    assign s_rsp_ready_o = !rst_i && ((is_b && !b_full) || (is_r && !r_full));
    assign m_b_valid_o = !rst_i && !b_empty;
    assign r_valid[0] = !rst_i && !r_empty;
    assign m_r_valid_o = !rst_i && r_sel_valid;
    assign m_r_o = m_r_valid_o ? r_sel_data : '0;
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
        .data_o(r_data[0]), .pop_i(r_valid[0] && r_ready[0])
    );
    always_comb begin
        decoded_dat_r = '0;
        decoded_dat_r.axi.rid = s_dat_i.payload[DATA_R_RID_LSB +: DATA_R_RID_WIDTH];
        decoded_dat_r.axi.rresp = s_dat_i.payload[DATA_R_RRESP_LSB +: DATA_R_RRESP_WIDTH];
        decoded_dat_r.axi.rlast = s_dat_i.payload[DATA_R_RLAST_LSB];
        decoded_dat_r.axi.rdata = DATA_W'(s_dat_i.payload[DATA_R_RDATA_LSB +: DATA_R_RDATA_WIDTH]);
        decoded_dat_r.meta.is_data = 1'b1;
        decoded_dat_r.meta.ordering_req = s_dat_i.header[ORDERING_REQ_LSB];
        decoded_dat_r.meta.ordering_tag = s_dat_i.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH];
    end

    for (genvar vc = 0; vc < DAT_NUM_VC; vc++) begin : gen_dat_vc
        if (vc >= FIRST_RD_VC) begin : gen_read
            assign dat_push[vc] = !rst_i && s_dat_valid_i && dat_vc == VC_ID_WIDTH'(vc);
            assign dat_pop[vc] = r_valid[vc+1] && r_ready[vc+1];
            assign r_valid[vc+1] = !rst_i && !dat_empty[vc];
            ni_credit_fifo #(
                .Depth(DAT_RX_VC_DEPTH), .FallThrough(1'b0),
                .data_t(ni_child_types_pkg::nmu_r_response_t)
            ) i_fifo (
                .clk_i, .rst_ni(1'b1), .flush_i(rst_i), .clr_i(1'b0),
                .full_o(dat_full[vc]), .empty_o(dat_empty[vc]), .usage_o(),
                .data_i(decoded_dat_r), .push_i(dat_push[vc]),
                .data_o(r_data[vc+1]), .pop_i(dat_pop[vc])
            );
        end else begin : gen_unused
            assign dat_push[vc] = 1'b0;
            assign dat_pop[vc] = 1'b0;
            assign dat_full[vc] = 1'b0;
            assign dat_empty[vc] = 1'b1;
            assign r_valid[vc+1] = 1'b0;
            assign r_data[vc+1] = '0;
        end
    end
    assign dat_credit_return_o = dat_pop;

    // Hold only a stalled beat; RLAST does not lock the selected VC.
    rr_arb_tree #(
        .NumIn(DAT_NUM_VC+1), .DataType(ni_child_types_pkg::nmu_r_response_t),
        .AxiVldRdy(1'b1), .LockIn(1'b1), .FairArb(1'b1)
    ) i_r_arb (
        .clk_i, .rst_ni(1'b1), .flush_i(rst_i), .rr_i('0),
        .req_i(r_valid), .gnt_o(r_ready), .data_i(r_data),
        .req_o(r_sel_valid), .gnt_i(!rst_i && m_r_ready_i),
        .data_o(r_sel_data), .idx_o()
    );

    // synthesis translate_off
    always @(posedge clk_i) begin
        if (!rst_i && s_dat_valid_i) begin
            if ($isunknown({dat_channel, dat_vc}) || dat_channel != AXI_CH_WIDTH'(AXI_CH_DataR) ||
                    int'(dat_vc) < FIRST_RD_VC || int'(dat_vc) >= DAT_NUM_VC)
                $fatal(1, "invalid channel or VC on NMU DAT ingress");
            if (dat_full[CL_VC'(dat_vc)] && !dat_pop[CL_VC'(dat_vc)])
                $fatal(1, "NMU DAT receive credit overflow");
        end
        if (!rst_i && s_rsp_valid_i && !is_b && !is_r)
            $fatal(1, "invalid channel on NMU RSP ingress");
    end
    // synthesis translate_on
endmodule
`resetall
