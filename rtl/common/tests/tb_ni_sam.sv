`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_ni_sam;

    import topology_pkg::*;

    localparam sam_idx_t OVERLAP_BROAD_IDX = '{
        dst_id: 8'd1,
        dst_port_id: 2'd0,
        is_data: 1'b1,
        collective_en: 1'b0,
        mask_x: '0,
        mask_y: '0
    };
    localparam sam_idx_t OVERLAP_NARROW_IDX = '{
        dst_id: 8'd2,
        dst_port_id: 2'd0,
        is_data: 1'b0,
        collective_en: 1'b0,
        mask_x: '0,
        mask_y: '0
    };
    localparam sam_rule_t [1:0] OVERLAP_SAM = '{
        1: '{idx: OVERLAP_BROAD_IDX, start_addr: 48'h000, end_addr: 48'h100},
        0: '{idx: OVERLAP_NARROW_IDX, start_addr: 48'h080, end_addr: 48'h0C0}
    };

    sam_addr_t addr;
    logic      lookup_en;
    sam_idx_t  sam_idx;
    logic      lookup_valid;
    logic      lookup_error;

    sam_idx_t overlap_idx;
    logic     overlap_valid;
    logic     overlap_error;

    ni_sam #(
        .SAM_NUM_RULES  ( SAM_NUM_RULES  ),
        .addr_t         ( sam_addr_t     ),
        .sam_mask_sel_t ( sam_mask_sel_t ),
        .sam_idx_t      ( sam_idx_t      ),
        .sam_rule_t     ( sam_rule_t     ),
        .SAM            ( SAM            )
    ) i_ni_sam (
        .addr_i         ( addr         ),
        .lookup_en_i    ( lookup_en    ),
        .sam_idx_o      ( sam_idx      ),
        .lookup_valid_o ( lookup_valid ),
        .lookup_error_o ( lookup_error )
    );

    ni_sam #(
        .SAM_NUM_RULES  ( 2              ),
        .addr_t         ( sam_addr_t     ),
        .sam_mask_sel_t ( sam_mask_sel_t ),
        .sam_idx_t      ( sam_idx_t      ),
        .sam_rule_t     ( sam_rule_t     ),
        .SAM            ( OVERLAP_SAM    )
    ) i_overlap_ni_sam (
        .addr_i         ( addr          ),
        .lookup_en_i    ( lookup_en     ),
        .sam_idx_o      ( overlap_idx   ),
        .lookup_valid_o ( overlap_valid ),
        .lookup_error_o ( overlap_error )
    );

    initial begin
        addr = 48'h0000_0000_0000;
        lookup_en = 1'b0;
        #1ps;
        assert (sam_idx == '0 && !lookup_valid && !lookup_error)
            else $fatal(1, "Disabled lookup exposed a decoder result");

        lookup_en = 1'b1;
        #1ps;
        assert (lookup_valid && !lookup_error && sam_idx == SAM[SAM_NUM_RULES-1].idx)
            else $fatal(1, "Memory lookup did not preserve sam_idx_t");

        addr = 48'h0000_0200_0000;
        #1ps;
        assert (lookup_valid && !lookup_error && !sam_idx.is_data &&
                sam_idx.dst_id == 8'd0 && sam_idx.dst_port_id == 2'd0)
            else $fatal(1, "Configuration lookup returned incorrect typed metadata");

        addr = 48'h0000_0000_0080;
        #1ps;
        assert (overlap_valid && !overlap_error && overlap_idx == OVERLAP_BROAD_IDX)
            else $fatal(1, "Overlap did not select the highest SAM array index");

        addr = 48'h0000_FFFF_F000;
        #1ps;
        assert (!lookup_valid && lookup_error && sam_idx == '0)
            else $fatal(1, "SAM miss did not report the required error result");

        $display("PASS: ni_sam pure-combinational lookup");
        $finish;
    end

endmodule

`resetall
