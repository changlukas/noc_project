// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// Raw NoC ingress storage. Channel assignment consumes the FIFO heads.
module nmu_response_buffer #(
    parameter int unsigned RSP_FIFO_DEPTH  = 32,
    parameter int unsigned NUM_DAT_VC      = ni_params_pkg::NUM_DAT_VC,
    parameter int unsigned DAT_VC_MODE     = ni_params_pkg::NOC_DAT_VC_MODE,
    parameter int unsigned DAT_RX_VC_DEPTH = 32
) (
    input  wire logic                                     clk_i,
    input  wire logic                                     rst_n_i,
    input  wire ni_flit_pkg::rsp_flit_t                   s_rsp_i,
    input  wire logic                                     s_rsp_valid_i,
    output wire logic                                     s_rsp_ready_o,
    input  wire ni_flit_pkg::dat_flit_t                   s_dat_i,
    input  wire logic                                     s_dat_valid_i,
    output wire logic                    [NUM_DAT_VC-1:0] dat_credit_return_o,
    output wire ni_flit_pkg::rsp_flit_t                   m_rsp_o,
    output wire logic                                     m_rsp_valid_o,
    input  wire logic                                     m_rsp_ready_i,
    output wire ni_flit_pkg::dat_flit_t  [NUM_DAT_VC-1:0] m_dat_o,
    output wire logic                    [NUM_DAT_VC-1:0] m_dat_valid_o,
    input  wire logic                    [NUM_DAT_VC-1:0] m_dat_ready_i
);
    import ni_flit_pkg::*;
    if (RSP_FIFO_DEPTH < 1 || RSP_FIFO_DEPTH > 1024) begin : gen_invalid_depth
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
    wire [NUM_DAT_VC-1:0] dat_full, dat_empty, dat_push, dat_pop;
    wire ni_flit_pkg::dat_flit_t [NUM_DAT_VC-1:0] dat_head;
    wire [AXI_CH_WIDTH-1:0] channel = s_rsp_i.header[AXI_CH_LSB +: AXI_CH_WIDTH];
    wire is_b = channel == AXI_CH_WIDTH'(AXI_CH_NarrowB) ||
                channel == AXI_CH_WIDTH'(AXI_CH_DataB);
    wire is_r = channel == AXI_CH_WIDTH'(AXI_CH_NarrowR);
    wire rsp_full, rsp_empty;
    wire ni_flit_pkg::rsp_flit_t rsp_head;
    assign s_rsp_ready_o = rst_n_i && !rsp_full;
    assign m_rsp_valid_o = rst_n_i && !rsp_empty;
    assign m_rsp_o       = m_rsp_valid_o ? rsp_head : '0;
    cc_fifo #(
        .Depth       (RSP_FIFO_DEPTH         ),
        .FallThrough (1'b0                   ),
        .data_t      (ni_flit_pkg::rsp_flit_t)
    ) i_rsp_fifo (
        .clk_i   (clk_i                         ),
        .rst_ni  (rst_n_i                       ),
        .flush_i (1'b0                          ),
        .clr_i   (1'b0                          ),
        .full_o  (rsp_full                      ),
        .empty_o (rsp_empty                     ),
        .usage_o (                              ),
        .data_i  (s_rsp_i                       ),
        .push_i  (s_rsp_valid_i && s_rsp_ready_o),
        .data_o  (rsp_head                      ),
        .pop_i   (m_rsp_valid_o && m_rsp_ready_i)
    );
    for (genvar vc = 0; vc < NUM_DAT_VC; vc++) begin : gen_dat_vc
        assign m_dat_o[vc] = m_dat_valid_o[vc] ? dat_head[vc] : '0;
        if (vc >= RD_VC_BASE) begin : gen_read
            assign dat_push[vc]      = rst_n_i && s_dat_valid_i && dat_vc == VC_ID_WIDTH'(vc);
            assign dat_pop[vc]       = m_dat_valid_o[vc] && m_dat_ready_i[vc];
            assign m_dat_valid_o[vc] = rst_n_i && !dat_empty[vc];
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
                .data_o  (dat_head[vc] ),
                .pop_i   (dat_pop[vc]  )
            );
        end else begin : gen_unused
            assign dat_push[vc]      = 1'b0;
            assign dat_pop[vc]       = 1'b0;
            assign dat_full[vc]      = 1'b0;
            assign dat_empty[vc]     = 1'b1;
            assign m_dat_valid_o[vc] = 1'b0;
            assign dat_head[vc]      = '0;
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
