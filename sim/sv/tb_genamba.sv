// gen_amba role-1 testbench top — BFM->NMU->NoC->NSU->mem_axi bridge
`timescale 1ns/1ps

module tb_genamba;
    reg ACLK = 0;
    always #5 ACLK = ~ACLK;          // 10 ns period
    reg ARESETn = 0;

    // ---- DPI imports + longint unsigned storage (copy of tb_top.sv:47-63 pattern) ----
    import "DPI-C" context function void    cmodel_init(input string path);
    import "DPI-C" context function void    cmodel_finalize();
    import "DPI-C" context function longint unsigned cmodel_channel_model_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_master_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_slave_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_nmu_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name);
    longint unsigned cm_ctx, master_ctx, slave_ctx, nmu_ctx, nsu_ctx;

    // ---- AXI + NoC bundles as packed structs (interfaces removed in Task 4) ----
    // bfm_nmu : BFM (master) -> NMU (slave). req = master-driven, rsp = NMU-driven.
    ni_signals_pkg::axi_req_t   bfm_nmu_req;
    ni_signals_pkg::axi_rsp_t   bfm_nmu_rsp;
    // nsu_mem : NSU (master) -> mem_axi (slave). req = NSU-driven, rsp = mem-driven.
    ni_signals_pkg::axi_req_t   nsu_mem_req;
    ni_signals_pkg::axi_rsp_t   nsu_mem_rsp;
    // NoC link as packed structs (noc_intf removed in Task 3 wrap refactor).
    ni_signals_pkg::noc_chan_t  noc_req_link;   // NMU req_o → NSU req_i
    noc_types_pkg::noc_credit_t noc_req_cred_link; // NSU req_cred_o → NMU req_cred_i
    ni_signals_pkg::noc_chan_t  noc_rsp_link;   // NSU rsp_o → NMU rsp_i
    noc_types_pkg::noc_credit_t noc_rsp_cred_link; // NMU rsp_cred_o → NSU rsp_cred_i

    // ---- BFM <-> bfm_nmu_{req,rsp} (BFM uppercase port -> struct field) ----
    // awregion/arregion carried-but-unused (not a BFM port); tie to 0.
    assign bfm_nmu_req.awregion = 4'b0;
    assign bfm_nmu_req.arregion = 4'b0;
    genamba_master_bfm #(.WIDTH_AD(64), .WIDTH_DA(256), .WIDTH_ID(8), .P_MST_ID(0)) u_bfm (
        .ACLK(ACLK), .ARESETn(ARESETn),
        .AWID(bfm_nmu_req.awid), .AWADDR(bfm_nmu_req.awaddr),
        .AWLEN(bfm_nmu_req.awlen), .AWSIZE(bfm_nmu_req.awsize),
        .AWBURST(bfm_nmu_req.awburst), .AWLOCK(bfm_nmu_req.awlock),
        .AWCACHE(bfm_nmu_req.awcache), .AWPROT(bfm_nmu_req.awprot),
        .AWQOS(bfm_nmu_req.awqos),
        .AWVALID(bfm_nmu_req.awvalid), .AWREADY(bfm_nmu_rsp.awready),
        .WDATA(bfm_nmu_req.wdata), .WSTRB(bfm_nmu_req.wstrb),
        .WLAST(bfm_nmu_req.wlast),
        .WVALID(bfm_nmu_req.wvalid), .WREADY(bfm_nmu_rsp.wready),
        .BID(bfm_nmu_rsp.bid), .BRESP(bfm_nmu_rsp.bresp),
        .BVALID(bfm_nmu_rsp.bvalid), .BREADY(bfm_nmu_req.bready),
        .ARID(bfm_nmu_req.arid), .ARADDR(bfm_nmu_req.araddr),
        .ARLEN(bfm_nmu_req.arlen), .ARSIZE(bfm_nmu_req.arsize),
        .ARBURST(bfm_nmu_req.arburst), .ARLOCK(bfm_nmu_req.arlock),
        .ARCACHE(bfm_nmu_req.arcache), .ARPROT(bfm_nmu_req.arprot),
        .ARQOS(bfm_nmu_req.arqos),
        .ARVALID(bfm_nmu_req.arvalid), .ARREADY(bfm_nmu_rsp.arready),
        .RID(bfm_nmu_rsp.rid), .RDATA(bfm_nmu_rsp.rdata),
        .RRESP(bfm_nmu_rsp.rresp), .RLAST(bfm_nmu_rsp.rlast),
        .RVALID(bfm_nmu_rsp.rvalid), .RREADY(bfm_nmu_req.rready)
    );

    // ---- DUT bridge: NMU + NSU connected via packed-struct NoC links ----
    // ctx_i: longint unsigned context for per-instance DPI tick (NMU/NSU wrap each
    // own one); verified against tb_top.sv pattern — mandatory port.
    nmu_wrap u_nmu (
        .clk_i(ACLK), .rst_ni(ARESETn),
        .ctx_i(nmu_ctx),
        .axi_req_i(bfm_nmu_req), .axi_rsp_o(bfm_nmu_rsp),
        .noc_req_o(noc_req_link),
        .noc_req_cred_i(noc_req_cred_link),
        .noc_rsp_i(noc_rsp_link),
        .noc_rsp_cred_o(noc_rsp_cred_link)
    );
    nsu_wrap u_nsu (
        .clk_i(ACLK), .rst_ni(ARESETn),
        .ctx_i(nsu_ctx),
        .noc_req_i(noc_req_link),
        .noc_req_cred_o(noc_req_cred_link),
        .noc_rsp_o(noc_rsp_link),
        .noc_rsp_cred_i(noc_rsp_cred_link),
        .axi_req_o(nsu_mem_req), .axi_rsp_i(nsu_mem_rsp)
    );

    // ---- nsu_mem_{req,rsp} <-> mem_axi (struct field -> UPPERCASE port) ----
    // WAITCONST: vendored mem_axi has a low-power handshake (`wait
    // (CSYSREQ==1'b0)`) that this testbench never exercises — CSYSREQ tied
    // high makes the wait condition constant by design.
    /* verilator lint_off WAITCONST */
    mem_axi #(
        .AXI_WIDTH_CID(0), .AXI_WIDTH_ID(8), .AXI_WIDTH_AD(64),
        .AXI_WIDTH_DA(256), .SIZE_IN_BYTES(16384)
    ) u_mem (
        .ARESETn(ARESETn), .ACLK(ACLK), .CSYSREQ(1'b1), .CSYSACK(), .CACTIVE(),
        .AWID(nsu_mem_req.awid), .AWADDR(nsu_mem_req.awaddr),
        .AWLEN(nsu_mem_req.awlen), .AWSIZE(nsu_mem_req.awsize),
        .AWBURST(nsu_mem_req.awburst), .AWLOCK(nsu_mem_req.awlock),
        .AWCACHE(nsu_mem_req.awcache), .AWPROT(nsu_mem_req.awprot),
        .AWQOS(nsu_mem_req.awqos), .AWREGION(4'b0),
        .AWVALID(nsu_mem_req.awvalid), .AWREADY(nsu_mem_rsp.awready),
        .WDATA(nsu_mem_req.wdata), .WSTRB(nsu_mem_req.wstrb),
        .WLAST(nsu_mem_req.wlast),
        .WVALID(nsu_mem_req.wvalid), .WREADY(nsu_mem_rsp.wready),
        .BID(nsu_mem_rsp.bid), .BRESP(nsu_mem_rsp.bresp),
        .BVALID(nsu_mem_rsp.bvalid), .BREADY(nsu_mem_req.bready),
        .ARID(nsu_mem_req.arid), .ARADDR(nsu_mem_req.araddr),
        .ARLEN(nsu_mem_req.arlen), .ARSIZE(nsu_mem_req.arsize),
        .ARBURST(nsu_mem_req.arburst), .ARLOCK(nsu_mem_req.arlock),
        .ARCACHE(nsu_mem_req.arcache), .ARPROT(nsu_mem_req.arprot),
        .ARQOS(nsu_mem_req.arqos), .ARREGION(4'b0),
        .ARVALID(nsu_mem_req.arvalid), .ARREADY(nsu_mem_rsp.arready),
        .RID(nsu_mem_rsp.rid), .RDATA(nsu_mem_rsp.rdata),
        .RRESP(nsu_mem_rsp.rresp), .RLAST(nsu_mem_rsp.rlast),
        .RVALID(nsu_mem_rsp.rvalid), .RREADY(nsu_mem_req.rready)
    );
    /* verilator lint_on WAITCONST */

    initial begin
        // DPI lifecycle (BEFORE reset deassert, per tb_top.sv:70-76 pattern)
        string scenario_path;
        if (!$value$plusargs("scenario=%s", scenario_path))
            scenario_path = "../genamba_init.yaml";  // fallback for direct exe runs from sim/verilator
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

        u_bfm.test_outstanding_N(4);
        u_bfm.test_outstanding_N(8);

        u_bfm.test_outstanding_burst_N4(4);
        u_bfm.test_outstanding_burst_N4(8);

        u_bfm.test_same_id_outstanding;

        u_bfm.test_mixed_rw_concurrent;

        u_bfm.test_deep_outstanding_pressure(8);
        u_bfm.test_deep_outstanding_pressure(16);

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

    // ---- AXI progress watchdog (project code, not vendored) ----
    // Kill sim if no AXI HANDSHAKE completes for 1us — turns a silent hang
    // into a finite failure log. Two deliberate choices:
    // - `time` (64-bit), not `integer`: a 32-bit signed copy of $time
    //   overflows at ~2.1 ms sim time and corrupts the silence computation
    //   (observed once as a watchdog firing with a garbage timestamp).
    // - Progress = completed handshake (valid && ready), not valid alone:
    //   a stuck transfer (VALID held high, READY never coming) must trip
    //   the watchdog, but valid-as-activity would keep resetting it.
    time last_event_time = 0;
    always @(posedge ACLK) begin
        if ((bfm_nmu_req.awvalid && bfm_nmu_rsp.awready) ||
            (bfm_nmu_req.wvalid  && bfm_nmu_rsp.wready)  ||
            (bfm_nmu_rsp.bvalid  && bfm_nmu_req.bready)  ||
            (bfm_nmu_req.arvalid && bfm_nmu_rsp.arready) ||
            (bfm_nmu_rsp.rvalid  && bfm_nmu_req.rready)  ||
            (nsu_mem_req.awvalid && nsu_mem_rsp.awready) ||
            (nsu_mem_req.wvalid  && nsu_mem_rsp.wready)  ||
            (nsu_mem_rsp.bvalid  && nsu_mem_req.bready)  ||
            (nsu_mem_req.arvalid && nsu_mem_rsp.arready) ||
            (nsu_mem_rsp.rvalid  && nsu_mem_req.rready))
            last_event_time = $time;
        if (ARESETn && ($time - last_event_time) > 1000) begin
            $display("[%0t] WATCHDOG: 1us without AXI handshake progress, dumping final state", $time);
            $display("  BFM-side : AWV=%b AWR=%b AWID=0x%0h AWADDR=0x%0h AWLEN=%0d",
                     bfm_nmu_req.awvalid, bfm_nmu_rsp.awready,
                     bfm_nmu_req.awid, bfm_nmu_req.awaddr, bfm_nmu_req.awlen);
            $display("  BFM-side : WV=%b WR=%b WLAST=%b WSTRB=0x%0h",
                     bfm_nmu_req.wvalid, bfm_nmu_rsp.wready,
                     bfm_nmu_req.wlast, bfm_nmu_req.wstrb);
            $display("  BFM-side : BV=%b BR=%b BID=0x%0h BRESP=%0d",
                     bfm_nmu_rsp.bvalid, bfm_nmu_req.bready,
                     bfm_nmu_rsp.bid, bfm_nmu_rsp.bresp);
            $display("  NSU-side : AWV=%b AWR=%b AWID=0x%0h AWLEN=%0d",
                     nsu_mem_req.awvalid, nsu_mem_rsp.awready,
                     nsu_mem_req.awid, nsu_mem_req.awlen);
            $display("  NSU-side : WV=%b WR=%b WLAST=%b",
                     nsu_mem_req.wvalid, nsu_mem_rsp.wready, nsu_mem_req.wlast);
            $display("  NSU-side : BV=%b BR=%b BID=0x%0h",
                     nsu_mem_rsp.bvalid, nsu_mem_req.bready, nsu_mem_rsp.bid);
            $fatal(1, "WATCHDOG: AXI silence");
        end
    end

    // Per-cycle AXI handshake monitors — first 30 W beats only to bound log.
    // Opt-in via `+define+GENAMBA_DBG_AXI` (default off — runs stay quiet).
`ifdef GENAMBA_DBG_AXI
    integer w_beat_count = 0;
    always @(posedge ACLK) begin
        if (ARESETn && bfm_nmu_req.awvalid && bfm_nmu_rsp.awready)
            $display("[%0t] DBG BFM AW handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, bfm_nmu_req.awid, bfm_nmu_req.awaddr, bfm_nmu_req.awlen);
        if (ARESETn && bfm_nmu_req.wvalid && bfm_nmu_rsp.wready && w_beat_count < 30) begin
            $display("[%0t] DBG BFM W#%0d: WLAST=%b WSTRB=0x%0h DATA[31:0]=0x%0h",
                     $time, w_beat_count, bfm_nmu_req.wlast,
                     bfm_nmu_req.wstrb[31:0], bfm_nmu_req.wdata[31:0]);
            w_beat_count = w_beat_count + 1;
        end
        if (ARESETn && bfm_nmu_rsp.bvalid && bfm_nmu_req.bready)
            $display("[%0t] DBG BFM B handshake: BID=0x%0h BRESP=%0d",
                     $time, bfm_nmu_rsp.bid, bfm_nmu_rsp.bresp);
        if (ARESETn && nsu_mem_req.awvalid && nsu_mem_rsp.awready)
            $display("[%0t] DBG NSU AW handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, nsu_mem_req.awid, nsu_mem_req.awaddr, nsu_mem_req.awlen);
        if (ARESETn && nsu_mem_req.wvalid && nsu_mem_rsp.wready)
            $display("[%0t] DBG NSU W: WLAST=%b WSTRB=0x%0h",
                     $time, nsu_mem_req.wlast, nsu_mem_req.wstrb[31:0]);
        if (ARESETn && nsu_mem_rsp.bvalid && nsu_mem_req.bready)
            $display("[%0t] DBG NSU B: BID=0x%0h BRESP=%0d",
                     $time, nsu_mem_rsp.bid, nsu_mem_rsp.bresp);
        if (ARESETn && bfm_nmu_req.arvalid && bfm_nmu_rsp.arready)
            $display("[%0t] DBG BFM AR handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, bfm_nmu_req.arid, bfm_nmu_req.araddr, bfm_nmu_req.arlen);
        if (ARESETn && bfm_nmu_rsp.rvalid && bfm_nmu_req.rready)
            $display("[%0t] DBG BFM R: RID=0x%0h RLAST=%b RRESP=%0d",
                     $time, bfm_nmu_rsp.rid, bfm_nmu_rsp.rlast, bfm_nmu_rsp.rresp);
        if (ARESETn && nsu_mem_req.arvalid && nsu_mem_rsp.arready)
            $display("[%0t] DBG NSU AR handshake: ID=0x%0h ADDR=0x%0h LEN=%0d",
                     $time, nsu_mem_req.arid, nsu_mem_req.araddr, nsu_mem_req.arlen);
        if (ARESETn && nsu_mem_rsp.rvalid && nsu_mem_req.rready)
            $display("[%0t] DBG NSU R: RID=0x%0h RLAST=%b RRESP=%0d",
                     $time, nsu_mem_rsp.rid, nsu_mem_rsp.rlast, nsu_mem_rsp.rresp);
    end
`endif // GENAMBA_DBG_AXI

    // FSDB waveform dump — compiled in only by the VCS flow with FSDB=1
    // (+define+FSDB_DUMP). Path comes from +fsdb=<abs-path>; the Makefile
    // run recipe supplies output/genamba_<scenario>/tb_genamba.fsdb.
`ifdef FSDB_DUMP
    initial begin
        string fsdb_path;
        if (!$value$plusargs("fsdb=%s", fsdb_path))
            fsdb_path = "tb_genamba.fsdb";
        $fsdbDumpfile(fsdb_path);
        $fsdbDumpvars(0, tb_genamba); // depth 0 = full hierarchy below top
    end
`endif

endmodule
