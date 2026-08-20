// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* NMU response-path boundary from NoC response flits to AXI response records.
 * This direction mirrors the NSU response path, which carries B/R to RSP/DAT.
 */
module nmu_response_path (
    // s_rsp_i/s_rsp_valid_i: Router LOCAL RSP -> depacketize/RoB. s_rsp_ready_o: depacketize/RoB -> Router.
    input  wire ni_flit_pkg::rsp_flit_t s_rsp_i,
    input  wire logic                   s_rsp_valid_i,
    output wire logic                   s_rsp_ready_o,
    // s_dat_i/s_dat_valid_i: Router LOCAL DAT -> depacketize/RoB. s_dat_ready_o: depacketize/RoB -> Router.
    input  wire ni_flit_pkg::dat_flit_t s_dat_i,
    input  wire logic                   s_dat_valid_i,
    output wire logic                   s_dat_ready_o,
    // m_b_o/m_b_valid_o: response RoB -> nmu_axi_cdc. m_b_ready_i: nmu_axi_cdc -> response RoB.
    output wire ni_signals_pkg::axi_b_t m_b_o,
    output wire logic                   m_b_valid_o,
    input  wire logic                   m_b_ready_i,
    // m_r_o/m_r_valid_o: response RoB -> nmu_axi_cdc. m_r_ready_i: nmu_axi_cdc -> response RoB.
    output wire ni_signals_pkg::axi_r_t m_r_o,
    output wire logic                   m_r_valid_o,
    input  wire logic                   m_r_ready_i
);

endmodule

`resetall
