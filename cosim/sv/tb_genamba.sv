// gen_amba role-1 testbench top — T3 BFM->NMU->NoC->NSU->mem_axi bridge
`timescale 1ns/1ps
`include "wb2axip/sim_wrapper.svh"

module tb_genamba;
    reg ACLK = 0;
    always #5 ACLK = ~ACLK;          // 10 ns period
    reg ARESETn = 0;

    // ---- DPI imports + chandle storage (copy of tb_top.sv:47-63 pattern) ----
    import "DPI-C" context function void    cmodel_init(input string path);
    import "DPI-C" context function void    cmodel_finalize();
    import "DPI-C" context function chandle cmodel_channel_model_create(input string name);
    import "DPI-C" context function chandle cmodel_master_create(input string name);
    import "DPI-C" context function chandle cmodel_slave_create(input string name);
    import "DPI-C" context function chandle cmodel_nmu_create(input string name);
    import "DPI-C" context function chandle cmodel_nsu_create(input string name);
    chandle cm_ctx, master_ctx, slave_ctx, nmu_ctx, nsu_ctx;

    // ---- AXI + NoC interface bundles ----
    axi4_intf #(.ID_WIDTH(8), .ADDR_WIDTH(64), .DATA_WIDTH(256)) bfm_nmu_axi();
    axi4_intf #(.ID_WIDTH(8), .ADDR_WIDTH(64), .DATA_WIDTH(256)) nsu_mem_axi();
    noc_intf  #(.NUM_VC(1), .FLIT_WIDTH(408)) noc_link();

    // Tie REGION (not marshalled by DPI per spec §3.2)
    assign bfm_nmu_axi.awregion = 4'b0;
    assign bfm_nmu_axi.arregion = 4'b0;

    // ---- BFM ↔ bfm_nmu_axi (BFM uppercase port → axi4_intf lowercase) ----
    genamba_master_bfm #(.WIDTH_AD(64), .WIDTH_DA(256), .WIDTH_ID(8), .P_MST_ID(0)) u_bfm (
        .ACLK(ACLK), .ARESETn(ARESETn),
        .AWID(bfm_nmu_axi.awid), .AWADDR(bfm_nmu_axi.awaddr),
        .AWLEN(bfm_nmu_axi.awlen), .AWSIZE(bfm_nmu_axi.awsize),
        .AWBURST(bfm_nmu_axi.awburst), .AWLOCK(bfm_nmu_axi.awlock),
        .AWCACHE(bfm_nmu_axi.awcache), .AWPROT(bfm_nmu_axi.awprot),
        .AWQOS(bfm_nmu_axi.awqos),
        .AWVALID(bfm_nmu_axi.awvalid), .AWREADY(bfm_nmu_axi.awready),
        .WDATA(bfm_nmu_axi.wdata), .WSTRB(bfm_nmu_axi.wstrb),
        .WLAST(bfm_nmu_axi.wlast),
        .WVALID(bfm_nmu_axi.wvalid), .WREADY(bfm_nmu_axi.wready),
        .BID(bfm_nmu_axi.bid), .BRESP(bfm_nmu_axi.bresp),
        .BVALID(bfm_nmu_axi.bvalid), .BREADY(bfm_nmu_axi.bready),
        .ARID(bfm_nmu_axi.arid), .ARADDR(bfm_nmu_axi.araddr),
        .ARLEN(bfm_nmu_axi.arlen), .ARSIZE(bfm_nmu_axi.arsize),
        .ARBURST(bfm_nmu_axi.arburst), .ARLOCK(bfm_nmu_axi.arlock),
        .ARCACHE(bfm_nmu_axi.arcache), .ARPROT(bfm_nmu_axi.arprot),
        .ARQOS(bfm_nmu_axi.arqos),
        .ARVALID(bfm_nmu_axi.arvalid), .ARREADY(bfm_nmu_axi.arready),
        .RID(bfm_nmu_axi.rid), .RDATA(bfm_nmu_axi.rdata),
        .RRESP(bfm_nmu_axi.rresp), .RLAST(bfm_nmu_axi.rlast),
        .RVALID(bfm_nmu_axi.rvalid), .RREADY(bfm_nmu_axi.rready)
    );

    // ---- DUT bridge: NMU + NSU on one noc_intf ----
    // ctx_i: chandle context for per-instance DPI tick (NMU/NSU wrap each
    // own one); verified against tb_top.sv:137,166 — mandatory port.
    nmu_wrap u_nmu (
        .clk_i(ACLK), .rst_ni(ARESETn),
        .ctx_i(nmu_ctx),
        .axi_i(bfm_nmu_axi.slave),
        .noc_mosi_o(noc_link.mosi)
    );
    nsu_wrap u_nsu (
        .clk_i(ACLK), .rst_ni(ARESETn),
        .ctx_i(nsu_ctx),
        .noc_miso_i(noc_link.miso),         // _i suffix per nsu_wrap.sv:44
        .axi_o(nsu_mem_axi.master)
    );

    // ---- nsu_mem_axi ↔ mem_axi (axi4_intf lowercase → UPPERCASE) ----
    mem_axi #(
        .AXI_WIDTH_CID(0), .AXI_WIDTH_ID(8), .AXI_WIDTH_AD(64),
        .AXI_WIDTH_DA(256), .SIZE_IN_BYTES(16384)
    ) u_mem (
        .ARESETn(ARESETn), .ACLK(ACLK), .CSYSREQ(1'b1), .CSYSACK(), .CACTIVE(),
        .AWID(nsu_mem_axi.awid), .AWADDR(nsu_mem_axi.awaddr),
        .AWLEN(nsu_mem_axi.awlen), .AWSIZE(nsu_mem_axi.awsize),
        .AWBURST(nsu_mem_axi.awburst), .AWLOCK(nsu_mem_axi.awlock),
        .AWCACHE(nsu_mem_axi.awcache), .AWPROT(nsu_mem_axi.awprot),
        .AWQOS(nsu_mem_axi.awqos), .AWREGION(4'b0),
        .AWVALID(nsu_mem_axi.awvalid), .AWREADY(nsu_mem_axi.awready),
        .WDATA(nsu_mem_axi.wdata), .WSTRB(nsu_mem_axi.wstrb),
        .WLAST(nsu_mem_axi.wlast),
        .WVALID(nsu_mem_axi.wvalid), .WREADY(nsu_mem_axi.wready),
        .BID(nsu_mem_axi.bid), .BRESP(nsu_mem_axi.bresp),
        .BVALID(nsu_mem_axi.bvalid), .BREADY(nsu_mem_axi.bready),
        .ARID(nsu_mem_axi.arid), .ARADDR(nsu_mem_axi.araddr),
        .ARLEN(nsu_mem_axi.arlen), .ARSIZE(nsu_mem_axi.arsize),
        .ARBURST(nsu_mem_axi.arburst), .ARLOCK(nsu_mem_axi.arlock),
        .ARCACHE(nsu_mem_axi.arcache), .ARPROT(nsu_mem_axi.arprot),
        .ARQOS(nsu_mem_axi.arqos), .ARREGION(4'b0),
        .ARVALID(nsu_mem_axi.arvalid), .ARREADY(nsu_mem_axi.arready),
        .RID(nsu_mem_axi.rid), .RDATA(nsu_mem_axi.rdata),
        .RRESP(nsu_mem_axi.rresp), .RLAST(nsu_mem_axi.rlast),
        .RVALID(nsu_mem_axi.rvalid), .RREADY(nsu_mem_axi.rready)
    );

    initial begin
        // DPI lifecycle (BEFORE reset deassert, per tb_top.sv:70-76 pattern)
        string scenario_path;
        if (!$value$plusargs("scenario=%s", scenario_path))
            scenario_path = "../../tests/scenarios/AX4-BAS-001_single_write_no_read/scenario.yaml";
        cmodel_init(scenario_path);
        cm_ctx     = cmodel_channel_model_create("channel_model_0");
        master_ctx = cmodel_master_create("master_0");
        slave_ctx  = cmodel_slave_create("slave_0");
        nmu_ctx    = cmodel_nmu_create("nmu_0");
        nsu_ctx    = cmodel_nsu_create("nsu_0");

        repeat (4) @(posedge ACLK);
        ARESETn = 1'b1;
        repeat (10) @(posedge ACLK);
        u_bfm.test_baseline_mem_test;

        u_bfm.test_burst_blen(4);
        u_bfm.test_burst_blen(8);
        u_bfm.test_burst_blen(16);

        repeat (50) @(posedge ACLK);
        $display("[%0t] tb_genamba: T3 PASS (BFM->NMU->NoC->NSU->mem mem_test)", $time);
        $finish;
    end

    final begin
        cmodel_finalize();
    end

    // ---- DPI per-cycle error pump (copy of tb_top.sv:374-388 pattern) ----
    import "DPI-C" context function int cmodel_check_error(output string msg);
    /* verilator lint_off WIDTHTRUNC */
    always_ff @(posedge ACLK) begin
        if (ARESETn) begin
            string dpi_err_msg;
            int    dpi_err_code;
            dpi_err_code = cmodel_check_error(dpi_err_msg);
            if (dpi_err_code != 0) begin
                $display("[%0t] DPI error code=%0d msg=%s", $time, dpi_err_code, dpi_err_msg);
                cmodel_finalize();
                $fatal(1, "DPI error pump fired");
            end
        end
    end
    /* verilator lint_on WIDTHTRUNC */

    // ---- T5 hang debug monitors (project code, not vendored) ----
    // Watchdog: kill sim if no AXI activity for 1us — turns silent hang into
    // a finite log we can hand to Codex. Includes AR/R channels — mem_test
    // alternates write-burst then read-burst phases, so a W-only watchdog
    // false-fires during the read phase.
    integer last_event_time = 0;
    always @(posedge ACLK) begin
        if (bfm_nmu_axi.awvalid || bfm_nmu_axi.wvalid || bfm_nmu_axi.bvalid ||
            bfm_nmu_axi.arvalid || bfm_nmu_axi.rvalid ||
            nsu_mem_axi.awvalid || nsu_mem_axi.wvalid || nsu_mem_axi.bvalid ||
            nsu_mem_axi.arvalid || nsu_mem_axi.rvalid)
            last_event_time = $time;
        if (ARESETn && ($time - last_event_time) > 1000) begin
            $display("[%0t] WATCHDOG: 1us of AXI silence, dumping final state", $time);
            $display("  BFM-side : AWV=%b AWR=%b AWID=0x%0h AWADDR=0x%0h AWLEN=%0d",
                     bfm_nmu_axi.awvalid, bfm_nmu_axi.awready,
                     bfm_nmu_axi.awid, bfm_nmu_axi.awaddr, bfm_nmu_axi.awlen);
            $display("  BFM-side : WV=%b WR=%b WLAST=%b WSTRB=0x%0h",
                     bfm_nmu_axi.wvalid, bfm_nmu_axi.wready,
                     bfm_nmu_axi.wlast, bfm_nmu_axi.wstrb);
            $display("  BFM-side : BV=%b BR=%b BID=0x%0h BRESP=%0d",
                     bfm_nmu_axi.bvalid, bfm_nmu_axi.bready,
                     bfm_nmu_axi.bid, bfm_nmu_axi.bresp);
            $display("  NSU-side : AWV=%b AWR=%b AWID=0x%0h AWLEN=%0d",
                     nsu_mem_axi.awvalid, nsu_mem_axi.awready,
                     nsu_mem_axi.awid, nsu_mem_axi.awlen);
            $display("  NSU-side : WV=%b WR=%b WLAST=%b",
                     nsu_mem_axi.wvalid, nsu_mem_axi.wready, nsu_mem_axi.wlast);
            $display("  NSU-side : BV=%b BR=%b BID=0x%0h",
                     nsu_mem_axi.bvalid, nsu_mem_axi.bready, nsu_mem_axi.bid);
            $fatal(1, "WATCHDOG: AXI silence");
        end
    end

    // Per-cycle AXI handshake monitors — first 30 W beats only to bound log.
    // Opt-in via `+define+GENAMBA_DBG_AXI` (default off — runs stay quiet).
`ifdef GENAMBA_DBG_AXI
    integer w_beat_count = 0;
    always @(posedge ACLK) begin
        if (ARESETn && bfm_nmu_axi.awvalid && bfm_nmu_axi.awready)
            $display("[%0t] DBG BFM AW handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, bfm_nmu_axi.awid, bfm_nmu_axi.awaddr, bfm_nmu_axi.awlen);
        if (ARESETn && bfm_nmu_axi.wvalid && bfm_nmu_axi.wready && w_beat_count < 30) begin
            $display("[%0t] DBG BFM W#%0d: WLAST=%b WSTRB=0x%0h DATA[31:0]=0x%0h",
                     $time, w_beat_count, bfm_nmu_axi.wlast,
                     bfm_nmu_axi.wstrb[31:0], bfm_nmu_axi.wdata[31:0]);
            w_beat_count = w_beat_count + 1;
        end
        if (ARESETn && bfm_nmu_axi.bvalid && bfm_nmu_axi.bready)
            $display("[%0t] DBG BFM B handshake: BID=0x%0h BRESP=%0d",
                     $time, bfm_nmu_axi.bid, bfm_nmu_axi.bresp);
        if (ARESETn && nsu_mem_axi.awvalid && nsu_mem_axi.awready)
            $display("[%0t] DBG NSU AW handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, nsu_mem_axi.awid, nsu_mem_axi.awaddr, nsu_mem_axi.awlen);
        if (ARESETn && nsu_mem_axi.wvalid && nsu_mem_axi.wready)
            $display("[%0t] DBG NSU W: WLAST=%b WSTRB=0x%0h",
                     $time, nsu_mem_axi.wlast, nsu_mem_axi.wstrb[31:0]);
        if (ARESETn && nsu_mem_axi.bvalid && nsu_mem_axi.bready)
            $display("[%0t] DBG NSU B: BID=0x%0h BRESP=%0d",
                     $time, nsu_mem_axi.bid, nsu_mem_axi.bresp);
        if (ARESETn && bfm_nmu_axi.arvalid && bfm_nmu_axi.arready)
            $display("[%0t] DBG BFM AR handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, bfm_nmu_axi.arid, bfm_nmu_axi.araddr, bfm_nmu_axi.arlen);
        if (ARESETn && bfm_nmu_axi.rvalid && bfm_nmu_axi.rready)
            $display("[%0t] DBG BFM R: RID=0x%0h RLAST=%b RRESP=%0d",
                     $time, bfm_nmu_axi.rid, bfm_nmu_axi.rlast, bfm_nmu_axi.rresp);
        if (ARESETn && nsu_mem_axi.arvalid && nsu_mem_axi.arready)
            $display("[%0t] DBG NSU AR handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, nsu_mem_axi.arid, nsu_mem_axi.araddr, nsu_mem_axi.arlen);
        if (ARESETn && nsu_mem_axi.rvalid && nsu_mem_axi.rready)
            $display("[%0t] DBG NSU R: RID=0x%0h RLAST=%b RRESP=%0d",
                     $time, nsu_mem_axi.rid, nsu_mem_axi.rlast, nsu_mem_axi.rresp);
    end
`endif // GENAMBA_DBG_AXI

endmodule
