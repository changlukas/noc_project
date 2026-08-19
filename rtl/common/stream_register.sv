// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Ready/valid adapter selecting bypass, simple, or spill register storage. */
module stream_register #(
    // 0: bypass, 1: simple register, 2: full spill register.
    parameter int unsigned REG_TYPE = 0,
    // Complete transaction record type.
    parameter type T = logic
) (
    input  wire logic  clk_i,
    input  wire logic  rst_ni,
    input  wire logic  s_valid_i,
    output wire logic  s_ready_o,
    input  wire T      s_data_i,
    output wire logic  m_valid_o,
    input  wire logic  m_ready_i,
    output wire T      m_data_o
);

    if (REG_TYPE > 2) begin : gen_invalid_reg_type
        initial $fatal(0, "Error: REG_TYPE must be 0, 1, or 2 (instance %m)");
    end

    if (REG_TYPE == 0) begin : gen_bypass
        assign s_ready_o = m_ready_i;
        assign m_valid_o = s_valid_i;
        assign m_data_o = s_data_i;
    end else if (REG_TYPE == 1) begin : gen_simple
        cc_stream_register #(
            .data_t ( T )
        ) i_cc_stream_register (
            .clk_i,
            .rst_ni,
            .clr_i      ( 1'b0      ),
            .valid_i    ( s_valid_i ),
            .ready_o    ( s_ready_o ),
            .data_i     ( s_data_i  ),
            .valid_o    ( m_valid_o ),
            .ready_i    ( m_ready_i ),
            .data_o     ( m_data_o  )
        );
    end else begin : gen_spill
        cc_spill_register #(
            .data_t ( T )
        ) i_cc_spill_register (
            .clk_i,
            .rst_ni,
            .clr_i   ( 1'b0      ),
            .valid_i ( s_valid_i ),
            .ready_o ( s_ready_o ),
            .data_i  ( s_data_i  ),
            .valid_o ( m_valid_o ),
            .ready_i ( m_ready_i ),
            .data_o  ( m_data_o  )
        );
    end

endmodule

`resetall
