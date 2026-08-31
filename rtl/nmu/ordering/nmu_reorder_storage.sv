// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Contiguous high-water response storage with independent fill and retire access. */
module nmu_reorder_storage #(
    parameter int unsigned DEPTH = 8,
    parameter int unsigned TAG_W = ni_flit_pkg::ORDERING_TAG_WIDTH,
    parameter type T = logic
) (
    input  wire logic                 clk_i,
    input  wire logic                 rst_i,
    input  wire logic                 reserve_valid_i,
    input  wire logic [TAG_W:0]       reserve_count_i,
    output wire logic [TAG_W-1:0]     next_base_o,
    output wire logic [TAG_W:0]       free_count_o,
    input  wire logic                 fill_valid_i,
    output wire logic                 fill_ready_o,
    input  wire logic [TAG_W-1:0]     fill_base_i,
    input  wire logic                 fill_last_i,
    input  wire T                     fill_data_i,
    input  wire logic [TAG_W-1:0]     peek_addr_i,
    output wire logic                 peek_complete_o,
    output wire T                     peek_data_o,
    input  wire logic                 release_valid_i,
    input  wire logic [TAG_W-1:0]     release_addr_i,
    output wire logic [DEPTH-1:0]     complete_o
);

    localparam int unsigned TAG_SPACE = 1 << TAG_W;

    if (DEPTH < 1 || DEPTH > TAG_SPACE) begin : gen_invalid_depth
        initial $fatal(0, "Error: DEPTH must be in [1, TAG_SPACE] (instance %m)");
    end

    T data_reg [DEPTH];
    logic [DEPTH-1:0] alloc_reg = '0;
    logic [DEPTH-1:0] complete_reg = '0;
    logic [TAG_W-1:0] fill_offset_reg [TAG_SPACE];
    logic [TAG_W:0] free_count;
    logic [TAG_W-1:0] next_base;
    logic [TAG_W:0] fill_addr;

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

    assign fill_addr = {1'b0, fill_base_i} + {1'b0, fill_offset_reg[fill_base_i]};
    assign next_base_o = next_base;
    assign free_count_o = free_count;
    assign fill_ready_o = fill_addr < DEPTH && alloc_reg[fill_addr] && !complete_reg[fill_addr];
    assign peek_complete_o = peek_addr_i < DEPTH && complete_reg[peek_addr_i];
    assign peek_data_o = data_reg[peek_addr_i];
    assign complete_o = complete_reg;

    always_ff @(posedge clk_i) begin
        if (reserve_valid_i) begin
            for (int n = 0; n < DEPTH; n++) begin
                if (n >= next_base && n < next_base + reserve_count_i) alloc_reg[n] <= 1'b1;
            end
        end
        if (fill_valid_i && fill_ready_o) begin
            data_reg[fill_addr] <= fill_data_i;
            complete_reg[fill_addr] <= 1'b1;
            fill_offset_reg[fill_base_i] <= fill_last_i ? '0 : fill_offset_reg[fill_base_i] + 1'b1;
        end
        if (release_valid_i) begin
            alloc_reg[release_addr_i] <= 1'b0;
            complete_reg[release_addr_i] <= 1'b0;
        end
        if (rst_i) begin
            alloc_reg <= '0;
            complete_reg <= '0;
            for (int tag = 0; tag < TAG_SPACE; tag++) fill_offset_reg[tag] <= '0;
        end
    end
endmodule

`resetall
