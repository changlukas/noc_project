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

    // Lint scope for the vendored includes only:
    // - INITIALDLY: vendored tasks drive AW/W/AR/R with `<= #LD` and are
    //   called from initial blocks — the intended gen_amba BFM idiom;
    //   correct under --timing.
    // - WIDTHXZEXPAND: vendored mem_test seeds `dataW[ind] = 'hX` unsized.
    /* verilator lint_off INITIALDLY */
    /* verilator lint_off WIDTHXZEXPAND */
    `include "genamba/axi_master_tasks.v"
    `include "genamba/mem_test_tasks.v"
    /* verilator lint_on WIDTHXZEXPAND */
    /* verilator lint_on INITIALDLY */

    // ---------- B/R channel Verilator --timing snapshot capture ----------
    // Vendored axi_master_write_b reads BID/BRESP procedurally right after
    // `@(posedge ACLK)`. Under Verilator --timing, the procedural resume
    // happens after the NBA region of that posedge, so the read returns the
    // NEXT cycle's value — which is 0 because NMU's adapter de-asserts
    // BVALID/BID one cycle after the handshake per AXI4 §A3.2.1 held-latch
    // pattern.
    //
    // Workaround: snapshot BID/BRESP into latches via NBA on the handshake
    // cycle. The patched vendored axi_master_write_b uses b_count as its
    // handshake detector (condition-based, same pattern as r_shadow_widx
    // below): it holds BREADY high until the counter advances, then reads
    // these latches. Neither a fixed one-cycle wait nor a procedural
    // BVALID poll is sound here — the poll resumes post-NBA (one
    // wire-cycle early) and a BREADY deassert issued from that point kills
    // the handshake before any always_ff samples it.
    reg [WIDTH_ID-1:0] b_id_latch;
    reg [1:0]          b_resp_latch;
    reg [7:0]          b_count = 8'd0;  // increments on every B handshake capture
    // Request-side handshake counters (same pattern): the vendored ready
    // polls resume post-NBA one wire-cycle early, which races the NMU's
    // one-shot wait_valid ready pulses. The patched vendored tasks wait on
    // these counters instead of polling AWREADY/WREADY/ARREADY.
    reg [7:0]          aw_count = 8'd0;
    reg [7:0]          w_count  = 8'd0;
    reg [7:0]          ar_count = 8'd0;
    // R-channel multi-beat shadow: a procedural loop can't read R-channel
    // signals per-beat reliably under --timing (same race as B, plus the
    // 2-cycles-per-iter starvation documented in ATTRIBUTION.md). So a
    // parallel block captures EVERY RVALID&&RREADY handshake — data AND
    // per-beat metadata (RID/RRESP/RLAST) — into NBA-counter-indexed
    // arrays. bfm_drain_r then waits for `blen` new entries, copies data
    // into dataR, and checks the metadata per beat.
    reg [WIDTH_DA-1:0] r_shadow      [0:255];
    reg [WIDTH_ID-1:0] r_shadow_id   [0:255];
    reg [1:0]          r_shadow_resp [0:255];
    reg                r_shadow_last [0:255];
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
                b_count      <= b_count + 8'd1;
            end
            if (AWVALID && AWREADY) aw_count <= aw_count + 8'd1;
            if (WVALID  && WREADY)  w_count  <= w_count  + 8'd1;
            if (ARVALID && ARREADY) ar_count <= ar_count + 8'd1;
            if (RVALID && RREADY) begin
                r_shadow[r_shadow_widx]      <= RDATA;
                r_shadow_id[r_shadow_widx]   <= RID;
                r_shadow_resp[r_shadow_widx] <= RRESP;
                r_shadow_last[r_shadow_widx] <= RLAST;
                r_shadow_widx                <= r_shadow_widx + 8'd1;
                // 8-bit counter wrap guard: one full run currently lands
                // ~232 R beats (A:16 reads + B..G). Past 255 the shadow
                // index wraps and drains would read stale slots — fail
                // loudly instead. Widen the counters when Phase 2 grows
                // the per-run beat count.
                if (r_shadow_widx == 8'd255)
                    $fatal(1, "r_shadow_widx wrap: widen shadow counters");
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
        // Vendored write_b sets error_flag on BID/BRESP mismatch (project
        // patch; see ATTRIBUTION.md) but does not abort — trap it here so
        // a B-channel protocol error fails the run with a non-zero exit.
        if (error_flag) $fatal(1, "bfm_drain_b: B-channel protocol error");
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
        // Per-beat checks against the captured metadata: RID must equal the
        // expected id on every beat, RRESP must be OKAY, and RLAST must be
        // asserted on exactly the final beat of the burst. The checks live
        // in check_r_beat (a function, not inline) — inline if-blocks here
        // push the enclosing coroutine over Verilator 5.036's splitter
        // threshold when this task is called inside a fork branch
        // (`__Vfork_N__sync was not declared` C++ compile error).
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
        // RREADY uses NBA from this initial-called task on purpose — same
        // BFM-driver idiom as the vendored tasks (avoids a same-timestep
        // race against the shadow-capture always block reading RREADY).
        /* verilator lint_off INITIALDLY */
        RREADY <= 1'b1;
        start = r_shadow_ridx;
        while ((r_shadow_widx - start) < blen[7:0]) @(posedge ACLK);
        for (i = 0; i < blen; i = i + 1) begin
            dataR[i] = r_shadow[start + i[7:0]];
            check_r_beat(start + i[7:0], id, i, blen);
        end
        r_shadow_ridx = start + blen[7:0];
        RREADY <= 1'b0;
        /* verilator lint_on INITIALDLY */
    endtask

    // R-shadow barrier between test tasks. A bare `ridx = widx` sync races
    // straggler beats: the vendored read path's procedural loop runs one
    // wire-cycle ahead of the actual handshakes, so its final beat can still
    // be in flight (RVALID held by the bridge, RREADY already dropped) when
    // the task returns. Flush by re-asserting RREADY, wait until RVALID has
    // been low for 4 consecutive edges, then sync the read pointer.
    task bfm_r_barrier;
        integer quiet;
        /* verilator lint_off INITIALDLY */
        RREADY <= 1'b1;  // release any straggler beat held by the bridge
        quiet = 0;
        while (quiet < 4) begin
            @(posedge ACLK);
            if (RVALID) quiet = 0; else quiet = quiet + 1;
        end
        RREADY <= 1'b0;
        /* verilator lint_on INITIALDLY */
        r_shadow_ridx = r_shadow_widx;
    endtask

    // Per-beat R metadata check (see bfm_drain_r). Function, not task: no
    // time consumption, and the body stays out of the calling coroutine.
    function automatic void check_r_beat(input [7:0] k, input [WIDTH_ID-1:0] id,
                                         input integer i, input integer blen);
        if (r_shadow_id[k] !== id) begin
            $display("[%0t] bfm_drain_r RID mismatch beat=%0d got=0x%0h exp=0x%0h",
                     $time, i, r_shadow_id[k], id);
            error_flag = 1;
            $fatal(1, "bfm_drain_r RID mismatch");
        end
        if (r_shadow_resp[k] !== 2'b00) begin
            $display("[%0t] bfm_drain_r RRESP not OKAY beat=%0d resp=%0d",
                     $time, i, r_shadow_resp[k]);
            error_flag = 1;
            $fatal(1, "bfm_drain_r RRESP error");
        end
        if (r_shadow_last[k] !== (i == blen - 1)) begin
            $display("[%0t] bfm_drain_r RLAST misplace beat=%0d last=%b blen=%0d",
                     $time, i, r_shadow_last[k], blen);
            error_flag = 1;
            $fatal(1, "bfm_drain_r RLAST framing error");
        end
    endfunction

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
        bfm_r_barrier;
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
        bfm_r_barrier;
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

    // ---------- Task D: outstanding burst ----------
    // Window 0x0A00-0x0DFF; N=4 outstanding x blen in {4,8}; distinct IDs.
    task test_outstanding_burst_N4(input integer blen);
        reg [WIDTH_DA-1:0] expected [0:3][0:15];
        integer i, b;
        reg [WIDTH_AD-1:0] addr;
        // R-shadow barrier (see test_burst_blen / test_outstanding_N for rationale).
        bfm_r_barrier;
        $display("[%0t] TASK D start: N=4 blen=%0d outstanding burst", $time, blen);

        for (i = 0; i < 4; i = i + 1) begin
            addr = 64'h0000_0A00 + i * (blen * 16);
            bfm_post_aw(i+1, addr, blen);
        end
        for (i = 0; i < 4; i = i + 1) begin
            addr = 64'h0000_0A00 + i * (blen * 16);
            for (b = 0; b < blen; b = b + 1) begin
                dataW[b] = get_data(0) & get_mask(addr + b*16, 16);
                expected[i][b] = dataW[b];
            end
            bfm_post_w(i+1, addr, blen);
        end
        for (i = 0; i < 4; i = i + 1) bfm_drain_b(i+1);

        for (i = 0; i < 4; i = i + 1) begin
            addr = 64'h0000_0A00 + i * (blen * 16);
            bfm_post_ar(i+1, addr, blen);
        end
        for (i = 0; i < 4; i = i + 1) begin
            addr = 64'h0000_0A00 + i * (blen * 16);
            bfm_drain_r(i+1, blen);
            for (b = 0; b < blen; b = b + 1) begin
                if ((dataR[b] & get_mask(addr + b*16, 16)) !== expected[i][b]) begin
                    $display("[%0t] TASK D N4 blen=%0d id=%0d beat=%0d mismatch D=0x%x exp=0x%x",
                             $time, blen, i+1, b, dataR[b], expected[i][b]);
                    error_flag = 1;
                    $fatal(1, "TASK D data mismatch");
                end
            end
        end
        $display("[%0t] TASK D PASS N=4 blen=%0d", $time, blen);
    endtask

    // ---------- Task E: same-ID outstanding (4 W/R with shared ID) ----------
    // AMBA AXI4 §A5.3: same-ID R returns must follow AR issue order;
    // since WID was removed, same-ID W beats follow AW issue order on the
    // channel (no fork on AW or W issuance — single sequencer prevents
    // multi-driver conflict on shared AW/W signals).
    // Concurrent B drain on its own branch avoids stalling B queue once W
    // beats land.
    localparam [7:0] E_FIXED_ID = 8'd7;
    task test_same_id_outstanding;
        reg [WIDTH_DA-1:0] expected [0:3];
        integer i, j;             // separate vars for fork branches
        reg [WIDTH_AD-1:0] addr;
        // R-shadow barrier (consistent with other test wrappers).
        bfm_r_barrier;
        $display("[%0t] TASK E start: same-ID outstanding (id=%0d)", $time, E_FIXED_ID);

        // Phase 1: serial AW with shared ID
        for (i = 0; i < 4; i = i + 1) begin
            addr = 64'h0000_0E00 + i * 16;
            bfm_post_aw(E_FIXED_ID, addr, 1);
        end
        // Phase 2: serial W in AW order + concurrent B drain (separate loop vars)
        fork
            begin : w_branch
                integer w_i;
                for (w_i = 0; w_i < 4; w_i = w_i + 1) begin
                    dataW[0] = get_data(0) & get_mask(64'h0000_0E00 + w_i*16, 16);
                    expected[w_i] = dataW[0];
                    bfm_post_w(E_FIXED_ID, 64'h0000_0E00 + w_i*16, 1);
                end
            end
            begin : b_branch
                integer b_i;
                for (b_i = 0; b_i < 4; b_i = b_i + 1)
                    bfm_drain_b(E_FIXED_ID);
            end
        join

        // Phase 3: serial AR with shared ID, then drain R; same-ID R MUST follow AR order
        for (j = 0; j < 4; j = j + 1) begin
            addr = 64'h0000_0E00 + j * 16;
            bfm_post_ar(E_FIXED_ID, addr, 1);
        end
        for (j = 0; j < 4; j = j + 1) begin
            bfm_drain_r(E_FIXED_ID, 1);
            if ((dataR[0] & get_mask(64'h0000_0E00 + j*16, 16)) !== expected[j]) begin
                $display("[%0t] TASK E same-ID ORDER mismatch j=%0d D=0x%x exp=0x%x",
                         $time, j, dataR[0], expected[j]);
                error_flag = 1;
                $fatal(1, "TASK E same-ID ordering violation");
            end
        end
        $display("[%0t] TASK E PASS (same-ID order preserved)", $time);
    endtask

    // ---------- Task F: mixed R+W concurrent ----------
    // Writes addrs 0x1000-0x107F (IDs 1..8); pre-seed reads at 0x1100-0x117F
    // (IDs 0x81..0x88 to avoid ID clash with concurrent writes). Each fork
    // branch owns its own loop variable. Verify writes landed via post-fork
    // single-outstanding readback.
    task test_mixed_rw_concurrent;
        reg [WIDTH_DA-1:0] w_expected [0:7];
        reg [WIDTH_DA-1:0] r_expected [0:7];
        integer i, k;             // serial-phase vars
        // R-shadow barrier (consistent with other test wrappers).
        bfm_r_barrier;
        $display("[%0t] TASK F start: mixed R+W concurrent", $time);

        // Pre-seed read window — sequential writes via the adapter layer
        // (NOT vendored axi_master_write). The vendored single-shot wrapper
        // contains an internal fork/join; combining it with the top-level
        // fork below crosses Verilator 5.036's coroutine-split threshold
        // (`__Vfork_N__sync was not declared in this scope` C++ build error).
        // Adapter sequential is functionally equivalent for setup writes —
        // we only need the data to land in mem before the read phase, not
        // to exercise AW/W parallel issue.
        for (i = 0; i < 8; i = i + 1) begin
            dataW[0] = get_data(0) & get_mask(64'h0000_1100 + i*16, 16);
            r_expected[i] = dataW[0];
            bfm_post_aw(8'h81, 64'h0000_1100 + i*16, 1);
            bfm_post_w(8'h81, 64'h0000_1100 + i*16, 1);
            bfm_drain_b(8'h81);
        end

        fork
            begin : w_concurrent
                integer wi;
                for (wi = 0; wi < 8; wi = wi + 1)
                    bfm_post_aw(wi+1, 64'h0000_1000 + wi*16, 1);
                for (wi = 0; wi < 8; wi = wi + 1) begin
                    dataW[0] = get_data(0) & get_mask(64'h0000_1000 + wi*16, 16);
                    w_expected[wi] = dataW[0];
                    bfm_post_w(wi+1, 64'h0000_1000 + wi*16, 1);
                end
                for (wi = 0; wi < 8; wi = wi + 1) bfm_drain_b(wi+1);
            end
            begin : r_concurrent
                integer ri;
                for (ri = 0; ri < 8; ri = ri + 1)
                    bfm_post_ar(ri+1+8'h80, 64'h0000_1100 + ri*16, 1);
                for (ri = 0; ri < 8; ri = ri + 1) begin
                    bfm_drain_r(ri+1+8'h80, 1);
                    if ((dataR[0] & get_mask(64'h0000_1100 + ri*16, 16)) !== r_expected[ri]) begin
                        $display("[%0t] TASK F R mismatch ri=%0d D=0x%x exp=0x%x",
                                 $time, ri, dataR[0], r_expected[ri]);
                        error_flag = 1;
                        $fatal(1, "TASK F R data mismatch");
                    end
                end
            end
        join

        // Post-fork: verify writes landed — adapter layer for same reason as
        // pre-seed above (avoids vendored axi_master_read's internal fork).
        for (k = 0; k < 8; k = k + 1) begin
            bfm_post_ar(8'h82, 64'h0000_1000 + k*16, 1);
            bfm_drain_r(8'h82, 1);
            if ((dataR[0] & get_mask(64'h0000_1000 + k*16, 16)) !== w_expected[k]) begin
                $display("[%0t] TASK F W readback mismatch k=%0d D=0x%x exp=0x%x",
                         $time, k, dataR[0], w_expected[k]);
                error_flag = 1;
                $fatal(1, "TASK F W readback mismatch");
            end
        end
        $display("[%0t] TASK F PASS (mixed R+W concurrent)", $time);
    endtask

    // ---------- Task G: deep outstanding pressure ----------
    // Window 0x1400-0x1FFF; N in {8, 16}; blen=1; distinct IDs 1..N.
    // --timing mode in this simulator restricts `disable`; use a bounded
    // `while (!done)` cycle counter inside the test branch with
    // `fork ... join_none` + `wait fork`. No dangling watchdog thread.
    localparam int WATCHDOG_CYCLES = 2000;
    task test_deep_outstanding_pressure(input integer N);
        reg [WIDTH_DA-1:0] expected [0:31];
        integer i;
        time t_start, t_end;
        int cycle_count;
        bit done;
        // R-shadow barrier (consistent with other test wrappers).
        bfm_r_barrier;
        $display("[%0t] TASK G start: N=%0d deep pressure", $time, N);
        done = 0;
        cycle_count = 0;
        t_start = $time;

        fork
            begin : test_branch
                for (i = 0; i < N; i = i + 1)
                    bfm_post_aw(i+1, 64'h0000_1400 + i*16, 1);
                for (i = 0; i < N; i = i + 1) begin
                    dataW[0] = get_data(0) & get_mask(64'h0000_1400 + i*16, 16);
                    expected[i] = dataW[0];
                    bfm_post_w(i+1, 64'h0000_1400 + i*16, 1);
                end
                for (i = 0; i < N; i = i + 1) bfm_drain_b(i+1);
                for (i = 0; i < N; i = i + 1)
                    bfm_post_ar(i+1, 64'h0000_1400 + i*16, 1);
                for (i = 0; i < N; i = i + 1) begin
                    bfm_drain_r(i+1, 1);
                    if ((dataR[0] & get_mask(64'h0000_1400 + i*16, 16)) !== expected[i]) begin
                        error_flag = 1;
                        $fatal(1, "TASK G N=%0d data mismatch i=%0d", N, i);
                    end
                end
                done = 1;
            end
        join_none

        // Bounded watchdog poll — completes when done set OR cycles exceeded.
        while (!done && cycle_count < WATCHDOG_CYCLES) begin
            @(posedge ACLK);
            cycle_count = cycle_count + 1;
        end
        if (!done) begin
            $fatal(1, "TASK G watchdog fired (N=%0d, cycles=%0d): stall != deadlock evidence",
                   N, cycle_count);
        end
        wait fork;     // ensure test_branch fully completes (cleanup)
        t_end = $time;
        $display("[%0t] TASK G PASS N=%0d (duration=%0d ns, %0d cycles)",
                 $time, N, t_end - t_start, cycle_count);
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
