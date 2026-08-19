`resetall
`timescale 1ns / 1ps
`default_nettype none

// Pure-combinational System Address Map lookup shared by NMU and NSU.
module ni_sam #(
    parameter int unsigned SAM_NUM_RULES,
    parameter type         addr_t,
    parameter type         sam_mask_sel_t,
    parameter type         sam_idx_t,
    parameter type         sam_rule_t,
    parameter sam_rule_t [SAM_NUM_RULES-1:0] SAM
) (
    input  wire addr_t    addr_i,
    input  wire logic     lookup_en_i,
    output wire sam_idx_t sam_idx_o,
    output wire logic     lookup_valid_o,
    output wire logic     lookup_error_o
);

    localparam int unsigned SAM_MASK_SEL_WIDTH =
        $clog2(ni_params_pkg::AXI_ADDR_WIDTH_DFLT + 1);
    localparam int unsigned SAM_IDX_WIDTH =
        ni_flit_pkg::DST_ID_WIDTH + ni_flit_pkg::DST_PORT_ID_WIDTH + 2 +
        2 * $bits(sam_mask_sel_t);

    if (SAM_NUM_RULES == 0) begin : gen_invalid_rule_count
        initial $fatal(0, "Error: SAM_NUM_RULES must be greater than zero (instance %m)");
    end

    if ($bits(addr_t) != ni_params_pkg::AXI_ADDR_WIDTH_DFLT) begin : gen_invalid_addr_type
        initial $fatal(0, "Error: addr_t must match AXI_ADDR_WIDTH_DFLT (instance %m)");
    end

    if ($bits(sam_mask_sel_t) != 2 * SAM_MASK_SEL_WIDTH) begin : gen_invalid_mask_selector_type
        initial $fatal(0, "Error: sam_mask_sel_t has an incompatible width (instance %m)");
    end

    if ($bits(sam_idx_t) != SAM_IDX_WIDTH) begin : gen_invalid_index_type
        initial $fatal(0, "Error: sam_idx_t has an incompatible width (instance %m)");
    end

    if ($bits(sam_rule_t) != $bits(sam_idx_t) + 2 * $bits(addr_t)) begin : gen_invalid_rule_type
        initial $fatal(0, "Error: sam_rule_t has an incompatible width (instance %m)");
    end

    sam_idx_t decoded_idx;
    logic     decoded_valid;
    logic     decoded_error;

    cc_addr_decode #(
        .NoRules ( SAM_NUM_RULES ),
        .addr_t  ( addr_t        ),
        .idx_t   ( sam_idx_t     ),
        .rule_t  ( sam_rule_t    )
    ) i_cc_addr_decode (
        .addr_i,
        .addr_map_i       ( SAM  ),
        .idx_o            ( decoded_idx ),
        .dec_valid_o      ( decoded_valid ),
        .dec_error_o      ( decoded_error ),
        .en_default_idx_i ( 1'b0 ),
        .default_idx_i    ( '0   )
    );

    assign sam_idx_o      = lookup_en_i && decoded_valid ? decoded_idx : '0;
    assign lookup_valid_o = lookup_en_i && decoded_valid;
    assign lookup_error_o = lookup_en_i && decoded_error;

endmodule

`resetall
