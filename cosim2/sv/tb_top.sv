`timescale 1ns/1ps
`include "wb2axip/sim_wrapper.svh"

// tb_top — Stage 5b wire-level co-sim testbench
//
// Per spec §4.1 topology:
//   axi_master_wrap →[master_nmu_axi]→ nmu_wrap →[nmu_loopback_req]→ loopback_noc_wrap
//                                              ←[loopback_nmu_rsp]←
//   loopback_noc_wrap →[loopback_nsu_req]→ nsu_wrap →[nsu_slave_axi]→ axi_slave_wrap
//                    ←[nsu_loopback_rsp]←
//
//   wb2axip faxi_slave  bound on master_nmu_axi (NMU manager-facing: checks NMU as AXI master)
//   wb2axip faxi_master bound on nsu_slave_axi  (NSU memory-facing: checks NSU as AXI slave)
//
// clk_i + rst_ni driven by C++ main.cpp (input ports — per Stage 5a pattern).
// Plusarg +scenario=<yaml-path> kicks off cmodel_init().
// Exit logic deferred to main.cpp (polls cmodel_done() per scenario_parser).
//
// MAXSTALL=32, MAXRSTALL=32, MAXDELAY=88 verified in T12 (spec §5.2.1).

`ifndef TB_TOP_SV
`define TB_TOP_SV

module tb_top (
    input logic clk_i,
    input logic rst_ni
);

    // -------------------------------------------------------------------------
    // Parameters
    // -------------------------------------------------------------------------
    localparam int ID_WIDTH   = 8;
    localparam int ADDR_WIDTH = 64;
    localparam int DATA_WIDTH = 256;
    localparam int NUM_VC     = 1;
    localparam int FLIT_W     = 408;  // ni::FLIT_WIDTH — project canonical

    // wb2axip parametric override per spec §5.2.1 (T12 verified values)
    localparam int F_LGDEPTH_VAL        = 10;  // default; accommodates MAXSTALL=32 (needs 6 bits)
    localparam int F_AXI_MAXSTALL_VAL   = 32;
    localparam int F_AXI_MAXRSTALL_VAL  = 32;
    localparam int F_AXI_MAXDELAY_VAL   = 88;

    // -------------------------------------------------------------------------
    // DPI lifecycle
    // -------------------------------------------------------------------------
    import "DPI-C" context function void cmodel_init(input string path);
    import "DPI-C" context function void cmodel_finalize();
    import "DPI-C" context function int  cmodel_done();
    import "DPI-C" context function int  cmodel_scoreboard_clean();

    string scenario_path;

    initial begin
        if (!$value$plusargs("scenario=%s", scenario_path)) begin
            $display("ERROR: +scenario=<yaml-path> required");
            $finish(1);
        end
        cmodel_init(scenario_path);
    end

    // -------------------------------------------------------------------------
    // 6 wire bundles (interfaces)
    // -------------------------------------------------------------------------

    // [1] master_nmu_axi — AXI between axi_master_wrap (master) and nmu_wrap (slave)
    axi_intf #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) master_nmu_axi (.clk_i(clk_i), .rst_ni(rst_ni));

    // [2] nmu_loopback_req — NoC request from nmu_wrap to loopback_noc_wrap
    noc_req_intf #(
        .NUM_VC(NUM_VC),
        .FLIT_W(FLIT_W)
    ) nmu_loopback_req (.clk_i(clk_i), .rst_ni(rst_ni));

    // [3] loopback_nmu_rsp — NoC response from loopback_noc_wrap back to nmu_wrap
    noc_rsp_intf #(
        .NUM_VC(NUM_VC),
        .FLIT_W(FLIT_W)
    ) loopback_nmu_rsp (.clk_i(clk_i), .rst_ni(rst_ni));

    // [4] loopback_nsu_req — NoC request from loopback_noc_wrap to nsu_wrap
    noc_req_intf #(
        .NUM_VC(NUM_VC),
        .FLIT_W(FLIT_W)
    ) loopback_nsu_req (.clk_i(clk_i), .rst_ni(rst_ni));

    // [5] nsu_loopback_rsp — NoC response from nsu_wrap back to loopback_noc_wrap
    noc_rsp_intf #(
        .NUM_VC(NUM_VC),
        .FLIT_W(FLIT_W)
    ) nsu_loopback_rsp (.clk_i(clk_i), .rst_ni(rst_ni));

    // [6] nsu_slave_axi — AXI between nsu_wrap (master) and axi_slave_wrap (slave)
    axi_intf #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) nsu_slave_axi (.clk_i(clk_i), .rst_ni(rst_ni));

    // -------------------------------------------------------------------------
    // 5 component instances
    // -------------------------------------------------------------------------

    // [1] AXI traffic generator — drives master_nmu_axi as AXI master
    axi_master_wrap #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) u_master (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .axi_o(master_nmu_axi.master)
    );

    // [2] NMU shell — AXI slave in, NoC producer/consumer out
    nmu_wrap #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC),
        .FLIT_W(FLIT_W)
    ) u_nmu (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .axi_i(master_nmu_axi.slave),
        .noc_req_o(nmu_loopback_req.producer),
        .noc_rsp_i(loopback_nmu_rsp.consumer)
    );

    // [3] Loopback NoC — routes NMU requests to NSU and NSU responses back
    loopback_noc_wrap #(
        .NUM_VC(NUM_VC),
        .FLIT_W(FLIT_W)
    ) u_loopback (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .req_from_nmu_i(nmu_loopback_req.consumer),
        .req_to_nsu_o(loopback_nsu_req.producer),
        .rsp_from_nsu_i(nsu_loopback_rsp.consumer),
        .rsp_to_nmu_o(loopback_nmu_rsp.producer)
    );

    // [4] NSU shell — NoC consumer/producer in, AXI master out
    nsu_wrap #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC),
        .FLIT_W(FLIT_W)
    ) u_nsu (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .noc_req_i(loopback_nsu_req.consumer),
        .noc_rsp_o(nsu_loopback_rsp.producer),
        .axi_o(nsu_slave_axi.master)
    );

    // [5] AXI memory model — receives transactions on nsu_slave_axi as AXI slave
    axi_slave_wrap #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) u_slave (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .axi_i(nsu_slave_axi.slave)
    );

    // -------------------------------------------------------------------------
    // Induction output wires (faxi_slave / faxi_master outputs — 4 per checker)
    // -------------------------------------------------------------------------

    // NMU boundary (faxi_slave outputs)
    logic [F_LGDEPTH_VAL-1:0] nmu_f_axi_awr_nbursts;
    logic [8:0]               nmu_f_axi_wr_pending;
    logic [F_LGDEPTH_VAL-1:0] nmu_f_axi_rd_nbursts;
    logic [F_LGDEPTH_VAL-1:0] nmu_f_axi_rd_outstanding;

    // NSU boundary (faxi_master outputs)
    logic [F_LGDEPTH_VAL-1:0] nsu_f_axi_awr_nbursts;
    logic [8:0]               nsu_f_axi_wr_pending;
    logic [F_LGDEPTH_VAL-1:0] nsu_f_axi_rd_nbursts;
    logic [F_LGDEPTH_VAL-1:0] nsu_f_axi_rd_outstanding;

    // -------------------------------------------------------------------------
    // wb2axip checker 1: faxi_slave on master_nmu_axi
    // Checks that NMU behaves as a well-formed AXI4 master (assume
    // master-driven channels, assert slave-driven channels).
    // OPT_EXCLUSIVE=0: exclusive-access state machine omitted; c_model does
    // not generate lock transactions in this testbench.
    // PINMISSING suppress: upstream wb2axip faxi_slave.v has a trailing-comma
    // null port (line 135); all real induction outputs are connected above.
    // -------------------------------------------------------------------------
    /* verilator lint_off PINMISSING */
    faxi_slave #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH),
        .OPT_EXCLUSIVE(0),
        .F_LGDEPTH(F_LGDEPTH_VAL),
        .F_AXI_MAXSTALL(F_AXI_MAXSTALL_VAL),
        .F_AXI_MAXRSTALL(F_AXI_MAXRSTALL_VAL),
        .F_AXI_MAXDELAY(F_AXI_MAXDELAY_VAL)
    ) u_nmu_check (
        .i_clk(clk_i),
        .i_axi_reset_n(rst_ni),
        // AW
        .i_axi_awvalid(master_nmu_axi.awvalid),
        .i_axi_awready(master_nmu_axi.awready),
        .i_axi_awid(master_nmu_axi.awid),
        .i_axi_awaddr(master_nmu_axi.awaddr),
        .i_axi_awlen(master_nmu_axi.awlen),
        .i_axi_awsize(master_nmu_axi.awsize),
        .i_axi_awburst(master_nmu_axi.awburst),
        .i_axi_awlock(master_nmu_axi.awlock),
        .i_axi_awcache(master_nmu_axi.awcache),
        .i_axi_awprot(master_nmu_axi.awprot),
        .i_axi_awqos(master_nmu_axi.awqos),
        // W
        .i_axi_wvalid(master_nmu_axi.wvalid),
        .i_axi_wready(master_nmu_axi.wready),
        .i_axi_wdata(master_nmu_axi.wdata),
        .i_axi_wstrb(master_nmu_axi.wstrb),
        .i_axi_wlast(master_nmu_axi.wlast),
        // B
        .i_axi_bvalid(master_nmu_axi.bvalid),
        .i_axi_bready(master_nmu_axi.bready),
        .i_axi_bid(master_nmu_axi.bid),
        .i_axi_bresp(master_nmu_axi.bresp),
        // AR
        .i_axi_arvalid(master_nmu_axi.arvalid),
        .i_axi_arready(master_nmu_axi.arready),
        .i_axi_arid(master_nmu_axi.arid),
        .i_axi_araddr(master_nmu_axi.araddr),
        .i_axi_arlen(master_nmu_axi.arlen),
        .i_axi_arsize(master_nmu_axi.arsize),
        .i_axi_arburst(master_nmu_axi.arburst),
        .i_axi_arlock(master_nmu_axi.arlock),
        .i_axi_arcache(master_nmu_axi.arcache),
        .i_axi_arprot(master_nmu_axi.arprot),
        .i_axi_arqos(master_nmu_axi.arqos),
        // R
        .i_axi_rid(master_nmu_axi.rid),
        .i_axi_rresp(master_nmu_axi.rresp),
        .i_axi_rvalid(master_nmu_axi.rvalid),
        .i_axi_rdata(master_nmu_axi.rdata),
        .i_axi_rlast(master_nmu_axi.rlast),
        .i_axi_rready(master_nmu_axi.rready),
        // Induction outputs
        .f_axi_awr_nbursts(nmu_f_axi_awr_nbursts),
        .f_axi_wr_pending(nmu_f_axi_wr_pending),
        .f_axi_rd_nbursts(nmu_f_axi_rd_nbursts),
        .f_axi_rd_outstanding(nmu_f_axi_rd_outstanding)
    );
    /* verilator lint_on PINMISSING */

    // -------------------------------------------------------------------------
    // wb2axip checker 2: faxi_master on nsu_slave_axi
    // Checks that NSU behaves as a well-formed AXI4 slave (assume
    // slave-driven channels, assert master-driven channels — roles inverted
    // relative to faxi_slave).
    // OPT_EXCLUSIVE=0: exclusive-access state machine omitted; same rationale.
    // PINMISSING suppress: same upstream null-port issue as faxi_slave above.
    // -------------------------------------------------------------------------
    /* verilator lint_off PINMISSING */
    faxi_master #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH),
        .OPT_EXCLUSIVE(0),
        .F_LGDEPTH(F_LGDEPTH_VAL),
        .F_AXI_MAXSTALL(F_AXI_MAXSTALL_VAL),
        .F_AXI_MAXRSTALL(F_AXI_MAXRSTALL_VAL),
        .F_AXI_MAXDELAY(F_AXI_MAXDELAY_VAL)
    ) u_nsu_check (
        .i_clk(clk_i),
        .i_axi_reset_n(rst_ni),
        // AW
        .i_axi_awvalid(nsu_slave_axi.awvalid),
        .i_axi_awready(nsu_slave_axi.awready),
        .i_axi_awid(nsu_slave_axi.awid),
        .i_axi_awaddr(nsu_slave_axi.awaddr),
        .i_axi_awlen(nsu_slave_axi.awlen),
        .i_axi_awsize(nsu_slave_axi.awsize),
        .i_axi_awburst(nsu_slave_axi.awburst),
        .i_axi_awlock(nsu_slave_axi.awlock),
        .i_axi_awcache(nsu_slave_axi.awcache),
        .i_axi_awprot(nsu_slave_axi.awprot),
        .i_axi_awqos(nsu_slave_axi.awqos),
        // W
        .i_axi_wvalid(nsu_slave_axi.wvalid),
        .i_axi_wready(nsu_slave_axi.wready),
        .i_axi_wdata(nsu_slave_axi.wdata),
        .i_axi_wstrb(nsu_slave_axi.wstrb),
        .i_axi_wlast(nsu_slave_axi.wlast),
        // B
        .i_axi_bvalid(nsu_slave_axi.bvalid),
        .i_axi_bready(nsu_slave_axi.bready),
        .i_axi_bid(nsu_slave_axi.bid),
        .i_axi_bresp(nsu_slave_axi.bresp),
        // AR
        .i_axi_arvalid(nsu_slave_axi.arvalid),
        .i_axi_arready(nsu_slave_axi.arready),
        .i_axi_arid(nsu_slave_axi.arid),
        .i_axi_araddr(nsu_slave_axi.araddr),
        .i_axi_arlen(nsu_slave_axi.arlen),
        .i_axi_arsize(nsu_slave_axi.arsize),
        .i_axi_arburst(nsu_slave_axi.arburst),
        .i_axi_arlock(nsu_slave_axi.arlock),
        .i_axi_arcache(nsu_slave_axi.arcache),
        .i_axi_arprot(nsu_slave_axi.arprot),
        .i_axi_arqos(nsu_slave_axi.arqos),
        // R
        .i_axi_rid(nsu_slave_axi.rid),
        .i_axi_rresp(nsu_slave_axi.rresp),
        .i_axi_rvalid(nsu_slave_axi.rvalid),
        .i_axi_rdata(nsu_slave_axi.rdata),
        .i_axi_rlast(nsu_slave_axi.rlast),
        .i_axi_rready(nsu_slave_axi.rready),
        // Induction outputs
        .f_axi_awr_nbursts(nsu_f_axi_awr_nbursts),
        .f_axi_wr_pending(nsu_f_axi_wr_pending),
        .f_axi_rd_nbursts(nsu_f_axi_rd_nbursts),
        .f_axi_rd_outstanding(nsu_f_axi_rd_outstanding)
    );
    /* verilator lint_on PINMISSING */

    // -------------------------------------------------------------------------
    // Exit logic
    // -------------------------------------------------------------------------
    // Scenario completion is polled by C++ main.cpp via cmodel_done().
    // This always block provides a SV-side mirror for simulation observers.
    // cmodel_done/cmodel_scoreboard_clean return int; cast to bit for Verilator.
    always @(posedge clk_i) begin
        /* verilator lint_off WIDTHTRUNC */
        if (rst_ni && (cmodel_done() != 0)) begin
            if (cmodel_scoreboard_clean() != 0) begin
        /* verilator lint_on WIDTHTRUNC */
                $display("PASS: scenario complete, scoreboard clean");
                cmodel_finalize();
                $finish(0);
            end else begin
                $display("FAIL: scoreboard mismatch");
                cmodel_finalize();
                $finish(1);
            end
        end
    end

endmodule

`endif  // TB_TOP_SV
