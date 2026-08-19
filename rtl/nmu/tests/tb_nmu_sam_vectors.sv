// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Generated SAM vector and mode-0 AW/AR independence checks.
module tb_nmu_sam_vectors;

    import topology_pkg::*;

    logic clk = 1'b0;
    logic rst_ni = 1'b0;
    logic s_aw_valid = 1'b0;
    logic s_aw_ready;
    ni_child_types_pkg::nmu_sam_aw_t s_aw = '0;
    logic m_aw_valid;
    logic m_aw_ready = 1'b1;
    ni_child_types_pkg::nmu_sam_aw_result_t m_aw;
    logic s_ar_valid = 1'b0;
    logic s_ar_ready;
    ni_signals_pkg::axi_ar_t s_ar = '0;
    logic m_ar_valid;
    logic m_ar_ready = 1'b1;
    ni_child_types_pkg::nmu_sam_ar_result_t m_ar;

    nmu_sam #(
        .AW_SAM_REG_TYPE ( 0              ),
        .AR_SAM_REG_TYPE ( 0              ),
        .SAM_NUM_RULES   ( SAM_NUM_RULES  ),
        .addr_t          ( sam_addr_t     ),
        .sam_mask_sel_t  ( sam_mask_sel_t ),
        .sam_idx_t       ( sam_idx_t      ),
        .sam_rule_t      ( sam_rule_t     ),
        .SAM             ( SAM            )
    ) dut (
        .noc_clk_i    ( clk        ),
        .noc_rst_ni   ( rst_ni     ),
        .s_aw_valid_i ( s_aw_valid ),
        .s_aw_ready_o ( s_aw_ready ),
        .s_aw_i       ( s_aw       ),
        .m_aw_valid_o ( m_aw_valid ),
        .m_aw_ready_i ( m_aw_ready ),
        .m_aw_o       ( m_aw       ),
        .s_ar_valid_i ( s_ar_valid ),
        .s_ar_ready_o ( s_ar_ready ),
        .s_ar_i       ( s_ar       ),
        .m_ar_valid_o ( m_ar_valid ),
        .m_ar_ready_i ( m_ar_ready ),
        .m_ar_o       ( m_ar       )
    );

    function automatic sam_addr_t rule_address(
        input int unsigned rule_index,
        input int unsigned point
    );
        case (point)
            0: rule_address = SAM[rule_index].start_addr;
            1: rule_address = SAM[rule_index].start_addr +
                (SAM[rule_index].end_addr - SAM[rule_index].start_addr) / 2;
            default: rule_address = SAM[rule_index].end_addr - 1;
        endcase
    endfunction

    task automatic check_pair(
        input int unsigned aw_rule, input int unsigned ar_rule, input int unsigned point
    );
        s_aw = '0;
        s_ar = '0;
        s_aw.axi.awid = ni_params_pkg::NOC_ID_WIDTH_DFLT'(aw_rule);
        s_aw.axi.awaddr = rule_address(aw_rule, point);
        s_aw.awuser[7:0] = 8'h80 + 8'(aw_rule);
        s_ar.arid = ni_params_pkg::NOC_ID_WIDTH_DFLT'(ar_rule);
        s_ar.araddr = rule_address(ar_rule, point);
        s_aw_valid = 1'b1;
        s_ar_valid = 1'b1;
        #1ps;
        assert (s_aw_ready && s_ar_ready && m_aw_valid && m_ar_valid)
            else $fatal(1, "Mode-0 AW/AR pair did not remain independent");
        assert (m_aw.axi == s_aw.axi && m_aw.route.route.domain.dst_id == SAM[aw_rule].idx.dst_id &&
                m_aw.route.route.domain.dst_port_id == SAM[aw_rule].idx.dst_port_id &&
                m_aw.route.route.domain.is_data == SAM[aw_rule].idx.is_data &&
                m_aw.route.user == s_aw.awuser[7:0] && m_aw.route.collective_op == '0 &&
                m_aw.route.collective_mask == '0)
            else $fatal(1, "AW vector %0d point %0d lost address or SAM metadata", aw_rule, point);
        assert (m_ar.axi == s_ar && m_ar.route.domain.dst_id == SAM[ar_rule].idx.dst_id &&
                m_ar.route.domain.dst_port_id == SAM[ar_rule].idx.dst_port_id &&
                m_ar.route.domain.is_data == SAM[ar_rule].idx.is_data)
            else $fatal(1, "AR vector %0d point %0d lost address or SAM metadata", ar_rule, point);
    endtask

    initial begin
        #1ps;
        assert (!s_aw_ready && !s_ar_ready && !m_aw_valid && !m_ar_valid)
            else $fatal(1, "SAM vectors exposed traffic during reset");
        rst_ni = 1'b1;
        for (int unsigned rule_index = 0; rule_index < SAM_NUM_RULES; rule_index++) begin
            for (int unsigned point = 0; point < 3; point++) begin
                check_pair(rule_index, (rule_index + 1) % SAM_NUM_RULES, point);
            end
        end
        s_aw_valid = 1'b0;
        s_ar_valid = 1'b0;
        #1ps;
        assert (!m_aw_valid && !m_ar_valid) else $fatal(1, "SAM vectors left a stale transfer");
        $display("PASS: nmu_sam generated vectors, boundaries, and AW/AR independence");
        $finish;
    end
endmodule

`resetall
