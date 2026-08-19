`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_sam;

    import topology_pkg::*;

    localparam int unsigned NUM_MODE_PAIRS = 9;

    logic clk;
    logic rst_ni;

    logic [NUM_MODE_PAIRS-1:0] s_aw_valid;
    logic [NUM_MODE_PAIRS-1:0] s_aw_ready;
    ni_child_types_pkg::nmu_sam_aw_t [NUM_MODE_PAIRS-1:0] s_aw;
    logic [NUM_MODE_PAIRS-1:0] m_aw_valid;
    logic [NUM_MODE_PAIRS-1:0] m_aw_ready;
    ni_child_types_pkg::nmu_sam_aw_result_t [NUM_MODE_PAIRS-1:0] m_aw;

    logic [NUM_MODE_PAIRS-1:0] s_ar_valid;
    logic [NUM_MODE_PAIRS-1:0] s_ar_ready;
    ni_signals_pkg::axi_ar_t [NUM_MODE_PAIRS-1:0] s_ar;
    logic [NUM_MODE_PAIRS-1:0] m_ar_valid;
    logic [NUM_MODE_PAIRS-1:0] m_ar_ready;
    ni_child_types_pkg::nmu_sam_ar_result_t [NUM_MODE_PAIRS-1:0] m_ar;

    for (genvar n = 0; n < NUM_MODE_PAIRS; n++) begin : gen_mode_pairs
        nmu_sam #(
            .AW_SAM_REG_TYPE ( n / 3 ),
            .AR_SAM_REG_TYPE ( n % 3 ),
            .SAM_NUM_RULES   ( SAM_NUM_RULES  ),
            .addr_t          ( sam_addr_t     ),
            .sam_mask_sel_t  ( sam_mask_sel_t ),
            .sam_idx_t       ( sam_idx_t      ),
            .sam_rule_t      ( sam_rule_t     ),
            .SAM             ( SAM            )
        ) dut (
            .noc_clk_i      ( clk            ),
            .noc_rst_ni      ( rst_ni         ),
            .s_aw_valid_i    ( s_aw_valid[n]  ),
            .s_aw_ready_o    ( s_aw_ready[n]  ),
            .s_aw_i          ( s_aw[n]        ),
            .m_aw_valid_o    ( m_aw_valid[n]  ),
            .m_aw_ready_i    ( m_aw_ready[n]  ),
            .m_aw_o          ( m_aw[n]        ),
            .s_ar_valid_i    ( s_ar_valid[n]  ),
            .s_ar_ready_o    ( s_ar_ready[n]  ),
            .s_ar_i          ( s_ar[n]        ),
            .m_ar_valid_o    ( m_ar_valid[n]  ),
            .m_ar_ready_i    ( m_ar_ready[n]  ),
            .m_ar_o          ( m_ar[n]        )
        );
    end

    // verilator lint_off BLKSEQ
    always #5ns clk = !clk;
    // verilator lint_on BLKSEQ

    task automatic check_aw(input int unsigned n);
        assert (m_aw[n].axi == s_aw[n].axi)
            else $fatal(1, "AW address record changed in SAM path");
        assert (m_aw[n].route.route.domain.dst_id == SAM[SAM_NUM_RULES-1].idx.dst_id &&
                m_aw[n].route.route.domain.dst_port_id == SAM[SAM_NUM_RULES-1].idx.dst_port_id &&
                m_aw[n].route.route.domain.is_data == SAM[SAM_NUM_RULES-1].idx.is_data)
            else $fatal(1, "AW route/class metadata did not match the authored-first rule");
        assert (m_aw[n].route.user == 8'hA5 &&
                m_aw[n].route.collective_op == 2'd1 &&
                m_aw[n].route.collective_mask == 8'h11)
            else $fatal(1, "AW collective metadata was lost or translated incorrectly");
    endtask

    task automatic check_ar(input int unsigned n);
        assert (m_ar[n].axi == s_ar[n])
            else $fatal(1, "AR address record changed in SAM path");
        assert (m_ar[n].route.domain.dst_id == SAM[3].idx.dst_id &&
                m_ar[n].route.domain.dst_port_id == SAM[3].idx.dst_port_id &&
                m_ar[n].route.domain.is_data == SAM[3].idx.is_data)
            else $fatal(1, "AR route/class metadata did not match the configuration rule");
    endtask

    initial begin
        clk = 1'b0;
        rst_ni = 1'b0;
        s_aw_valid = '0;
        s_ar_valid = '0;
        m_aw_ready = '0;
        m_ar_ready = '0;
        s_aw = '{default: '0};
        s_ar = '{default: '0};

        repeat (2) @(posedge clk);
        rst_ni = 1'b1;

        for (int unsigned n = 0; n < NUM_MODE_PAIRS; n++) begin
            s_aw[n].axi.awaddr = 48'h0000_0000_0000;
            s_aw[n].awuser[7:0] = 8'hA5;
            s_aw[n].awuser[9:8] = 2'd1;
            s_aw[n].awuser[57:10] = 48'h0003_0000_0000;
            s_ar[n].araddr = 48'h0000_0200_0000;
        end
        s_aw_valid = '1;
        s_ar_valid = '1;

        // Type 0 is immediately visible; types 1 and 2 capture on this edge.
        @(posedge clk);
        #1ps;
        for (int unsigned n = 0; n < NUM_MODE_PAIRS; n++) begin
            assert (m_aw_valid[n] && m_ar_valid[n])
                else $fatal(1, "SAM slice did not present both decoded streams");
            check_aw(n);
            check_ar(n);
        end

        // A stalled AW must not block an independently ready AR in the same mode pair.
        s_ar_valid[4] = 1'b0;
        m_ar_ready[4] = 1'b1;
        @(posedge clk);
        #1ps;
        assert (m_aw_valid[4])
            else $fatal(1, "AW stalled state was not retained");
        assert (!m_ar_valid[4])
            else $fatal(1, "AR did not progress independently of the stalled AW");
        check_aw(4);

        $display("PASS: nmu_sam decode and independent timing cuts");
        $finish;
    end

endmodule

`resetall
