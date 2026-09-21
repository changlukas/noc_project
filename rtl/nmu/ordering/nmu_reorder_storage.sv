// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Contiguous response storage with independent fill and retire access. */
module nmu_reorder_storage #(
    parameter int unsigned DEPTH = 8,
    parameter int unsigned TAG_W = ni_flit_pkg::ORDERING_TAG_WIDTH,
    parameter type T = logic
) (
    input wire logic clk_i,
    input wire logic rst_i,
    input wire logic reserve_valid_i,
    input wire logic [TAG_W-1:0] reserve_base_i,
    input wire logic [TAG_W:0] reserve_count_i,
    output wire logic [TAG_W-1:0] next_base_o,
    output wire logic [TAG_W:0] free_count_o,
    input wire logic fill_valid_i,
    input wire logic fill_bypass_i,
    output wire logic fill_ready_o,
    input wire logic [TAG_W-1:0] fill_base_i,
    input wire logic fill_last_i,
    input wire T fill_data_i,
    input wire logic [TAG_W-1:0] peek_addr_i,
    output wire logic peek_complete_o,
    output wire T peek_data_o,
    input wire logic release_valid_i,
    input wire logic [TAG_W-1:0] release_addr_i,
    output wire logic [DEPTH-1:0] complete_o
);

    localparam int unsigned TAG_SPACE = 1 << TAG_W;
    localparam int unsigned CL_DEPTH = DEPTH > 1 ? $clog2(DEPTH) : 1;

    if (DEPTH < 1 || DEPTH > TAG_SPACE) begin : gen_invalid_depth
        initial $fatal(0, "Error: DEPTH must be in [1, TAG_SPACE] (instance %m)");
    end

    T data_reg [DEPTH];
    logic [DEPTH-1:0] alloc_reg, alloc_next;
    logic [DEPTH-1:0] complete_reg, complete_next;
    logic [TAG_W-1:0] fill_offset_reg [TAG_SPACE], fill_offset_next [TAG_SPACE];
    logic [TAG_W:0] free_count;
    logic [TAG_W-1:0] next_base;
    wire logic [TAG_W:0] fill_addr =
        {1'b0, fill_base_i} + {1'b0, fill_offset_reg[fill_base_i]};
    wire logic fill_transfer = fill_valid_i && fill_ready_o;
    wire logic [CL_DEPTH-1:0] fill_index = CL_DEPTH'(fill_addr);
    wire logic [CL_DEPTH-1:0] peek_index = CL_DEPTH'(peek_addr_i);
    wire logic [CL_DEPTH-1:0] release_index = CL_DEPTH'(release_addr_i);

    always_comb begin
        free_count = (TAG_W+1)'(DEPTH);
        next_base = '0;
        for (int n = 0; n < DEPTH; n++) begin
            if (alloc_reg[n]) begin
                free_count = (TAG_W+1)'(DEPTH - n - 1);
                next_base = TAG_W'(n + 1);
            end
        end
    end

    assign next_base_o = next_base;
    assign free_count_o = free_count;
    assign fill_ready_o = fill_addr < (TAG_W+1)'(DEPTH) &&
        alloc_reg[fill_index] && !complete_reg[fill_index];
    assign peek_complete_o = int'(peek_addr_i) < DEPTH && complete_reg[peek_index];
    assign peek_data_o = int'(peek_addr_i) < DEPTH ? data_reg[peek_index] : T'('0);
    assign complete_o = complete_reg;

    always_comb begin
        alloc_next = alloc_reg;
        complete_next = complete_reg;
        for (int tag = 0; tag < TAG_SPACE; tag++) begin
            fill_offset_next[tag] = fill_offset_reg[tag];
        end

        if (reserve_valid_i) begin
            for (int n = 0; n < DEPTH; n++) begin
                if (n >= int'(reserve_base_i) &&
                        n < int'(reserve_base_i) + int'(reserve_count_i)) begin
                    alloc_next[n] = 1'b1;
                end
            end
            fill_offset_next[reserve_base_i] = '0;
        end
        if (fill_transfer) begin
            if (!fill_bypass_i) begin
                complete_next[fill_index] = 1'b1;
            end
            fill_offset_next[fill_base_i] = fill_last_i ? '0 :
                fill_offset_reg[fill_base_i] + 1'b1;
        end
        if (release_valid_i && int'(release_addr_i) < DEPTH) begin
            alloc_next[release_index] = 1'b0;
            complete_next[release_index] = 1'b0;
        end
    end

    always_ff @(posedge clk_i) begin
        if (rst_i) begin
            alloc_reg <= '0;
            complete_reg <= '0;
            for (int tag = 0; tag < TAG_SPACE; tag++) begin
                fill_offset_reg[tag] <= '0;
            end
        end else begin
            alloc_reg <= alloc_next;
            complete_reg <= complete_next;
            for (int tag = 0; tag < TAG_SPACE; tag++) begin
                fill_offset_reg[tag] <= fill_offset_next[tag];
            end
            if (fill_transfer && !fill_bypass_i) begin
                data_reg[fill_index] <= fill_data_i;
            end
        end
    end
endmodule

`resetall
