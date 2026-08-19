`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_sam_guards #(
    parameter int unsigned INVALID_CASE = 0
);

    import topology_pkg::*;

    localparam int unsigned AW_REG_TYPE = INVALID_CASE == 0 ? 3 : 0;
    localparam int unsigned AR_REG_TYPE = INVALID_CASE == 1 ? 3 : 0;

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

    nmu_sam #(
        .AW_SAM_REG_TYPE ( AW_REG_TYPE   ),
        .AR_SAM_REG_TYPE ( AR_REG_TYPE   ),
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

    // verilator lint_off BLKSEQ
    always #5ns clk = !clk;
    // verilator lint_on BLKSEQ

    initial begin
        if (INVALID_CASE < 2) begin
            #1ps;
            $finish;
        end

        repeat (2) @(posedge clk);
        @(negedge clk);
        rst_ni = 1'b1;

        case (INVALID_CASE)
            2: begin
                s_aw = '0;
                s_aw.axi.awaddr = 48'h0000_01ff_ffc0;
                s_aw.axi.awlen = 8'd1;
                s_aw.axi.awsize = 3'd6;
                s_aw.axi.awburst = 2'd1;
                s_aw_valid = 1'b1;
            end
            3: begin
                s_ar = '0;
                s_ar.araddr = 48'h0000_01ff_ffc0;
                s_ar.arlen = 8'd1;
                s_ar.arsize = 3'd6;
                s_ar.arburst = 2'd1;
                s_ar_valid = 1'b1;
            end
            4: begin
                s_aw = '0;
                s_aw.axi.awaddr = 48'h0000_0000_0080;
                s_aw.awuser[9:8] = 2'd2;
                s_aw_valid = 1'b1;
            end
            5: begin
                s_aw = '0;
                s_aw.axi.awaddr = 48'hffff_ffff_f000;
                s_aw_valid = 1'b1;
            end
            6: begin
                s_ar = '0;
                s_ar.araddr = 48'hffff_ffff_f000;
                s_ar_valid = 1'b1;
            end
            default: begin
                s_aw = '0;
                s_aw.axi.awaddr = 48'h0004_0000_0080;
                s_aw.awuser[9:8] = 2'd1;
                s_aw.awuser[57:10] = 48'h0003_0000_0000;
                s_aw_valid = 1'b1;
            end
        endcase

        @(posedge clk);
        #1ps;
        $fatal(1, "Invalid nmu_sam case %0d did not fail", INVALID_CASE);
    end

endmodule

`resetall
