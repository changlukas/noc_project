// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

module nmu_rx_channel_assign #(
    parameter int unsigned NUM_DAT_VC = ni_params_pkg::NUM_DAT_VC
) (
    input  wire logic                                     clk_i,
    input  wire logic                                     rst_n_i,
    input  wire ni_flit_pkg::rsp_flit_t                   s_rsp_i,
    input  wire logic                                     s_rsp_valid_i,
    output wire logic                                     s_rsp_ready_o,
    input  wire ni_flit_pkg::dat_flit_t  [NUM_DAT_VC-1:0] s_dat_i,
    input  wire logic                    [NUM_DAT_VC-1:0] s_dat_valid_i,
    output wire logic                    [NUM_DAT_VC-1:0] s_dat_ready_o,
    output wire ni_flit_pkg::rsp_flit_t                   m_b_o,
    output wire logic                                     m_b_valid_o,
    input  wire logic                                     m_b_ready_i,
    output wire ni_flit_pkg::dat_flit_t                   m_r_o,
    output wire logic                                     m_r_valid_o,
    input  wire logic                                     m_r_ready_i
);
    import ni_flit_pkg::*;
    if (NUM_DAT_VC < 1 || NUM_DAT_VC > (1 << VC_ID_WIDTH)) begin : gen_invalid_vcs
        initial $fatal(0, "NUM_DAT_VC is outside the encoded VC range");
    end
    wire [AXI_CH_WIDTH-1:0] channel = s_rsp_i.header[AXI_CH_LSB +: AXI_CH_WIDTH];
    wire is_b = channel == AXI_CH_WIDTH'(AXI_CH_NarrowB) ||
                channel == AXI_CH_WIDTH'(AXI_CH_DataB);
    wire is_r = channel == AXI_CH_WIDTH'(AXI_CH_NarrowR);
    wire ni_flit_pkg::dat_flit_t [NUM_DAT_VC:0] r_data;
    wire ni_flit_pkg::dat_flit_t                r_sel_data;
    wire                       [NUM_DAT_VC:0] r_valid, r_ready;
    wire                                      r_sel_valid;

    // Only the RSP head participates; a blocked head holds later B and R flits.
    assign m_b_valid_o   = rst_n_i && s_rsp_valid_i && is_b;
    assign m_b_o         = m_b_valid_o ? s_rsp_i : '0;
    assign s_rsp_ready_o = rst_n_i && ((is_b && m_b_ready_i) || (is_r && r_ready[0]));
    assign r_data[0]     = '{header: s_rsp_i.header, payload: PAYLOAD_WIDTH'(s_rsp_i.payload)};
    assign r_valid[0]    = rst_n_i && s_rsp_valid_i && is_r;
    assign m_r_valid_o   = rst_n_i && r_sel_valid;
    assign m_r_o         = m_r_valid_o ? r_sel_data : '0;
    for (genvar vc = 0; vc < NUM_DAT_VC; vc++) begin : gen_dat_vc
        assign r_data[vc+1]      = s_dat_i[vc];
        assign r_valid[vc+1]     = rst_n_i && s_dat_valid_i[vc];
        assign s_dat_ready_o[vc] = rst_n_i && r_ready[vc+1];
    end

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

endmodule
`resetall
