// gen_amba role-1 testbench BFM. Wraps vendored axi_master_tasks.v +
// mem_test_tasks.v as module-body includes; provides the adapter task
// layer (bfm_post_aw / bfm_post_w / bfm_drain_b / bfm_post_ar /
// bfm_drain_r) and the 7 task helpers (A: vendored mem_test; B-G:
// adapter-layer wrappers — added in T5-T10).
`timescale 1ns/1ps

module genamba_master_bfm #(
    parameter integer WIDTH_AD  = 64,
    parameter integer WIDTH_DA  = 256,
    parameter integer WIDTH_DS  = WIDTH_DA / 8,
    parameter integer WIDTH_DSB = $clog2(WIDTH_DS),  // strobe-width log2; vendored axi_master_tasks.v:399/427 + mem_test_tasks.v:52/70/236 reference this in caller scope (matches axi_tester.v:26 convention)
    parameter integer WIDTH_ID  = 8,
    parameter integer P_MST_ID  = 0     // required by mem_test_tasks.v:30 (mem_test seeds dataW with P_MST_ID[3:0])
) (
    input wire                  ACLK,
    input wire                  ARESETn,
    // AW
    output reg  [WIDTH_ID-1:0]  AWID,
    output reg  [WIDTH_AD-1:0]  AWADDR,
    output reg  [7:0]           AWLEN,
    output reg  [2:0]           AWSIZE,
    output reg  [1:0]           AWBURST,
    output reg                  AWLOCK,
    output reg  [3:0]           AWCACHE,
    output reg  [2:0]           AWPROT,
    output reg  [3:0]           AWQOS,
    output reg                  AWVALID,
    input  wire                 AWREADY,
    // W
    output reg  [WIDTH_DA-1:0]  WDATA,
    output reg  [WIDTH_DS-1:0]  WSTRB,
    output reg                  WLAST,
    output reg                  WVALID,
    input  wire                 WREADY,
    // B
    input  wire [WIDTH_ID-1:0]  BID,
    input  wire [1:0]           BRESP,
    input  wire                 BVALID,
    output reg                  BREADY,
    // AR
    output reg  [WIDTH_ID-1:0]  ARID,
    output reg  [WIDTH_AD-1:0]  ARADDR,
    output reg  [7:0]           ARLEN,
    output reg  [2:0]           ARSIZE,
    output reg  [1:0]           ARBURST,
    output reg                  ARLOCK,
    output reg  [3:0]           ARCACHE,
    output reg  [2:0]           ARPROT,
    output reg  [3:0]           ARQOS,
    output reg                  ARVALID,
    input  wire                 ARREADY,
    // R
    input  wire [WIDTH_ID-1:0]  RID,
    input  wire [WIDTH_DA-1:0]  RDATA,
    input  wire [1:0]           RRESP,
    input  wire                 RLAST,
    input  wire                 RVALID,
    output reg                  RREADY
);
    // dataW/dataR are wrapper-owned; vendored tasks resolve these names
    // into the calling scope (they are NOT declared inside the vendored .v
    // includes — verified against axi_master_tasks.v).
    reg [WIDTH_DA-1:0] dataW [0:1023];
    reg [WIDTH_DA-1:0] dataR [0:1023];

    `include "genamba/axi_master_tasks.v"
    `include "genamba/mem_test_tasks.v"

    // ---------- B/R channel Verilator --timing snapshot latches ----------
    // Vendored axi_master_write_b / axi_master_read_r read BID/RID
    // procedurally right after `@(posedge ACLK)`. Under Verilator --timing,
    // the procedural resume happens after the NBA region of that posedge,
    // so the read returns the NEXT cycle's value — which is 0 because
    // NMU's adapter de-asserts BVALID/BID one cycle after the handshake
    // per AXI4 §A3.2.1 held-latch pattern. The DBG monitors in tb_genamba.sv
    // (always @(posedge) blocks) read the correct in-cycle values.
    //
    // Workaround: snapshot BID/BRESP and RID/RRESP into latches via NBA
    // on the handshake cycle. The patched vendored tasks read these
    // latches (after waiting one extra @(posedge) for the latch to settle)
    // instead of reading the raw input wires.
    reg [WIDTH_ID-1:0] b_id_latch;
    reg [1:0]          b_resp_latch;
    reg [WIDTH_ID-1:0] r_id_latch;
    reg [1:0]          r_resp_latch;
    reg                r_last_latch;
    always @(posedge ACLK) begin
        if (BVALID && BREADY) begin
            b_id_latch   <= BID;
            b_resp_latch <= BRESP;
        end
        if (RVALID && RREADY) begin
            r_id_latch   <= RID;
            r_resp_latch <= RRESP;
            r_last_latch <= RLAST;
        end
    end

    // ---------- Adapter task layer (semantic names, positional vendored calls) ----------
    task bfm_post_aw(input [WIDTH_ID-1:0] id, input [WIDTH_AD-1:0] addr, input integer blen);
        axi_master_write_aw(id, addr, 16'd16, blen[15:0], 2'b01 /*INCR*/, 2'b00 /*no lock*/);
    endtask

    task bfm_post_w(input [WIDTH_ID-1:0] id, input [WIDTH_AD-1:0] addr, input integer blen);
        // Caller pre-populates dataW[0:blen-1] before invoking.
        axi_master_write_w(id, addr, 16'd16, blen[15:0], 2'b01, 1'b0 /*no delay*/);
    endtask

    task bfm_drain_b(input [WIDTH_ID-1:0] id);
        axi_master_write_b(id);
    endtask

    task bfm_post_ar(input [WIDTH_ID-1:0] id, input [WIDTH_AD-1:0] addr, input integer blen);
        axi_master_read_ar(id, addr, 16'd16, blen[15:0], 2'b01, 2'b00);
    endtask

    task bfm_drain_r(input [WIDTH_ID-1:0] id, input integer blen);
        // After return, dataR[0:blen-1] holds the read data.
        axi_master_read_r(id, blen[15:0], 1'b0);
    endtask

    // ---------- Task A: vendored baseline ----------
    task test_baseline_mem_test;
        $display("[%0t] TASK A start: mem_test baseline", $time);
        mem_test(64'h0000, 64'h00FF, 16'd16, 1'b0);
        if (error_flag) $fatal(1, "TASK A: mem_test failed");
        $display("[%0t] TASK A PASS", $time);
    endtask

    // Tasks B-G defined in later commits (T5-T10).

    // Idle defaults
    initial begin
        AWID = 0; AWADDR = 0; AWLEN = 0; AWSIZE = 3'd5; AWBURST = 2'b01;
        AWLOCK = 0; AWCACHE = 0; AWPROT = 0; AWQOS = 0; AWVALID = 0;
        WDATA = 0; WSTRB = 0; WLAST = 0; WVALID = 0; BREADY = 1;
        ARID = 0; ARADDR = 0; ARLEN = 0; ARSIZE = 3'd5; ARBURST = 2'b01;
        ARLOCK = 0; ARCACHE = 0; ARPROT = 0; ARQOS = 0; ARVALID = 0; RREADY = 1;
    end
endmodule
