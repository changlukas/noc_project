// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* NMU request-path boundary from AXI request records to NoC request flits.
 * This direction mirrors the NSU request path, which carries REQ/DAT to AW/AR/W.
 */
module nmu_request_path (
    // s_aw_i/s_aw_valid_i: nmu_axi_cdc -> SAM/RoB. s_aw_ready_o: SAM/RoB -> nmu_axi_cdc.
    input  wire ni_signals_pkg::axi_aw_t          s_aw_i,
    input  wire logic                             s_aw_valid_i,
    output wire logic                             s_aw_ready_o,
    // s_w_i/s_w_valid_i: nmu_axi_cdc -> RoB/packetize. s_w_ready_o: RoB/packetize -> nmu_axi_cdc.
    input  wire ni_signals_pkg::axi_w_t           s_w_i,
    input  wire logic                             s_w_valid_i,
    output wire logic                             s_w_ready_o,
    // s_ar_i/s_ar_valid_i: nmu_axi_cdc -> SAM/RoB. s_ar_ready_o: SAM/RoB -> nmu_axi_cdc.
    input  wire ni_signals_pkg::axi_ar_t          s_ar_i,
    input  wire logic                             s_ar_valid_i,
    output wire logic                             s_ar_ready_o,
    // m_req_o/m_req_valid_o: channel assignment -> Router LOCAL REQ. m_req_ready_i: Router -> channel assignment.
    output wire ni_flit_pkg::req_flit_t           m_req_o,
    output wire logic                             m_req_valid_o,
    input  wire logic                             m_req_ready_i,
    // m_dat_o/m_dat_valid_o: channel assignment -> Router LOCAL DAT. m_dat_crdvalid_i: Router -> credit management.
    output wire ni_flit_pkg::dat_flit_t           m_dat_o,
    output wire logic                             m_dat_valid_o,
    input  wire logic [ni_params_pkg::NOC_DAT_NUM_VC_DFLT-1:0] m_dat_crdvalid_i
);

endmodule

`resetall
