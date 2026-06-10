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
    // R-channel multi-beat shadow: single r_last/r_id latch only captures
    // the FINAL beat. For burst reads, a procedural loop in the vendored
    // task can't read RDATA per-beat reliably (Verilator --timing race +
    // RREADY-held-high causes beats to be skipped). So we shadow every
    // RDATA via a parallel-captured array indexed by a NBA counter. The
    // task waits for (r_shadow_widx - snapshot_at_entry) >= blen, then
    // copies into dataR[0..blen-1].
    reg [WIDTH_DA-1:0] r_shadow [0:255];
    reg [7:0]          r_shadow_widx;       // write-side counter (NBA, parallel block)
    reg [7:0]          r_shadow_ridx = 8'd0; // read-side counter (blocking, drain task)
    always @(posedge ACLK) begin
        if (!ARESETn) begin
            r_shadow_widx <= 8'd0;
        end
        else begin
            if (BVALID && BREADY) begin
                b_id_latch   <= BID;
                b_resp_latch <= BRESP;
            end
            if (RVALID && RREADY) begin
                r_id_latch              <= RID;
                r_resp_latch            <= RRESP;
                r_last_latch            <= RLAST;
                r_shadow[r_shadow_widx] <= RDATA;
                r_shadow_widx           <= r_shadow_widx + 8'd1;
            end
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
        // Project-owned drain (NOT calling vendored axi_master_read_r) —
        // sidesteps the Verilator --timing per-beat RDATA read race entirely.
        // Assert RREADY (the vendored axi_master_read_r used by Task A's
        // mem_test deasserts RREADY at its exit; we must re-assert here),
        // then wait for the parallel always block to land `blen` new R beats
        // in r_shadow. r_shadow_ridx tracks the read pointer across calls so
        // multiple outstanding drains consume the right slots.
        //
        // Assumes the bridge preserves AR-issue → R-return order across IDs
        // (empirically true for the current NMU/NSU c_model on a single
        // noc_intf with no contention; see docs/superpowers/specs/
        // 2026-06-08-genamba-role1-testbench-design.md §3.5 for the
        // assumption rationale). If a future bridge variant returns R
        // out-of-order across distinct IDs, the shadow array would need
        // per-ID FIFOs instead of a single global queue.
        reg [7:0] start;
        integer i;
        RREADY <= 1'b1;
        start = r_shadow_ridx;
        while ((r_shadow_widx - start) < blen[7:0]) @(posedge ACLK);
        for (i = 0; i < blen; i = i + 1) begin
            dataR[i] = r_shadow[start + i[7:0]];
        end
        r_shadow_ridx = start + blen[7:0];
        RREADY <= 1'b0;
    endtask

    // ---------- Task A: vendored baseline ----------
    task test_baseline_mem_test;
        $display("[%0t] TASK A start: mem_test baseline", $time);
        mem_test(64'h0000, 64'h00FF, 16'd16, 1'b0);
        if (error_flag) $fatal(1, "TASK A: mem_test failed");
        $display("[%0t] TASK A PASS", $time);
    endtask

    // ---------- Task B: burst single-outstanding ----------
    // AXI4 §A3.3: master may issue W beats before or after AW — NMU
    // must buffer W until AW arrives. We issue AW then W sequentially to
    // avoid fork/join inside loops, which triggers a Verilator 5.036
    // coroutine-split bug (VlForkSync declared in wrong split frame).
    // expected[] flat: 4 outer × 16 beats max, indexed [outer*16+beat].
    task test_burst_blen(input integer blen);
        reg [WIDTH_DA-1:0] expected [0:63];
        integer i, b;
        reg [WIDTH_AD-1:0] addr;
        // R-shadow barrier: previous task (Task A vendored mem_test) advanced
        // r_shadow_widx via its own R handshakes; sync r_shadow_ridx so this
        // task's bfm_drain_r calls start counting from beats that arrive AFTER
        // this barrier, not from stale shadow entries.
        r_shadow_ridx = r_shadow_widx;
        $display("[%0t] TASK B start: burst blen=%0d", $time, blen);

        // Write phase — single-outstanding: AW → W beats → B drain
        for (i = 0; i < 4; i = i + 1) begin
            addr = 64'h0000_0400 + i * (blen * 16);
            for (b = 0; b < blen; b = b + 1) begin
                dataW[b]           = get_data(0) & get_mask(addr + b * 16, 16);
                expected[i*16 + b] = dataW[b];
            end
            bfm_post_aw(0, addr, blen);
            bfm_post_w(0, addr, blen);
            bfm_drain_b(0);
        end

        // Read + compare phase
        for (i = 0; i < 4; i = i + 1) begin
            addr = 64'h0000_0400 + i * (blen * 16);
            bfm_post_ar(0, addr, blen);
            bfm_drain_r(0, blen);
            // bfm_drain_r populates dataR[0:blen-1]
            for (b = 0; b < blen; b = b + 1) begin
                if ((dataR[b] & get_mask(addr + b * 16, 16)) !== expected[i*16 + b]) begin
                    $display("[%0t] TASK B blen=%0d outer=%0d beat=%0d mismatch D=0x%x exp=0x%x",
                             $time, blen, i, b, dataR[b], expected[i*16 + b]);
                    error_flag = 1;
                    $fatal(1, "TASK B data mismatch");
                end
            end
        end
        $display("[%0t] TASK B PASS blen=%0d", $time, blen);
    endtask

    // ---------- Task C: outstanding writes/reads via adapter layer ----------
    // N AWs sequentially absorbed into NMU AW queue, then N Ws sequentially,
    // then drain N Bs. Same shape for reads. Distinct AXI IDs 1..N.
    // Window 0x0800-0x09FF; blen=1 (single beat).
    task test_outstanding_N(input integer N);
        reg [WIDTH_DA-1:0] expected [0:15];
        integer i;
        reg [WIDTH_AD-1:0] addr;
        // R-shadow barrier (see test_burst_blen for rationale).
        r_shadow_ridx = r_shadow_widx;
        $display("[%0t] TASK C start: N=%0d outstanding", $time, N);

        for (i = 0; i < N; i = i + 1) begin
            addr = 64'h0000_0800 + i * 16;
            bfm_post_aw(i+1, addr, 1);
        end
        for (i = 0; i < N; i = i + 1) begin
            addr = 64'h0000_0800 + i * 16;
            dataW[0] = get_data(0) & get_mask(addr, 16);
            expected[i] = dataW[0];
            bfm_post_w(i+1, addr, 1);
        end
        for (i = 0; i < N; i = i + 1) bfm_drain_b(i+1);

        for (i = 0; i < N; i = i + 1) begin
            addr = 64'h0000_0800 + i * 16;
            bfm_post_ar(i+1, addr, 1);
        end
        for (i = 0; i < N; i = i + 1) begin
            addr = 64'h0000_0800 + i * 16;
            bfm_drain_r(i+1, 1);
            if ((dataR[0] & get_mask(addr, 16)) !== expected[i]) begin
                $display("[%0t] TASK C N=%0d id=%0d mismatch A=0x%x D=0x%x exp=0x%x",
                         $time, N, i+1, addr, dataR[0], expected[i]);
                error_flag = 1;
                $fatal(1, "TASK C data mismatch");
            end
        end
        $display("[%0t] TASK C PASS N=%0d", $time, N);
    endtask

    // Idle defaults
    initial begin
        AWID = 0; AWADDR = 0; AWLEN = 0; AWSIZE = 3'd5; AWBURST = 2'b01;
        AWLOCK = 0; AWCACHE = 0; AWPROT = 0; AWQOS = 0; AWVALID = 0;
        WDATA = 0; WSTRB = 0; WLAST = 0; WVALID = 0; BREADY = 1;
        ARID = 0; ARADDR = 0; ARLEN = 0; ARSIZE = 3'd5; ARBURST = 2'b01;
        ARLOCK = 0; ARCACHE = 0; ARPROT = 0; ARQOS = 0; ARVALID = 0; RREADY = 1;
    end
endmodule
