// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// Keep one AW context through acceptance of its final W beat.
module nmu_write_context (
    input  wire logic                                                           clk_i,
    input  wire logic                                                           rst_n_i,
    input  wire ni_types_pkg::nmu_aw_request_t                                  s_aw_i,
    input  wire logic                                                           s_aw_valid_i,
    output wire logic                                                           s_aw_ready_o,
    output wire ni_types_pkg::nmu_aw_request_t                                  m_aw_o,
    output wire logic                                                           m_aw_valid_o,
    input  wire logic                                                           m_aw_ready_i,
    input  wire ni_signals_pkg::axi_w_t                                         s_w_i,
    input  wire logic                                                           s_w_valid_i,
    output wire logic                                                           s_w_ready_o,
    output wire ni_signals_pkg::axi_w_t                                         m_w_o,
    output wire logic                                                           m_w_valid_o,
    input  wire logic                                                           m_w_ready_i,
    output wire ni_types_pkg::nmu_aw_request_t                                  m_w_aw_o,
    output wire logic                          [ni_flit_pkg::AXI_LEN_WIDTH-1:0] m_w_beat_o
);
    ni_types_pkg::nmu_aw_request_t aw_reg, aw_next;
    logic active_reg, active_next;
    logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0] beat_reg, beat_next;
    wire w_accept = m_w_valid_o && m_w_ready_i;
    wire aw_space = !active_reg || (w_accept && s_w_i.wlast);
    wire aw_accept = m_aw_valid_o && m_aw_ready_i;
    assign m_aw_o       = m_aw_valid_o ? s_aw_i : '0;
    assign m_aw_valid_o = rst_n_i && aw_space && s_aw_valid_i;
    assign s_aw_ready_o = rst_n_i && aw_space && m_aw_ready_i;
    assign m_w_o        = m_w_valid_o ? s_w_i : '0;
    assign m_w_valid_o  = rst_n_i && active_reg && s_w_valid_i;
    assign s_w_ready_o  = rst_n_i && active_reg && m_w_ready_i;
    assign m_w_aw_o     = active_reg ? aw_reg : '0;
    assign m_w_beat_o   = active_reg ? beat_reg : '0;
    always_comb begin
        aw_next     = aw_reg;
        active_next = active_reg;
        beat_next   = beat_reg;
        if (w_accept) begin
            beat_next = beat_reg + 1'b1;
            if (s_w_i.wlast) begin
                active_next = 1'b0;
                beat_next   = '0;
            end
        end
        if (aw_accept) begin
            aw_next     = s_aw_i;
            active_next = 1'b1;
            beat_next   = '0;
        end
    end
    always @(posedge clk_i or negedge rst_n_i) begin
        if (~rst_n_i) begin
            aw_reg     <= '0;
            active_reg <= '0;
            beat_reg   <= '0;
        end else begin
            aw_reg     <= aw_next;
            active_reg <= active_next;
            beat_reg   <= beat_next;
        end
    end
endmodule
`resetall
