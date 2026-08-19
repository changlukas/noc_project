// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// NMU System Address Map decode and independent AW/AR timing cuts.
module nmu_sam #(
    // AW decode-to-RoB timing cut: 0 bypass, 1 simple, 2 full skid.
    parameter int unsigned AW_SAM_REG_TYPE = 0,
    // AR decode-to-RoB timing cut: 0 bypass, 1 simple, 2 full skid.
    parameter int unsigned AR_SAM_REG_TYPE = 0,
    // Generated System Address Map contract.
    parameter int unsigned SAM_NUM_RULES,
    parameter type         addr_t,
    parameter type         sam_mask_sel_t,
    parameter type         sam_idx_t,
    parameter type         sam_rule_t,
    parameter sam_rule_t [SAM_NUM_RULES-1:0] SAM
) (
    input  wire logic                                  noc_clk_i,
    input  wire logic                                  noc_rst_ni,
    input  wire logic                                  s_aw_valid_i,
    output wire logic                                  s_aw_ready_o,
    input  wire ni_child_types_pkg::nmu_sam_aw_t       s_aw_i,
    output wire logic                                  m_aw_valid_o,
    input  wire logic                                  m_aw_ready_i,
    output wire ni_child_types_pkg::nmu_sam_aw_result_t m_aw_o,
    input  wire logic                                  s_ar_valid_i,
    output wire logic                                  s_ar_ready_o,
    input  wire ni_signals_pkg::axi_ar_t               s_ar_i,
    output wire logic                                  m_ar_valid_o,
    input  wire logic                                  m_ar_ready_i,
    output wire ni_child_types_pkg::nmu_sam_ar_result_t m_ar_o
);

    localparam int unsigned AXI_ADDR_W = ni_params_pkg::AXI_ADDR_WIDTH_DFLT;
    localparam int unsigned COLLECTIVE_MASK_W = ni_flit_pkg::COLLECTIVE_MASK_WIDTH;
    // Generated SAM ranges are 4 KiB aligned and sized.
    localparam int unsigned SAM_REGION_ALIGN_W = 12;

    if (AW_SAM_REG_TYPE > 2) begin : gen_invalid_aw_reg_type
        initial $fatal(0, "Error: AW_SAM_REG_TYPE must be 0, 1, or 2 (instance %m)");
    end

    if (AR_SAM_REG_TYPE > 2) begin : gen_invalid_ar_reg_type
        initial $fatal(0, "Error: AR_SAM_REG_TYPE must be 0, 1, or 2 (instance %m)");
    end

    if (COLLECTIVE_MASK_W != ni_flit_pkg::DST_ID_WIDTH ||
            COLLECTIVE_MASK_W != ni_flit_pkg::X_WIDTH + ni_flit_pkg::Y_WIDTH) begin : gen_invalid_collective_width
        initial $fatal(0, "Error: collective mask width must match the destination ID layout (instance %m)");
    end

    function automatic logic [AXI_ADDR_W-1:0] selector_mask(
        input sam_mask_sel_t selector
    );
        logic [AXI_ADDR_W-1:0] mask;
        int unsigned            selector_offset;
        int unsigned            selector_limit;

        mask = '0;
        selector_offset = int'(selector.offset);
        selector_limit = selector_offset + int'(selector.len);
        for (int unsigned bit_idx = 0; bit_idx < AXI_ADDR_W; bit_idx++) begin
            if (bit_idx >= selector_offset && bit_idx < selector_limit) begin
                mask[bit_idx] = 1'b1;
            end
        end
        return mask;
    endfunction

    function automatic logic [COLLECTIVE_MASK_W-1:0] collective_mask_from_address_mask(
        input logic [AXI_ADDR_W-1:0] address_mask,
        input sam_idx_t               sam_idx
    );
        logic [COLLECTIVE_MASK_W-1:0] collective_mask;

        collective_mask = '0;
        collective_mask[ni_flit_pkg::X_WIDTH-1:0] =
            ni_flit_pkg::X_WIDTH'((address_mask & selector_mask(sam_idx.mask_x)) >>
                sam_idx.mask_x.offset);
        collective_mask[ni_flit_pkg::X_WIDTH +: ni_flit_pkg::Y_WIDTH] =
            ni_flit_pkg::Y_WIDTH'((address_mask & selector_mask(sam_idx.mask_y)) >>
                sam_idx.mask_y.offset);
        return collective_mask;
    endfunction

    function automatic logic [AXI_ADDR_W:0] burst_last_byte(
        input logic [AXI_ADDR_W-1:0]                    addr,
        input logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0]   len,
        input logic [ni_flit_pkg::AXI_SIZE_WIDTH-1:0]  size,
        input logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0] burst
    );
        logic [AXI_ADDR_W:0] extended_addr;
        logic [AXI_ADDR_W:0] total_bytes;

        extended_addr = {1'b0, addr};
        total_bytes = '0;
        total_bytes[ni_flit_pkg::AXI_LEN_WIDTH-1:0] = len;
        total_bytes = (total_bytes + 1'b1) << size;

        if (burst == 2'd2) begin
            burst_last_byte = (extended_addr & ~(total_bytes - 1'b1)) + total_bytes - 1'b1;
        end else begin
            burst_last_byte = extended_addr + total_bytes - 1'b1;
        end
    endfunction

    function automatic logic burst_footprint_error(
        input logic [AXI_ADDR_W-1:0]                    addr,
        input logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0]   len,
        input logic [ni_flit_pkg::AXI_SIZE_WIDTH-1:0]  size,
        input logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0] burst
    );
        logic [AXI_ADDR_W:0] last_byte;

        last_byte = burst_last_byte(addr, len, size, burst);
        return last_byte[AXI_ADDR_W] ||
            last_byte[AXI_ADDR_W-1:SAM_REGION_ALIGN_W] !=
                addr[AXI_ADDR_W-1:SAM_REGION_ALIGN_W];
    endfunction

    function automatic logic collective_error(
        input logic [ni_params_pkg::AXI_AWUSER_WIDTH_DFLT-1:0] awuser,
        input logic                                            awlock,
        input sam_idx_t                                        sam_idx
    );
        logic [AXI_ADDR_W-1:0] address_mask;
        logic [AXI_ADDR_W-1:0] allowed_address_mask;

        address_mask = awuser[57:10];
        allowed_address_mask = selector_mask(sam_idx.mask_x) |
            selector_mask(sam_idx.mask_y);
        case (awuser[9:8])
            2'd0: collective_error = address_mask != '0;
            2'd1: collective_error = address_mask == '0 || awlock ||
                !sam_idx.collective_en ||
                (address_mask & ~allowed_address_mask) != '0;
            default: collective_error = 1'b1;
        endcase
    endfunction

    sam_idx_t aw_sam_idx;
    logic     aw_lookup_valid;
    logic     aw_lookup_error;
    sam_idx_t ar_sam_idx;
    logic     ar_lookup_valid;
    logic     ar_lookup_error;

    logic aw_slice_ready;
    logic ar_slice_ready;

    wire ni_child_types_pkg::nmu_sam_aw_result_t aw_decoded;
    wire ni_child_types_pkg::nmu_sam_ar_result_t ar_decoded;

    ni_sam #(
        .SAM_NUM_RULES  ( SAM_NUM_RULES  ),
        .addr_t         ( addr_t         ),
        .sam_mask_sel_t ( sam_mask_sel_t ),
        .sam_idx_t      ( sam_idx_t      ),
        .sam_rule_t     ( sam_rule_t     ),
        .SAM            ( SAM            )
    ) i_aw_ni_sam (
        .addr_i         ( s_aw_i.axi.awaddr ),
        .lookup_en_i    ( noc_rst_ni && s_aw_valid_i ),
        .sam_idx_o      ( aw_sam_idx        ),
        .lookup_valid_o ( aw_lookup_valid   ),
        .lookup_error_o ( aw_lookup_error   )
    );

    ni_sam #(
        .SAM_NUM_RULES  ( SAM_NUM_RULES  ),
        .addr_t         ( addr_t         ),
        .sam_mask_sel_t ( sam_mask_sel_t ),
        .sam_idx_t      ( sam_idx_t      ),
        .sam_rule_t     ( sam_rule_t     ),
        .SAM            ( SAM            )
    ) i_ar_ni_sam (
        .addr_i         ( s_ar_i.araddr ),
        .lookup_en_i    ( noc_rst_ni && s_ar_valid_i ),
        .sam_idx_o      ( ar_sam_idx    ),
        .lookup_valid_o ( ar_lookup_valid ),
        .lookup_error_o ( ar_lookup_error )
    );

    assign s_aw_ready_o = noc_rst_ni && aw_slice_ready;
    assign s_ar_ready_o = noc_rst_ni && ar_slice_ready;

    assign aw_decoded.axi = s_aw_i.axi;
    assign aw_decoded.route.route.domain.dst_id = aw_sam_idx.dst_id;
    assign aw_decoded.route.route.domain.dst_port_id = aw_sam_idx.dst_port_id;
    assign aw_decoded.route.route.domain.is_data = aw_sam_idx.is_data;
    assign aw_decoded.route.user = s_aw_i.awuser[7:0];
    assign aw_decoded.route.collective_op = s_aw_i.awuser[9:8];
    assign aw_decoded.route.collective_mask = collective_mask_from_address_mask(
        s_aw_i.awuser[57:10], aw_sam_idx);

    assign ar_decoded.axi = s_ar_i;
    assign ar_decoded.route.domain.dst_id = ar_sam_idx.dst_id;
    assign ar_decoded.route.domain.dst_port_id = ar_sam_idx.dst_port_id;
    assign ar_decoded.route.domain.is_data = ar_sam_idx.is_data;

    stream_register #(
        .REG_TYPE ( AW_SAM_REG_TYPE ),
        .T        ( ni_child_types_pkg::nmu_sam_aw_result_t )
    ) i_aw_reg_slice (
        .clk_i     ( noc_clk_i                            ),
        .rst_ni    ( noc_rst_ni                           ),
        .s_valid_i ( noc_rst_ni && s_aw_valid_i && aw_lookup_valid ),
        .s_ready_o ( aw_slice_ready                       ),
        .s_data_i  ( aw_decoded                           ),
        .m_valid_o ( m_aw_valid_o                         ),
        .m_ready_i ( m_aw_ready_i                         ),
        .m_data_o  ( m_aw_o                               )
    );

    stream_register #(
        .REG_TYPE ( AR_SAM_REG_TYPE ),
        .T        ( ni_child_types_pkg::nmu_sam_ar_result_t )
    ) i_ar_reg_slice (
        .clk_i     ( noc_clk_i                            ),
        .rst_ni    ( noc_rst_ni                           ),
        .s_valid_i ( noc_rst_ni && s_ar_valid_i && ar_lookup_valid ),
        .s_ready_o ( ar_slice_ready                       ),
        .s_data_i  ( ar_decoded                           ),
        .m_valid_o ( m_ar_valid_o                         ),
        .m_ready_i ( m_ar_ready_i                         ),
        .m_data_o  ( m_ar_o                               )
    );

    always_ff @(posedge noc_clk_i) begin
        if (noc_rst_ni) begin
            if (s_aw_valid_i && aw_lookup_error) begin
                $fatal(0, "Error: invalid AW SAM mapping (instance %m)");
            end else if (s_aw_valid_i && burst_footprint_error(
                    s_aw_i.axi.awaddr, s_aw_i.axi.awlen,
                    s_aw_i.axi.awsize, s_aw_i.axi.awburst)) begin
                $fatal(0, "Error: AW burst footprint crosses a SAM region boundary: addr=%h last=%h (instance %m)",
                    s_aw_i.axi.awaddr, burst_last_byte(
                        s_aw_i.axi.awaddr, s_aw_i.axi.awlen,
                        s_aw_i.axi.awsize, s_aw_i.axi.awburst));
            end else if (s_aw_valid_i && collective_error(
                    s_aw_i.awuser, s_aw_i.axi.awlock, aw_sam_idx)) begin
                $fatal(0, "Error: invalid AW collective mapping (instance %m)");
            end
            if (s_ar_valid_i && ar_lookup_error) begin
                $fatal(0, "Error: invalid AR SAM mapping (instance %m)");
            end else if (s_ar_valid_i && burst_footprint_error(
                    s_ar_i.araddr, s_ar_i.arlen, s_ar_i.arsize, s_ar_i.arburst)) begin
                $fatal(0, "Error: AR burst footprint crosses a SAM region boundary: addr=%h last=%h (instance %m)",
                    s_ar_i.araddr, burst_last_byte(
                        s_ar_i.araddr, s_ar_i.arlen, s_ar_i.arsize, s_ar_i.arburst));
            end
        end
    end

endmodule

`resetall
