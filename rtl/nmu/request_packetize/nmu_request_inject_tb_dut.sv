// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Packetize REQ/DAT independently; accepted AW order determines W ownership.
module nmu_request_inject_tb_dut #(
    parameter int unsigned                               FIFO_DEPTH      = ni_params_pkg::NOC_FIFO_DEPTH,
    parameter int unsigned                               NUM_DAT_VC      = ni_params_pkg::NUM_DAT_VC,
    parameter int unsigned                               DAT_VC_MODE     = ni_params_pkg::NOC_DAT_VC_MODE,
    parameter int unsigned                               ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH,
    parameter logic [ni_flit_pkg::SRC_ID_WIDTH-1:0]      SRC_ID          = '0,
    parameter logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] SRC_PORT_ID     = '0
) (
    input  wire logic                                           clk_i,
    input  wire logic                                           rst_i,
    input  wire ni_types_pkg::nmu_aw_request_t                  s_aw_i,
    input  wire logic                                           s_aw_valid_i,
    output wire logic                                           s_aw_ready_o,
    input  wire ni_signals_pkg::axi_w_t                         s_w_i,
    input  wire logic                                           s_w_valid_i,
    output wire logic                                           s_w_ready_o,
    input  wire ni_types_pkg::nmu_ar_request_t                  s_ar_i,
    input  wire logic                                           s_ar_valid_i,
    output wire logic                                           s_ar_ready_o,
    output wire ni_flit_pkg::req_flit_t                         m_req_o,
    output wire logic                                           m_req_valid_o,
    input  wire logic                                           m_req_ready_i,
    output wire ni_flit_pkg::dat_flit_t                         m_dat_o,
    output wire logic                                           m_dat_valid_o,
    input  wire logic                          [NUM_DAT_VC-1:0] dat_credit_return_i
);
    import ni_types_pkg::*;

    wire ni_flit_pkg::req_flit_t [NUM_NMU_REQ_CH-1:0] req_candidates;
    wire ni_flit_pkg::dat_flit_t [NUM_NMU_DAT_CH-1:0] dat_candidates;
    wire                         [NUM_NMU_REQ_CH-1:0] req_valid, req_ready;
    wire                         [NUM_NMU_DAT_CH-1:0] dat_valid, dat_ready;
    nmu_request_packetize #(
        .FIFO_DEPTH  (FIFO_DEPTH ),
        .SRC_ID      (SRC_ID     ),
        .SRC_PORT_ID (SRC_PORT_ID)
    ) i_packetize (
        .clk_i         (clk_i         ),
        .rst_i         (rst_i         ),
        .s_aw_i        (s_aw_i        ),
        .s_aw_valid_i  (s_aw_valid_i  ),
        .s_aw_ready_o  (s_aw_ready_o  ),
        .s_w_i         (s_w_i         ),
        .s_w_valid_i   (s_w_valid_i   ),
        .s_w_ready_o   (s_w_ready_o   ),
        .s_ar_i        (s_ar_i        ),
        .s_ar_valid_i  (s_ar_valid_i  ),
        .s_ar_ready_o  (s_ar_ready_o  ),
        .m_req_o       (req_candidates),
        .m_req_valid_o (req_valid     ),
        .m_req_ready_i (req_ready     ),
        .m_dat_o       (dat_candidates),
        .m_dat_valid_o (dat_valid     ),
        .m_dat_ready_i (dat_ready     )
    );
    nmu_channel_assign #(
        .NUM_DAT_VC      (NUM_DAT_VC     ),
        .DAT_VC_MODE     (DAT_VC_MODE    ),
        .ROUTER_VC_DEPTH (ROUTER_VC_DEPTH)
    ) i_channel_assign (
        .clk_i               (clk_i              ),
        .rst_i               (rst_i              ),
        .s_req_i             (req_candidates     ),
        .s_req_valid_i       (req_valid          ),
        .s_req_ready_o       (req_ready          ),
        .s_dat_i             (dat_candidates     ),
        .s_dat_valid_i       (dat_valid          ),
        .s_dat_ready_o       (dat_ready          ),
        .m_req_o             (m_req_o            ),
        .m_req_valid_o       (m_req_valid_o      ),
        .m_req_ready_i       (m_req_ready_i      ),
        .m_dat_o             (m_dat_o            ),
        .m_dat_valid_o       (m_dat_valid_o      ),
        .dat_credit_return_i (dat_credit_return_i)
    );
endmodule
`resetall
