// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

module nmu_request_buffer #(
    parameter int unsigned REQ_FIFO_DEPTH  = 32,
    parameter int unsigned DAT_FIFO_DEPTH  = 32,
    parameter int unsigned NUM_DAT_VC      = ni_params_pkg::NUM_DAT_VC,
    parameter int unsigned DAT_VC_MODE     = ni_params_pkg::NOC_DAT_VC_MODE,
    parameter int unsigned ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH
) (
    input  wire logic                                    clk_i,
    input  wire logic                                    rst_n_i,
    input  wire ni_flit_pkg::req_flit_t                  s_req_i,
    input  wire logic                                    s_req_valid_i,
    output wire logic                                    s_req_ready_o,
    output wire ni_flit_pkg::req_flit_t                  m_req_o,
    output wire logic                                    m_req_valid_o,
    input  wire logic                                    m_req_ready_i,
    input  wire ni_flit_pkg::dat_flit_t                  s_dat_i,
    input  wire logic                                    s_dat_valid_i,
    output wire logic                   [NUM_DAT_VC-1:0] dat_ready_o,
    output wire ni_flit_pkg::dat_flit_t                  m_dat_o,
    output wire logic                                    m_dat_valid_o,
    input  wire logic                   [NUM_DAT_VC-1:0] dat_credit_return_i
);
    import ni_flit_pkg::*;
    localparam int unsigned NUM_WR_VC = NUM_DAT_VC < 2 ? 1 :
        DAT_VC_MODE == 1 ? NUM_DAT_VC/2 : NUM_DAT_VC;
    localparam int unsigned VC_IDX_W = NUM_WR_VC > 1 ? $clog2(NUM_WR_VC) : 1;
    if (REQ_FIFO_DEPTH < 1 || DAT_FIFO_DEPTH < 1) begin : gen_invalid_depth
        initial $fatal(0, "Error: TX FIFO depths must be positive (instance %m)");
    end
    if (NUM_DAT_VC < 1 || NUM_DAT_VC > (1 << VC_ID_WIDTH) || DAT_VC_MODE > 1 ||
            (DAT_VC_MODE == 1 && (NUM_DAT_VC < 2 || NUM_DAT_VC % 2 != 0))) begin : gen_invalid_vc
        initial $fatal(0, "Error: invalid DAT VC configuration (instance %m)");
    end
    if (ROUTER_VC_DEPTH < 2 || (ROUTER_VC_DEPTH & (ROUTER_VC_DEPTH-1)) != 0) begin : gen_invalid_credit_depth
        initial $fatal(0, "Error: ROUTER_VC_DEPTH must be a power of two >= 2 (instance %m)");
    end
    wire req_full, req_empty;
    wire req_flit_t req_head;
    assign s_req_ready_o = rst_n_i && !req_full;
    assign m_req_valid_o = rst_n_i && !req_empty;
    assign m_req_o       = m_req_valid_o ? req_head : '0;
    cc_fifo #(
        .Depth       (REQ_FIFO_DEPTH),
        .FallThrough (1'b0          ),
        .data_t      (req_flit_t    )
    ) i_req_fifo (
        .clk_i   (clk_i                         ),
        .rst_ni  (rst_n_i                       ),
        .clr_i   (1'b0                          ),
        .flush_i (1'b0                          ),
        .full_o  (req_full                      ),
        .empty_o (req_empty                     ),
        .usage_o (                              ),
        .data_i  (s_req_i                       ),
        .push_i  (s_req_valid_i && s_req_ready_o),
        .data_o  (req_head                      ),
        .pop_i   (m_req_valid_o && m_req_ready_i)
    );
    wire [VC_ID_WIDTH-1:0] wr_vc = s_dat_i.header[VC_ID_LSB +: VC_ID_WIDTH];
    wire [NUM_WR_VC-1:0] dat_full, dat_empty, dat_pop, dat_grant, dat_req, credit_left;
    wire dat_flit_t [NUM_WR_VC-1:0] dat_head;
    wire dat_flit_t dat_sel;
    wire dat_valid;
    assign dat_pop = dat_grant & dat_req;
    for (genvar vc = 0; vc < NUM_DAT_VC; vc++) begin : gen_dat_vc
        if (vc < NUM_WR_VC) begin : gen_write
            assign dat_ready_o[vc] = rst_n_i && !dat_full[vc];
            assign dat_req[vc] = rst_n_i && !dat_empty[vc] &&
                (credit_left[vc] || dat_credit_return_i[vc]);
            cc_fifo #(
                .Depth       (DAT_FIFO_DEPTH),
                .FallThrough (1'b0          ),
                .data_t      (dat_flit_t    )
            ) i_fifo (
                .clk_i   (clk_i),
                .rst_ni  (rst_n_i),
                .clr_i   (1'b0),
                .flush_i (1'b0),
                .full_o  (dat_full[vc]),
                .empty_o (dat_empty[vc]),
                .usage_o (),
                .data_i  (s_dat_i),
                .push_i  (s_dat_valid_i && dat_ready_o[vc] && wr_vc == VC_ID_WIDTH'(vc)),
                .data_o  (dat_head[vc]),
                .pop_i   (dat_pop[vc])
            );
            cc_credit_counter #(
                .NumCredits (ROUTER_VC_DEPTH)
            ) i_credit (
                .clk_i         (clk_i                             ),
                .rst_ni        (rst_n_i                           ),
                .clr_i         (1'b0                              ),
                .credit_o      (                                  ),
                .credit_give_i (rst_n_i && dat_credit_return_i[vc]),
                .credit_take_i (dat_pop[vc]                       ),
                .credit_left_o (credit_left[vc]                   ),
                .credit_crit_o (                                  ),
                .credit_full_o (                                  )
            );
        end else begin : gen_unused
            assign dat_ready_o[vc] = 1'b0;
        end
    end
    rr_arb_tree #(
        .NumIn     (NUM_WR_VC ),
        .DataType  (dat_flit_t),
        .AxiVldRdy (1'b1      ),
        .LockIn    (1'b0      ),
        .FairArb   (1'b1      )
    ) i_dat_arb (
        .clk_i   (clk_i    ),
        .rst_ni  (rst_n_i  ),
        .flush_i (1'b0     ),
        .rr_i    ('0       ),
        .req_i   (dat_req  ),
        .gnt_o   (dat_grant),
        .data_i  (dat_head ),
        .req_o   (dat_valid),
        .gnt_i   (rst_n_i  ),
        .data_o  (dat_sel  ),
        .idx_o   (         )
    );
    assign m_dat_valid_o = rst_n_i && dat_valid;
    assign m_dat_o       = m_dat_valid_o ? dat_sel : '0;
    // synthesis translate_off
    always @(posedge clk_i) begin
        if (rst_n_i && s_dat_valid_i) begin
            if ($isunknown(wr_vc) || int'(wr_vc) >= NUM_WR_VC)
                $fatal(1, "invalid TX DAT VC");
            else if (!dat_ready_o[VC_IDX_W'(wr_vc)])
                $fatal(1, "TX DAT FIFO overflow");
        end
    end
    // synthesis translate_on
endmodule
`resetall
