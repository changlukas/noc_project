`timescale 1ns/1ps

// tb_top — bidirectional 2-node router co-sim testbench
//
// Two symmetric nodes, each driving its own per-node router_wrap, joined by a
// cross-node link:
//   node k:  axi_master_wrap →[master_nmu_axi_k]→ nmu_wrap →[nodeK_nmu]→ ┐
//            router_wrap[k] ←link→ router_wrap[1-k] ┘
//            router_wrap[k] →[nodeK_nsu]→ nsu_wrap →[nsu_slave_axi_k]→ axi_slave_wrap
//   Response travels the opposite direction along the same path.
//
// Variant→instance pairing (spec §2/§5/§8): master at node k is fed the OTHER
// node's coordinate variant so traffic crosses the link:
//   node0.master ← scn_node1 (high addr, targets (1,0)) → ejects at node1.NSU →
//                  slave_1 ← scn_node1.
//   node1.master ← scn_node0 (low addr, targets (0,0))  → ejects at node0.NSU →
//                  slave_0 ← scn_node0.
// src_id matches each node's coordinate (NSU stamps response dst_id from
// request src_id): nmu_0/nsu_0 → 0, nmu_1/nsu_1 → 1.
//
// clk_i + rst_ni driven by C++ main.cpp (input ports — per Stage 5a pattern).
// Plusargs +scenario_node0=<path> +scenario_node1=<path> kick off the run;
// cmodel_init is called once with either variant (shared config).

`ifndef TB_TOP_SV
`define TB_TOP_SV

module tb_top (
    input logic clk_i,
    input logic rst_ni
);

    // -------------------------------------------------------------------------
    // Parameters
    // -------------------------------------------------------------------------
    localparam int unsigned ID_WIDTH              = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT;
    localparam int unsigned ADDR_WIDTH            = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT;
    localparam int unsigned DATA_WIDTH            = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT;
    localparam int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT;
    localparam int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT;
    localparam int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT;

    // -------------------------------------------------------------------------
    // DPI lifecycle
    // -------------------------------------------------------------------------
    import "DPI-C" context function void    cmodel_init(input string path);
    import "DPI-C" context function void    cmodel_finalize();
    import "DPI-C" context function int     cmodel_done();
    import "DPI-C" context function int     cmodel_scoreboard_clean();
    import "DPI-C" context function void    cmodel_dump_scoreboard();
    import "DPI-C" context function longint unsigned cmodel_router_create(input string name, input int x_coord);
    import "DPI-C" context function longint unsigned cmodel_master_create(input string name,
                                                                 input string scenario_path);
    import "DPI-C" context function longint unsigned cmodel_slave_create(input string name,
                                                                input string scenario_path);
    import "DPI-C" context function longint unsigned cmodel_nmu_create(input string name, input int src_id);
    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name, input int src_id);
    import "DPI-C" context function int     cmodel_master_count();
    import "DPI-C" context function int     cmodel_reads_checked();

    string  scn_node0;  // node0 coordinate variant (low addr);  drives node1.master
    string  scn_node1;  // node1 coordinate variant (high addr); drives node0.master
    longint unsigned router0_ctx, router1_ctx;
    longint unsigned m0_ctx, s0_ctx, n0_nmu_ctx, n0_nsu_ctx;
    longint unsigned m1_ctx, s1_ctx, n1_nmu_ctx, n1_nsu_ctx;

    initial begin
        if (!$value$plusargs("scenario_node0=%s", scn_node0) ||
            !$value$plusargs("scenario_node1=%s", scn_node1)) begin
            $display("ERROR: +scenario_node0=<path> +scenario_node1=<path> required");
            $finish(1);
        end
        cmodel_init(scn_node1);  // shared config; either variant is fine
        router0_ctx = cmodel_router_create("router_0", 0);
        router1_ctx = cmodel_router_create("router_1", 1);
        // node0.master drives node1-variant (high addr, targets (1,0)); ejects at node1.NSU.
        m0_ctx     = cmodel_master_create("master_0", scn_node1);
        s1_ctx     = cmodel_slave_create ("slave_1",  scn_node1);  // node1.slave: high range
        n0_nmu_ctx = cmodel_nmu_create("nmu_0", 0);                // src_id = node0 coordinate
        n0_nsu_ctx = cmodel_nsu_create("nsu_0", 0);
        // node1.master drives node0-variant (low addr, targets (0,0)); ejects at node0.NSU.
        m1_ctx     = cmodel_master_create("master_1", scn_node0);
        s0_ctx     = cmodel_slave_create ("slave_0",  scn_node0);  // node0.slave: low range
        n1_nmu_ctx = cmodel_nmu_create("nmu_1", 1);                // src_id = node1 coordinate
        n1_nsu_ctx = cmodel_nsu_create("nsu_1", 1);
    end

    // -------------------------------------------------------------------------
    // Wire bundles — 2 nodes × {master_nmu AXI, NMU-side NoC, NSU-side NoC, nsu_slave AXI}
    // -------------------------------------------------------------------------

    // node0
    axi4_intf #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) master_nmu_axi_0 ();
    noc_intf #(
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) node0_nmu ();
    noc_intf #(
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) node0_nsu ();
    axi4_intf #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) nsu_slave_axi_0 ();

    // node1
    axi4_intf #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) master_nmu_axi_1 ();
    noc_intf #(
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) node1_nmu ();
    noc_intf #(
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) node1_nsu ();
    axi4_intf #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) nsu_slave_axi_1 ();

    // -------------------------------------------------------------------------
    // Component instances — node0
    // -------------------------------------------------------------------------
    axi_master_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) u_master_0 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(m0_ctx), .axi_o(master_nmu_axi_0.master)
    );

    nmu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nmu_0 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(n0_nmu_ctx),
        .axi_i(master_nmu_axi_0.slave), .noc_mosi_o(node0_nmu.mosi)
    );

    nsu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nsu_0 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(n0_nsu_ctx),
        .noc_miso_i(node0_nsu.miso), .axi_o(nsu_slave_axi_0.master)
    );

    axi_slave_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) u_slave_0 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(s0_ctx), .axi_i(nsu_slave_axi_0.slave)
    );

    // -------------------------------------------------------------------------
    // Component instances — node1
    // -------------------------------------------------------------------------
    axi_master_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) u_master_1 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(m1_ctx), .axi_o(master_nmu_axi_1.master)
    );

    nmu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nmu_1 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(n1_nmu_ctx),
        .axi_i(master_nmu_axi_1.slave), .noc_mosi_o(node1_nmu.mosi)
    );

    nsu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nsu_1 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(n1_nsu_ctx),
        .noc_miso_i(node1_nsu.miso), .axi_o(nsu_slave_axi_1.master)
    );

    axi_slave_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)
    ) u_slave_1 (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(s1_ctx), .axi_i(nsu_slave_axi_1.slave)
    );

    // -------------------------------------------------------------------------
    // Router fabric — two per-node router_wraps joined by a cross-wired link
    // -------------------------------------------------------------------------
    // Link nets named <net>_<src>to<dst>_*: data flows src->dst, credit pulse
    // flows on the net whose direction matches the credit's travel. For the REQ
    // network, 0to1 carries node0's outbound data AND node0's returned credit
    // (credit for data node0 received from node1, travelling 0->1); 1to0
    // mirrors. Same for RSP. Each net has exactly one OUT driver + one IN reader.
    logic                  link_req_0to1_valid, link_req_1to0_valid;
    logic [FLIT_WIDTH-1:0] link_req_0to1_flit,  link_req_1to0_flit;
    logic                  link_req_0to1_credit, link_req_1to0_credit;  // pulse
    logic                  link_rsp_0to1_valid, link_rsp_1to0_valid;
    logic [FLIT_WIDTH-1:0] link_rsp_0to1_flit,  link_rsp_1to0_flit;
    logic                  link_rsp_0to1_credit, link_rsp_1to0_credit;  // pulse

    router_wrap #(
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_router_0 (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .ctx_i(router0_ctx),
        .noc_nmu_i(node0_nmu.miso),
        .noc_nsu_o(node0_nsu.mosi),
        // REQ link: node0 OUT -> 0to1 data; node0 IN <- 1to0 data.
        .link_req_out_valid(link_req_0to1_valid),
        .link_req_out_flit(link_req_0to1_flit),
        .link_req_out_credit(link_req_1to0_credit),  // credit for node0's sent data
        .link_req_in_valid(link_req_1to0_valid),
        .link_req_in_flit(link_req_1to0_flit),
        .link_req_in_credit(link_req_0to1_credit),   // credit node0 returns to node1
        // RSP link: mirrored.
        .link_rsp_out_valid(link_rsp_0to1_valid),
        .link_rsp_out_flit(link_rsp_0to1_flit),
        .link_rsp_out_credit(link_rsp_1to0_credit),
        .link_rsp_in_valid(link_rsp_1to0_valid),
        .link_rsp_in_flit(link_rsp_1to0_flit),
        .link_rsp_in_credit(link_rsp_0to1_credit)
    );

    router_wrap #(
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_router_1 (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .ctx_i(router1_ctx),
        .noc_nmu_i(node1_nmu.miso),
        .noc_nsu_o(node1_nsu.mosi),
        // REQ link: node1 OUT -> 1to0 data; node1 IN <- 0to1 data.
        .link_req_out_valid(link_req_1to0_valid),
        .link_req_out_flit(link_req_1to0_flit),
        .link_req_out_credit(link_req_0to1_credit),  // credit for node1's sent data
        .link_req_in_valid(link_req_0to1_valid),
        .link_req_in_flit(link_req_0to1_flit),
        .link_req_in_credit(link_req_1to0_credit),   // credit node1 returns to node0
        // RSP link: mirrored.
        .link_rsp_out_valid(link_rsp_1to0_valid),
        .link_rsp_out_flit(link_rsp_1to0_flit),
        .link_rsp_out_credit(link_rsp_0to1_credit),
        .link_rsp_in_valid(link_rsp_0to1_valid),
        .link_rsp_in_flit(link_rsp_0to1_flit),
        .link_rsp_in_credit(link_rsp_1to0_credit)
    );

    // -------------------------------------------------------------------------
    // PMU monitors — passive; no drives
    // -------------------------------------------------------------------------
    // AXI slot monitors: one per node × {manager, subordinate} edge.
    axi_perf_monitor #(
        .SLOT_NAME("node0.manager"), .ID_W($bits(master_nmu_axi_0.awid))
    ) u_perf_mgr_0 (
        .clk_i, .rst_ni,
        .awvalid(master_nmu_axi_0.awvalid), .awready(master_nmu_axi_0.awready),
        .awid(master_nmu_axi_0.awid),       .awaddr(master_nmu_axi_0.awaddr),
        .awlen(master_nmu_axi_0.awlen),     .awsize(master_nmu_axi_0.awsize),
        .wvalid(master_nmu_axi_0.wvalid),   .wready(master_nmu_axi_0.wready),
        .bvalid(master_nmu_axi_0.bvalid),   .bready(master_nmu_axi_0.bready),
        .bid(master_nmu_axi_0.bid),
        .arvalid(master_nmu_axi_0.arvalid), .arready(master_nmu_axi_0.arready),
        .arid(master_nmu_axi_0.arid),       .araddr(master_nmu_axi_0.araddr),
        .arlen(master_nmu_axi_0.arlen),     .arsize(master_nmu_axi_0.arsize),
        .rvalid(master_nmu_axi_0.rvalid),   .rready(master_nmu_axi_0.rready),
        .rlast(master_nmu_axi_0.rlast),     .rid(master_nmu_axi_0.rid)
    );

    axi_perf_monitor #(
        .SLOT_NAME("node1.manager"), .ID_W($bits(master_nmu_axi_1.awid))
    ) u_perf_mgr_1 (
        .clk_i, .rst_ni,
        .awvalid(master_nmu_axi_1.awvalid), .awready(master_nmu_axi_1.awready),
        .awid(master_nmu_axi_1.awid),       .awaddr(master_nmu_axi_1.awaddr),
        .awlen(master_nmu_axi_1.awlen),     .awsize(master_nmu_axi_1.awsize),
        .wvalid(master_nmu_axi_1.wvalid),   .wready(master_nmu_axi_1.wready),
        .bvalid(master_nmu_axi_1.bvalid),   .bready(master_nmu_axi_1.bready),
        .bid(master_nmu_axi_1.bid),
        .arvalid(master_nmu_axi_1.arvalid), .arready(master_nmu_axi_1.arready),
        .arid(master_nmu_axi_1.arid),       .araddr(master_nmu_axi_1.araddr),
        .arlen(master_nmu_axi_1.arlen),     .arsize(master_nmu_axi_1.arsize),
        .rvalid(master_nmu_axi_1.rvalid),   .rready(master_nmu_axi_1.rready),
        .rlast(master_nmu_axi_1.rlast),     .rid(master_nmu_axi_1.rid)
    );

    axi_perf_monitor #(
        .SLOT_NAME("node0.subordinate"), .ID_W($bits(nsu_slave_axi_0.awid))
    ) u_perf_sub_0 (
        .clk_i, .rst_ni,
        .awvalid(nsu_slave_axi_0.awvalid), .awready(nsu_slave_axi_0.awready),
        .awid(nsu_slave_axi_0.awid),       .awaddr(nsu_slave_axi_0.awaddr),
        .awlen(nsu_slave_axi_0.awlen),     .awsize(nsu_slave_axi_0.awsize),
        .wvalid(nsu_slave_axi_0.wvalid),   .wready(nsu_slave_axi_0.wready),
        .bvalid(nsu_slave_axi_0.bvalid),   .bready(nsu_slave_axi_0.bready),
        .bid(nsu_slave_axi_0.bid),
        .arvalid(nsu_slave_axi_0.arvalid), .arready(nsu_slave_axi_0.arready),
        .arid(nsu_slave_axi_0.arid),       .araddr(nsu_slave_axi_0.araddr),
        .arlen(nsu_slave_axi_0.arlen),     .arsize(nsu_slave_axi_0.arsize),
        .rvalid(nsu_slave_axi_0.rvalid),   .rready(nsu_slave_axi_0.rready),
        .rlast(nsu_slave_axi_0.rlast),     .rid(nsu_slave_axi_0.rid)
    );

    axi_perf_monitor #(
        .SLOT_NAME("node1.subordinate"), .ID_W($bits(nsu_slave_axi_1.awid))
    ) u_perf_sub_1 (
        .clk_i, .rst_ni,
        .awvalid(nsu_slave_axi_1.awvalid), .awready(nsu_slave_axi_1.awready),
        .awid(nsu_slave_axi_1.awid),       .awaddr(nsu_slave_axi_1.awaddr),
        .awlen(nsu_slave_axi_1.awlen),     .awsize(nsu_slave_axi_1.awsize),
        .wvalid(nsu_slave_axi_1.wvalid),   .wready(nsu_slave_axi_1.wready),
        .bvalid(nsu_slave_axi_1.bvalid),   .bready(nsu_slave_axi_1.bready),
        .bid(nsu_slave_axi_1.bid),
        .arvalid(nsu_slave_axi_1.arvalid), .arready(nsu_slave_axi_1.arready),
        .arid(nsu_slave_axi_1.arid),       .araddr(nsu_slave_axi_1.araddr),
        .arlen(nsu_slave_axi_1.arlen),     .arsize(nsu_slave_axi_1.arsize),
        .rvalid(nsu_slave_axi_1.rvalid),   .rready(nsu_slave_axi_1.rready),
        .rlast(nsu_slave_axi_1.rlast),     .rid(nsu_slave_axi_1.rid)
    );

    // Inter-router link monitors: valid paired with credit from the OPPOSITE direction
    // (credit for data flowing 0→1 returns on the 1→0 net, and vice versa).
    flit_link_perf_monitor #(
        .LINK_NAME("req_0to1"), .BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_perf_link_req01 (
        .clk_i, .rst_ni,
        .valid(link_req_0to1_valid), .credit_pulse(link_req_1to0_credit)
    );

    flit_link_perf_monitor #(
        .LINK_NAME("req_1to0"), .BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_perf_link_req10 (
        .clk_i, .rst_ni,
        .valid(link_req_1to0_valid), .credit_pulse(link_req_0to1_credit)
    );

    flit_link_perf_monitor #(
        .LINK_NAME("rsp_0to1"), .BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_perf_link_rsp01 (
        .clk_i, .rst_ni,
        .valid(link_rsp_0to1_valid), .credit_pulse(link_rsp_1to0_credit)
    );

    flit_link_perf_monitor #(
        .LINK_NAME("rsp_1to0"), .BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_perf_link_rsp10 (
        .clk_i, .rst_ni,
        .valid(link_rsp_1to0_valid), .credit_pulse(link_rsp_0to1_credit)
    );

    // -------------------------------------------------------------------------
    // Exit logic — non-vacuous PASS guard
    // -------------------------------------------------------------------------
    // Scenario completion is polled by C++ main.cpp via cmodel_done().
    // PASS requires a non-vacuous run: scoreboard clean AND both masters created
    // AND at least one read actually checked (so a write-only / no-op run fails).
    always @(posedge clk_i) begin
        /* verilator lint_off WIDTHTRUNC */
        if (rst_ni && (cmodel_done() != 0)) begin
            if (cmodel_scoreboard_clean() != 0 &&
                cmodel_master_count() == 2 && cmodel_reads_checked() > 0) begin
        /* verilator lint_on WIDTHTRUNC */
                $display("PASS: scenario complete, scoreboard clean");
                cmodel_dump_scoreboard();
                $finish(0);
            end else begin
                $display("FAIL: scoreboard mismatch or vacuous run (masters=%0d reads=%0d)",
                         cmodel_master_count(), cmodel_reads_checked());
                cmodel_dump_scoreboard();
                $fatal(1, "tb_top: bidirectional run failed");
            end
        end
    end

    // -------------------------------------------------------------------------
    // Centralized DPI error poll (T1.4)
    // -------------------------------------------------------------------------
    // Wraps no longer call cmodel_finalize individually; this single block
    // owns the fatal-exit path. Previously each wrap had its own error_check
    // block that called cmodel_finalize on non-zero, which risked a race where
    // multiple wraps tried to destruct the C++ model in the same delta cycle.
    import "DPI-C" context function int cmodel_check_error(output string msg);

    always_ff @(posedge clk_i) begin
        /* verilator lint_off WIDTHTRUNC */
        if (rst_ni) begin
            string dpi_err_msg;
            int    dpi_err_code;
            dpi_err_code = cmodel_check_error(dpi_err_msg);
            if (dpi_err_code != 0) begin
                $display("[tb_top] DPI fatal (code=%0d): %s",
                         dpi_err_code, dpi_err_msg);
                cmodel_dump_scoreboard();
                cmodel_finalize();
                $fatal(1, "tb_top: DPI error, simulation aborted");
            end
        end
        /* verilator lint_on WIDTHTRUNC */
    end

endmodule

`endif  // TB_TOP_SV
