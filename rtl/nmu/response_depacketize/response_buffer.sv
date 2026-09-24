// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// RSP-network response decode. B and NarrowR have independent class queues.
// DAT uses receiver-owned per-VC credits and merges at R-beat granularity.
module nmu_response_buffer #(
    parameter int unsigned RSP_FIFO_DEPTH  = 32,
    parameter int unsigned B_FIFO_DEPTH    = RSP_FIFO_DEPTH,
    parameter int unsigned R_FIFO_DEPTH    = RSP_FIFO_DEPTH,
    parameter int unsigned NUM_DAT_VC      = ni_params_pkg::NUM_DAT_VC,
    parameter int unsigned DAT_VC_MODE     = ni_params_pkg::NOC_DAT_VC_MODE,
    parameter int unsigned DAT_RX_VC_DEPTH = 32
) (
    input  wire logic                                    clk_i,
    input  wire logic                                    rst_n_i,
    input  wire ni_flit_pkg::rsp_flit_t                  s_rsp_i,
    input  wire logic                                    s_rsp_valid_i,
    output wire logic                                    s_rsp_ready_o,
    input  wire ni_flit_pkg::dat_flit_t                  s_dat_i,
    input  wire logic                                    s_dat_valid_i,
    output wire logic                   [NUM_DAT_VC-1:0] dat_credit_return_o,
    output wire ni_flit_pkg::rsp_flit_t                  m_b_o,
    output wire logic                                    m_b_valid_o,
    input  wire logic                                    m_b_ready_i,
    output wire ni_flit_pkg::dat_flit_t                  m_r_o,
    output wire logic                                    m_r_valid_o,
    input  wire logic                                    m_r_ready_i
);
    import ni_flit_pkg::*;
    if (B_FIFO_DEPTH < 1 || B_FIFO_DEPTH > 1024 || R_FIFO_DEPTH < 1 || R_FIFO_DEPTH > 1024) begin : gen_invalid_depth
        initial $fatal(0, "Error: response FIFO depths must be in [1, 1024] (instance %m)");
    end
    if (NUM_DAT_VC < 1 || NUM_DAT_VC > (1 << VC_ID_WIDTH)) begin : gen_invalid_vcs
        initial $fatal(0, "NUM_DAT_VC is outside the encoded VC range");
    end
    if (DAT_VC_MODE > 1 || (DAT_VC_MODE == 1 && (NUM_DAT_VC < 2 || NUM_DAT_VC % 2 != 0))) begin : gen_invalid_mode
        initial $fatal(0, "DAT_VC_MODE split requires a positive even VC count");
    end
    if (DAT_RX_VC_DEPTH < 2 || (DAT_RX_VC_DEPTH & (DAT_RX_VC_DEPTH-1)) != 0) begin : gen_invalid_dat_depth
        initial $fatal(0, "DAT_RX_VC_DEPTH must be a power of two and at least 2");
    end
    localparam int unsigned VC_IDX_W   = NUM_DAT_VC > 1 ? $clog2(NUM_DAT_VC) : 1;
    localparam int unsigned RD_VC_BASE = DAT_VC_MODE == 1 ? NUM_DAT_VC/2 : 0;
    wire [VC_ID_WIDTH-1:0] dat_vc = s_dat_i.header[VC_ID_LSB +: VC_ID_WIDTH];
    wire [AXI_CH_WIDTH-1:0] dat_channel = s_dat_i.header[AXI_CH_LSB +: AXI_CH_WIDTH];
    wire ni_flit_pkg::dat_flit_t   [NUM_DAT_VC:0] r_data;
    wire ni_flit_pkg::dat_flit_t                  r_sel_data;
    wire                           [NUM_DAT_VC:0] r_valid, r_ready;
    wire                         [NUM_DAT_VC-1:0] dat_full, dat_empty, dat_push, dat_pop;
    wire                                          r_sel_valid;
    wire [AXI_CH_WIDTH-1:0] channel = s_rsp_i.header[AXI_CH_LSB +: AXI_CH_WIDTH];
    wire is_b = channel == AXI_CH_WIDTH'(AXI_CH_NarrowB) ||
                channel == AXI_CH_WIDTH'(AXI_CH_DataB);
    wire is_r = channel == AXI_CH_WIDTH'(AXI_CH_NarrowR);
    wire                         b_full, b_empty, r_full, r_empty;
    wire ni_flit_pkg::rsp_flit_t b_flit, narrow_r_flit;
    assign m_b_o         = m_b_valid_o ? b_flit : '0;
    assign r_data[0]     = '{header: narrow_r_flit.header, payload: PAYLOAD_WIDTH'(narrow_r_flit.payload)};
    assign s_rsp_ready_o = rst_n_i && ((is_b && !b_full) || (is_r && !r_full));
    assign m_b_valid_o   = rst_n_i && !b_empty;
    assign r_valid[0]    = rst_n_i && !r_empty;
    assign m_r_valid_o   = rst_n_i && r_sel_valid;
    assign m_r_o         = m_r_valid_o ? r_sel_data : '0;
    cc_fifo #(
        .Depth       (B_FIFO_DEPTH           ),
        .FallThrough (1'b0                   ),
        .data_t      (ni_flit_pkg::rsp_flit_t)
    ) i_b_fifo (
        .clk_i   (clk_i                                 ),
        .rst_ni  (rst_n_i                               ),
        .flush_i (1'b0                                  ),
        .clr_i   (1'b0                                  ),
        .full_o  (b_full                                ),
        .empty_o (b_empty                               ),
        .usage_o (                                      ),
        .data_i  (s_rsp_i                               ),
        .push_i  (s_rsp_valid_i && s_rsp_ready_o && is_b),
        .data_o  (b_flit                                ),
        .pop_i   (m_b_valid_o && m_b_ready_i            )
    );
    cc_fifo #(
        .Depth       (R_FIFO_DEPTH           ),
        .FallThrough (1'b0                   ),
        .data_t      (ni_flit_pkg::rsp_flit_t)
    ) i_r_fifo (
        .clk_i   (clk_i                                 ),
        .rst_ni  (rst_n_i                               ),
        .flush_i (1'b0                                  ),
        .clr_i   (1'b0                                  ),
        .full_o  (r_full                                ),
        .empty_o (r_empty                               ),
        .usage_o (                                      ),
        .data_i  (s_rsp_i                               ),
        .push_i  (s_rsp_valid_i && s_rsp_ready_o && is_r),
        .data_o  (narrow_r_flit                         ),
        .pop_i   (r_valid[0] && r_ready[0]              )
    );
    for (genvar vc = 0; vc < NUM_DAT_VC; vc++) begin : gen_dat_vc
        if (vc >= RD_VC_BASE) begin : gen_read
            assign dat_push[vc] = rst_n_i && s_dat_valid_i && dat_vc == VC_ID_WIDTH'(vc);
            assign dat_pop[vc]  = r_valid[vc+1] && r_ready[vc+1];
            assign r_valid[vc+1] = rst_n_i && !dat_empty[vc];
            cc_fifo #(
                .Depth       (DAT_RX_VC_DEPTH        ),
                .FallThrough (1'b0                   ),
                .data_t      (ni_flit_pkg::dat_flit_t)
            ) i_fifo (
                .clk_i   (clk_i        ),
                .rst_ni  (rst_n_i      ),
                .flush_i (1'b0         ),
                .clr_i   (1'b0         ),
                .full_o  (dat_full[vc] ),
                .empty_o (dat_empty[vc]),
                .usage_o (             ),
                .data_i  (s_dat_i      ),
                .push_i  (dat_push[vc] ),
                .data_o  (r_data[vc+1] ),
                .pop_i   (dat_pop[vc]  )
            );
        end else begin : gen_unused
            assign dat_push[vc]  = 1'b0;
            assign dat_pop[vc]   = 1'b0;
            assign dat_full[vc]  = 1'b0;
            assign dat_empty[vc] = 1'b1;
            assign r_valid[vc+1] = 1'b0;
            assign r_data[vc+1]  = '0;
        end
    end
    logic [NUM_DAT_VC-1:0] credit_return_reg, credit_return_next;
    assign credit_return_next = dat_pop;
    always @(posedge clk_i or negedge rst_n_i) begin
        if (~rst_n_i) begin
            credit_return_reg <= '0;
        end else begin
            credit_return_reg <= credit_return_next;
        end
    end
    assign dat_credit_return_o = ~rst_n_i ? '0 : credit_return_reg;

    // Hold only a stalled beat; RLAST does not lock the selected VC.
    rr_arb_tree #(
        .NumIn     (NUM_DAT_VC+1           ),
        .DataType  (ni_flit_pkg::dat_flit_t),
        .AxiVldRdy (1'b1                   ),
        .LockIn    (1'b1                   ),
        .FairArb   (1'b1                   )
    ) i_r_arb (
        .clk_i   (clk_i                 ),
        .rst_ni  (rst_n_i               ),
        .flush_i (1'b0                  ),
        .rr_i    ('0                    ),
        .req_i   (r_valid               ),
        .gnt_o   (r_ready               ),
        .data_i  (r_data                ),
        .req_o   (r_sel_valid           ),
        .gnt_i   (rst_n_i && m_r_ready_i),
        .data_o  (r_sel_data            ),
        .idx_o   (                      )
    );

    // synthesis translate_off
    always @(posedge clk_i) begin
        if (rst_n_i && s_dat_valid_i) begin
            if ($isunknown({dat_channel, dat_vc}) || dat_channel != AXI_CH_WIDTH'(AXI_CH_DataR) ||
                    int'(dat_vc) < RD_VC_BASE || int'(dat_vc) >= NUM_DAT_VC)
                $fatal(1, "invalid channel or VC on NMU DAT ingress");
            if (dat_full[VC_IDX_W'(dat_vc)])
                $fatal(1, "NMU DAT receive credit overflow");
        end
        if (rst_n_i && s_rsp_valid_i && !is_b && !is_r)
            $fatal(1, "invalid channel on NMU RSP ingress");
    end
    // synthesis translate_on
endmodule
`resetall
