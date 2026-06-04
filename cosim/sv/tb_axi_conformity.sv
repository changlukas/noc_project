`include "wb2axip/sim_wrapper.svh"

// tb_axi_conformity — AXI4 boundary conformity testbench
//
// Topology:
//   C-model DPI  →  nmu_cmodel_proxy  →  nmu_aif  →  faxi_slave  (NMU acts as AXI master;
//                                                                    faxi_slave checks master-side)
//   C-model DPI  →  nsu_cmodel_proxy  →  nsu_aif  →  faxi_master (NSU acts as AXI slave;
//                                                                    faxi_master checks slave-side)
//
// f_past_valid: both faxi_slave.v and faxi_master.v declare
//   "initial f_past_valid = 1'b0;" internally — no TB init required.

module tb_axi_conformity;
    localparam int ID_WIDTH   = 8;
    localparam int ADDR_WIDTH = 64;
    localparam int DATA_WIDTH = 256;
    localparam int F_LGDEPTH  = 10;

    logic clk;
    logic rst_n;

    string scenario_path;

    // DPI-C imports -------------------------------------------------------
    import "DPI-C" context function void cmodel_init(input string path);
    import "DPI-C" context function void cmodel_finalize();
    import "DPI-C" context function int  cmodel_done();
    import "DPI-C" context function int  cmodel_scoreboard_clean();

    initial begin
        if (!$value$plusargs("scenario=%s", scenario_path)) begin
            $display("ERROR: +scenario=<yaml-path> required");
            $finish(1);
        end
        cmodel_init(scenario_path);
    end

    // Clock + reset -------------------------------------------------------
    initial begin clk = 0; forever #5 clk = ~clk; end
    initial begin
        rst_n = 0;
        #20 rst_n = 1;
    end

    // AXI bundle interfaces -----------------------------------------------
    axi_if #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) nmu_aif (.clk(clk), .rst_n(rst_n));

    axi_if #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) nsu_aif (.clk(clk), .rst_n(rst_n));

    // C-model proxies -----------------------------------------------------
    nmu_cmodel_proxy #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) u_nmu_proxy (
        .clk(clk),
        .rst_n(rst_n),
        .aif(nmu_aif)
    );

    nsu_cmodel_proxy #(
        .ID_WIDTH(ID_WIDTH),
        .ADDR_WIDTH(ADDR_WIDTH),
        .DATA_WIDTH(DATA_WIDTH)
    ) u_nsu_proxy (
        .clk(clk),
        .rst_n(rst_n),
        .aif(nsu_aif)
    );

    // Induction counter wires (faxi_slave outputs) ------------------------
    logic [F_LGDEPTH-1:0] nmu_f_axi_awr_nbursts;
    logic [8:0]           nmu_f_axi_wr_pending;
    logic [F_LGDEPTH-1:0] nmu_f_axi_rd_nbursts;
    logic [F_LGDEPTH-1:0] nmu_f_axi_rd_outstanding;

    // Induction counter wires (faxi_master outputs) -----------------------
    logic [F_LGDEPTH-1:0] nsu_f_axi_awr_nbursts;
    logic [8:0]           nsu_f_axi_wr_pending;
    logic [F_LGDEPTH-1:0] nsu_f_axi_rd_nbursts;
    logic [F_LGDEPTH-1:0] nsu_f_axi_rd_outstanding;

    // wb2axip: NMU side — faxi_slave checks that NMU behaves as a
    // well-formed AXI4 master (assume master-driven channels, assert
    // slave-driven channels).
    faxi_slave #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH)
    ) u_nmu_check (
        .i_clk(clk),
        .i_axi_reset_n(rst_n),
        // AW
        .i_axi_awvalid(nmu_aif.awvalid),
        .i_axi_awready(nmu_aif.awready),
        .i_axi_awid(nmu_aif.awid),
        .i_axi_awaddr(nmu_aif.awaddr),
        .i_axi_awlen(nmu_aif.awlen),
        .i_axi_awsize(nmu_aif.awsize),
        .i_axi_awburst(nmu_aif.awburst),
        .i_axi_awlock(nmu_aif.awlock),
        .i_axi_awcache(nmu_aif.awcache),
        .i_axi_awprot(nmu_aif.awprot),
        .i_axi_awqos(nmu_aif.awqos),
        // W
        .i_axi_wvalid(nmu_aif.wvalid),
        .i_axi_wready(nmu_aif.wready),
        .i_axi_wdata(nmu_aif.wdata),
        .i_axi_wstrb(nmu_aif.wstrb),
        .i_axi_wlast(nmu_aif.wlast),
        // B
        .i_axi_bvalid(nmu_aif.bvalid),
        .i_axi_bready(nmu_aif.bready),
        .i_axi_bid(nmu_aif.bid),
        .i_axi_bresp(nmu_aif.bresp),
        // AR
        .i_axi_arvalid(nmu_aif.arvalid),
        .i_axi_arready(nmu_aif.arready),
        .i_axi_arid(nmu_aif.arid),
        .i_axi_araddr(nmu_aif.araddr),
        .i_axi_arlen(nmu_aif.arlen),
        .i_axi_arsize(nmu_aif.arsize),
        .i_axi_arburst(nmu_aif.arburst),
        .i_axi_arlock(nmu_aif.arlock),
        .i_axi_arcache(nmu_aif.arcache),
        .i_axi_arprot(nmu_aif.arprot),
        .i_axi_arqos(nmu_aif.arqos),
        // R
        .i_axi_rid(nmu_aif.rid),
        .i_axi_rresp(nmu_aif.rresp),
        .i_axi_rvalid(nmu_aif.rvalid),
        .i_axi_rdata(nmu_aif.rdata),
        .i_axi_rlast(nmu_aif.rlast),
        .i_axi_rready(nmu_aif.rready),
        // Induction outputs
        .f_axi_awr_nbursts(nmu_f_axi_awr_nbursts),
        .f_axi_wr_pending(nmu_f_axi_wr_pending),
        .f_axi_rd_nbursts(nmu_f_axi_rd_nbursts),
        .f_axi_rd_outstanding(nmu_f_axi_rd_outstanding)
    );

    // wb2axip: NSU side — faxi_master checks that NSU behaves as a
    // well-formed AXI4 slave (assume slave-driven channels, assert
    // master-driven channels — roles inverted relative to faxi_slave).
    faxi_master #(
        .C_AXI_ID_WIDTH(ID_WIDTH),
        .C_AXI_DATA_WIDTH(DATA_WIDTH),
        .C_AXI_ADDR_WIDTH(ADDR_WIDTH)
    ) u_nsu_check (
        .i_clk(clk),
        .i_axi_reset_n(rst_n),
        // AW
        .i_axi_awvalid(nsu_aif.awvalid),
        .i_axi_awready(nsu_aif.awready),
        .i_axi_awid(nsu_aif.awid),
        .i_axi_awaddr(nsu_aif.awaddr),
        .i_axi_awlen(nsu_aif.awlen),
        .i_axi_awsize(nsu_aif.awsize),
        .i_axi_awburst(nsu_aif.awburst),
        .i_axi_awlock(nsu_aif.awlock),
        .i_axi_awcache(nsu_aif.awcache),
        .i_axi_awprot(nsu_aif.awprot),
        .i_axi_awqos(nsu_aif.awqos),
        // W
        .i_axi_wvalid(nsu_aif.wvalid),
        .i_axi_wready(nsu_aif.wready),
        .i_axi_wdata(nsu_aif.wdata),
        .i_axi_wstrb(nsu_aif.wstrb),
        .i_axi_wlast(nsu_aif.wlast),
        // B
        .i_axi_bvalid(nsu_aif.bvalid),
        .i_axi_bready(nsu_aif.bready),
        .i_axi_bid(nsu_aif.bid),
        .i_axi_bresp(nsu_aif.bresp),
        // AR
        .i_axi_arvalid(nsu_aif.arvalid),
        .i_axi_arready(nsu_aif.arready),
        .i_axi_arid(nsu_aif.arid),
        .i_axi_araddr(nsu_aif.araddr),
        .i_axi_arlen(nsu_aif.arlen),
        .i_axi_arsize(nsu_aif.arsize),
        .i_axi_arburst(nsu_aif.arburst),
        .i_axi_arlock(nsu_aif.arlock),
        .i_axi_arcache(nsu_aif.arcache),
        .i_axi_arprot(nsu_aif.arprot),
        .i_axi_arqos(nsu_aif.arqos),
        // R
        .i_axi_rid(nsu_aif.rid),
        .i_axi_rresp(nsu_aif.rresp),
        .i_axi_rvalid(nsu_aif.rvalid),
        .i_axi_rdata(nsu_aif.rdata),
        .i_axi_rlast(nsu_aif.rlast),
        .i_axi_rready(nsu_aif.rready),
        // Induction outputs
        .f_axi_awr_nbursts(nsu_f_axi_awr_nbursts),
        .f_axi_wr_pending(nsu_f_axi_wr_pending),
        .f_axi_rd_nbursts(nsu_f_axi_rd_nbursts),
        .f_axi_rd_outstanding(nsu_f_axi_rd_outstanding)
    );

    // Exit when scenario complete -----------------------------------------
    always @(posedge clk) begin
        if (rst_n && cmodel_done()) begin
            if (cmodel_scoreboard_clean())
                $display("PASS: scenario complete, scoreboard clean");
            else begin
                $display("FAIL: scoreboard mismatch");
                $finish(1);
            end
            cmodel_finalize();
            $finish(0);
        end
    end

    // Safety timeout ------------------------------------------------------
    initial begin
        #1_000_000;
        $display("FAIL: timeout (1ms simulated)");
        $finish(1);
    end

endmodule
