// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Contiguous response storage with independent fill and retire access. */
module nmu_reorder_storage #(
    parameter int unsigned DEPTH  = 8,
    parameter int unsigned TAG_W  = ni_flit_pkg::ORDERING_TAG_WIDTH,
    parameter type         data_t = logic
) (
    input  wire logic              clk_i,
    input  wire logic              rst_n_i,
    input  wire logic              alloc_valid_i,
    input  wire logic  [TAG_W-1:0] alloc_base_i,
    input  wire logic    [TAG_W:0] alloc_cnt_i,
    output wire logic  [TAG_W-1:0] next_base_o,
    output wire logic    [TAG_W:0] free_cnt_o,
    input  wire logic              wr_valid_i,
    input  wire logic              wr_bypass_i,
    output wire logic              wr_ready_o,
    input  wire logic  [TAG_W-1:0] wr_base_i,
    input  wire logic              wr_last_i,
    input  wire data_t             wr_data_i,
    input  wire logic              rd_en_i,
    input  wire logic  [TAG_W-1:0] rd_addr_i,
    output wire logic              rd_entry_complete_o,
    output wire data_t             rd_data_o,
    input  wire logic              free_valid_i,
    input  wire logic  [TAG_W-1:0] free_addr_i,
    output wire logic  [DEPTH-1:0] complete_o
);

    localparam int unsigned NUM_TAGS = 1 << TAG_W;
    localparam int unsigned ADDR_W   = DEPTH > 1 ? $clog2(DEPTH) : 1;

    if (DEPTH < 1 || DEPTH > NUM_TAGS) begin : gen_invalid_depth
        initial $fatal(0, "Error: DEPTH must be in [1, NUM_TAGS] (instance %m)");
    end

    data_t             data_reg [DEPTH];
    logic  [DEPTH-1:0] alloc_reg, alloc_next;
    logic  [DEPTH-1:0] complete_reg, complete_next;
    logic  [TAG_W-1:0] wr_offset_reg [DEPTH], wr_offset_next [DEPTH];
    logic    [TAG_W:0] free_cnt;
    logic  [TAG_W-1:0] next_base;
    wire logic [TAG_W-1:0] wr_offset = int'(wr_base_i) < DEPTH ?
        wr_offset_reg[ADDR_W'(wr_base_i)] : '0;
    wire logic [TAG_W:0] wr_addr = {1'b0, wr_base_i} + {1'b0, wr_offset};
    wire logic wr_accept = wr_valid_i && wr_ready_o;
    wire logic [ADDR_W-1:0] wr_idx = ADDR_W'(wr_addr);
    wire logic [ADDR_W-1:0] rd_idx = ADDR_W'(rd_addr_i);
    wire logic [ADDR_W-1:0] free_idx = ADDR_W'(free_addr_i);

    always_comb begin
        free_cnt  = (TAG_W+1)'(DEPTH);
        next_base = '0;
        for (int n = 0; n < DEPTH; n++) begin
            if (alloc_reg[n]) begin
                free_cnt  = (TAG_W+1)'(DEPTH - n - 1);
                next_base = TAG_W'(n + 1);
            end
        end
    end

    assign next_base_o = next_base;
    assign free_cnt_o  = free_cnt;
    assign wr_ready_o  = rst_n_i && wr_valid_i && wr_addr < (TAG_W+1)'(DEPTH) &&
        alloc_reg[wr_idx] && !complete_reg[wr_idx];
    assign rd_entry_complete_o = rst_n_i && rd_en_i &&
        int'(rd_addr_i) < DEPTH && complete_reg[rd_idx];
    // Payload memory is intentionally unreset; expose only completed entries.
    assign rd_data_o  = rd_entry_complete_o ? data_reg[rd_idx] : data_t'('0);
    assign complete_o = complete_reg;

    always_comb begin
        alloc_next    = alloc_reg;
        complete_next = complete_reg;
        for (int tag = 0; tag < DEPTH; tag++) begin
            wr_offset_next[tag] = wr_offset_reg[tag];
        end

        if (alloc_valid_i && int'(alloc_base_i) < DEPTH) begin
            for (int n = 0; n < DEPTH; n++) begin
                if (n >= int'(alloc_base_i) &&
                        n < int'(alloc_base_i) + int'(alloc_cnt_i)) begin
                    alloc_next[n] = 1'b1;
                end
            end
            wr_offset_next[ADDR_W'(alloc_base_i)] = '0;
        end
        if (wr_accept) begin
            if (!wr_bypass_i) begin
                complete_next[wr_idx] = 1'b1;
            end
            wr_offset_next[ADDR_W'(wr_base_i)] = wr_last_i ? '0 :
                wr_offset_reg[ADDR_W'(wr_base_i)] + 1'b1;
        end
        if (free_valid_i && int'(free_addr_i) < DEPTH) begin
            alloc_next[free_idx]    = 1'b0;
            complete_next[free_idx] = 1'b0;
        end
    end

    always @(posedge clk_i or negedge rst_n_i) begin
        if (~rst_n_i) begin
            alloc_reg    <= '0;
            complete_reg <= '0;
            for (int tag = 0; tag < DEPTH; tag++) begin
                wr_offset_reg[tag] <= '0;
            end
        end else begin
            alloc_reg    <= alloc_next;
            complete_reg <= complete_next;
            for (int tag = 0; tag < DEPTH; tag++) begin
                wr_offset_reg[tag] <= wr_offset_next[tag];
            end
            if (wr_accept && !wr_bypass_i) begin
                data_reg[wr_idx] <= wr_data_i;
            end
        end
    end
    // synthesis translate_off
    always @(posedge clk_i) begin
        if (rst_n_i) begin
            if (alloc_valid_i && (alloc_cnt_i == 0 ||
                    int'(alloc_base_i) + int'(alloc_cnt_i) > DEPTH))
                $fatal(1, "ROB allocation exceeds configured storage (%m)");
            if (wr_valid_i && int'(wr_base_i) >= DEPTH)
                $fatal(1, "ROB response base exceeds configured storage (%m)");
            if (!wr_valid_i && wr_ready_o !== 1'b0)
                $fatal(1, "inactive storage fill ready must be zero (%m)");
            if (!rd_en_i && {rd_entry_complete_o, rd_data_o} !== '0)
                $fatal(1, "inactive storage peek must be zero (%m)");
            if (rd_en_i && rd_entry_complete_o && $isunknown(rd_data_o))
                $fatal(1, "completed storage payload contains X (%m)");
        end
    end
    // synthesis translate_on
endmodule

`resetall
