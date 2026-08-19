`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_sam_guards #(
    parameter int unsigned INVALID_CASE = 0
);

    import topology_pkg::*;

    logic clk = 1'b0;
    logic rst_ni = 1'b0;
    logic s_aw_valid = 1'b0;
    logic s_aw_ready;
    ni_child_types_pkg::nmu_sam_aw_t s_aw = '0;
    logic m_aw_valid;
    logic m_aw_ready = 1'b0;
    ni_child_types_pkg::nmu_sam_aw_result_t m_aw;
    logic s_ar_valid = 1'b0;
    logic s_ar_ready;
    ni_signals_pkg::axi_ar_t s_ar = '0;
    logic m_ar_valid;
    logic m_ar_ready = 1'b0;
    ni_child_types_pkg::nmu_sam_ar_result_t m_ar;

    if (INVALID_CASE == 0) begin : gen_invalid_aw_reg_type
        nmu_sam #(
            .AW_SAM_REG_TYPE ( 3              ),
            .AR_SAM_REG_TYPE ( 0              ),
            .SAM_NUM_RULES   ( SAM_NUM_RULES  ),
            .addr_t          ( sam_addr_t     ),
            .sam_mask_sel_t  ( sam_mask_sel_t ),
            .sam_idx_t       ( sam_idx_t      ),
            .sam_rule_t      ( sam_rule_t     ),
            .SAM             ( SAM            )
        ) dut (
            .noc_clk_i       ( clk        ),
            .noc_rst_ni      ( rst_ni     ),
            .s_aw_valid_i    ( s_aw_valid ),
            .s_aw_ready_o    ( s_aw_ready ),
            .s_aw_i          ( s_aw       ),
            .m_aw_valid_o    ( m_aw_valid ),
            .m_aw_ready_i    ( m_aw_ready ),
            .m_aw_o          ( m_aw       ),
            .s_ar_valid_i    ( s_ar_valid ),
            .s_ar_ready_o    ( s_ar_ready ),
            .s_ar_i          ( s_ar       ),
            .m_ar_valid_o    ( m_ar_valid ),
            .m_ar_ready_i    ( m_ar_ready ),
            .m_ar_o          ( m_ar       )
        );
    end else begin : gen_invalid_ar_reg_type
        nmu_sam #(
            .AW_SAM_REG_TYPE ( 0              ),
            .AR_SAM_REG_TYPE ( 3              ),
            .SAM_NUM_RULES   ( SAM_NUM_RULES  ),
            .addr_t          ( sam_addr_t     ),
            .sam_mask_sel_t  ( sam_mask_sel_t ),
            .sam_idx_t       ( sam_idx_t      ),
            .sam_rule_t      ( sam_rule_t     ),
            .SAM             ( SAM            )
        ) dut (
            .noc_clk_i       ( clk        ),
            .noc_rst_ni      ( rst_ni     ),
            .s_aw_valid_i    ( s_aw_valid ),
            .s_aw_ready_o    ( s_aw_ready ),
            .s_aw_i          ( s_aw       ),
            .m_aw_valid_o    ( m_aw_valid ),
            .m_aw_ready_i    ( m_aw_ready ),
            .m_aw_o          ( m_aw       ),
            .s_ar_valid_i    ( s_ar_valid ),
            .s_ar_ready_o    ( s_ar_ready ),
            .s_ar_i          ( s_ar       ),
            .m_ar_valid_o    ( m_ar_valid ),
            .m_ar_ready_i    ( m_ar_ready ),
            .m_ar_o          ( m_ar       )
        );
    end

    initial #1ps $finish;

endmodule

`resetall
