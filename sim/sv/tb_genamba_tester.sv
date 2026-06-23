// tb_genamba_tester — pure-referee top: gen_amba's own axi_tester drives
// the NMU/NSU bridge with its upstream test sequence (selected via the
// upstream plusargs SINGLE_TEST / BURST_TEST / BURST_RANDOM_TEST /
// BURST_MISALIGNED_TEST / SINGLE_TEST_MEM / BURST_TEST_MEM /
// MULTIPLE_OUTSTANDING). No project-authored patterns: stimulus, data,
// and compares are all upstream code. The project side provides only the
// bridge, the DPI lifecycle, and the failure plumbing (error_flag check,
// DPI error pump, handshake watchdog).
`timescale 1ns/1ps

module tb_genamba_tester;
    reg ACLK = 0;
    always #5 ACLK = ~ACLK;          // 10 ns period
    reg ARESETn = 0;

    // ---- DPI imports + longint unsigned storage (same lifecycle as tb_genamba) ----
    import "DPI-C" context function void    cmodel_init(input string path);
    import "DPI-C" context function void    cmodel_finalize();
    import "DPI-C" context function longint unsigned cmodel_channel_model_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_master_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_slave_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_nmu_create(input string name);
    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name);
    longint unsigned cm_ctx, master_ctx, slave_ctx, nmu_ctx, nsu_ctx;

    // ---- AXI + NoC interface bundles ----
    axi4_intf #(.ID_WIDTH(8), .ADDR_WIDTH(64), .DATA_WIDTH(256)) bfm_nmu_axi();
    axi4_intf #(.ID_WIDTH(8), .ADDR_WIDTH(64), .DATA_WIDTH(256)) nsu_mem_axi();
    noc_intf  #(.NUM_VC(1), .FLIT_WIDTH(408)) noc_link();

    // ---- gen_amba axi_tester (upstream sequence) ↔ bfm_nmu_axi ----
    // AWREGION/ARREGION are driven by the tester (it inits them to 0);
    // REGION is not marshalled by DPI, so the values stop at the interface.
    axi_tester #(
        .P_MST_ID(0), .P_NUM_MST(1), .P_NUM_SLV(1),
        .WIDTH_CID(1), .WIDTH_ID(8), .WIDTH_AD(64), .WIDTH_DA(256),
        .EN(1), .P_SIZE_IN_BYTES(1024)
    ) u_tester (
        .ARESETn(ARESETn), .ACLK(ACLK),
        .AWID(bfm_nmu_axi.awid), .AWADDR(bfm_nmu_axi.awaddr),
        .AWLEN(bfm_nmu_axi.awlen), .AWLOCK(bfm_nmu_axi.awlock),
        .AWSIZE(bfm_nmu_axi.awsize), .AWBURST(bfm_nmu_axi.awburst),
        .AWCACHE(bfm_nmu_axi.awcache), .AWPROT(bfm_nmu_axi.awprot),
        .AWVALID(bfm_nmu_axi.awvalid), .AWREADY(bfm_nmu_axi.awready),
        .AWQOS(bfm_nmu_axi.awqos), .AWREGION(bfm_nmu_axi.awregion),
        .WDATA(bfm_nmu_axi.wdata), .WSTRB(bfm_nmu_axi.wstrb),
        .WLAST(bfm_nmu_axi.wlast),
        .WVALID(bfm_nmu_axi.wvalid), .WREADY(bfm_nmu_axi.wready),
        .BID(bfm_nmu_axi.bid), .BRESP(bfm_nmu_axi.bresp),
        .BVALID(bfm_nmu_axi.bvalid), .BREADY(bfm_nmu_axi.bready),
        .ARID(bfm_nmu_axi.arid), .ARADDR(bfm_nmu_axi.araddr),
        .ARLEN(bfm_nmu_axi.arlen), .ARLOCK(bfm_nmu_axi.arlock),
        .ARSIZE(bfm_nmu_axi.arsize), .ARBURST(bfm_nmu_axi.arburst),
        .ARCACHE(bfm_nmu_axi.arcache), .ARPROT(bfm_nmu_axi.arprot),
        .ARVALID(bfm_nmu_axi.arvalid), .ARREADY(bfm_nmu_axi.arready),
        .ARQOS(bfm_nmu_axi.arqos), .ARREGION(bfm_nmu_axi.arregion),
        .RID(bfm_nmu_axi.rid), .RDATA(bfm_nmu_axi.rdata),
        .RRESP(bfm_nmu_axi.rresp), .RLAST(bfm_nmu_axi.rlast),
        .RVALID(bfm_nmu_axi.rvalid), .RREADY(bfm_nmu_axi.rready),
        .CSYSREQ(1'b1), .CSYSACK(), .CACTIVE(),
        .busy_out(), .busy_in(1'b0)
    );

    // ---- DUT bridge: NMU + NSU on one noc_intf (same as tb_genamba) ----
    nmu_wrap u_nmu (
        .clk_i(ACLK), .rst_ni(ARESETn),
        .ctx_i(nmu_ctx),
        .axi_i(bfm_nmu_axi.slave),
        .noc_mosi_o(noc_link.mosi)
    );
    nsu_wrap u_nsu (
        .clk_i(ACLK), .rst_ni(ARESETn),
        .ctx_i(nsu_ctx),
        .noc_miso_i(noc_link.miso),
        .axi_o(nsu_mem_axi.master)
    );

    /* verilator lint_off WAITCONST */
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
    /* verilator lint_on WAITCONST */

    initial begin
        string scenario_path;
        if (!$value$plusargs("scenario=%s", scenario_path))
            scenario_path = "../genamba_init.yaml";  // fallback for direct exe runs from cosim/verilator
        cmodel_init(scenario_path);
        cm_ctx     = cmodel_channel_model_create("channel_model_0");
        master_ctx = cmodel_master_create("master_0");
        slave_ctx  = cmodel_slave_create("slave_0");
        nmu_ctx    = cmodel_nmu_create("nmu_0");
        nsu_ctx    = cmodel_nsu_create("nsu_0");

        repeat (4) @(posedge ACLK);
        ARESETn = 1'b1;

        // The tester's own initial block runs the upstream sequence and
        // raises `done` at the end. error_flag is upstream's verdict.
        wait (u_tester.done == 1'b1);
        repeat (10) @(posedge ACLK);
        if (u_tester.error_flag)
            $fatal(1, "tb_genamba_tester: upstream sequence FAILED (error_flag set)");
        $display("[%0t] tb_genamba_tester: upstream sequence PASS", $time);
        $finish;
    end

    final begin
        cmodel_finalize();
    end

    // ---- DPI per-cycle error pump (same as tb_genamba) ----
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

    // ---- AXI progress watchdog (same semantics as tb_genamba) ----
    time last_event_time = 0;
    always @(posedge ACLK) begin
        if ((bfm_nmu_axi.awvalid && bfm_nmu_axi.awready) ||
            (bfm_nmu_axi.wvalid  && bfm_nmu_axi.wready)  ||
            (bfm_nmu_axi.bvalid  && bfm_nmu_axi.bready)  ||
            (bfm_nmu_axi.arvalid && bfm_nmu_axi.arready) ||
            (bfm_nmu_axi.rvalid  && bfm_nmu_axi.rready)  ||
            (nsu_mem_axi.awvalid && nsu_mem_axi.awready) ||
            (nsu_mem_axi.wvalid  && nsu_mem_axi.wready)  ||
            (nsu_mem_axi.bvalid  && nsu_mem_axi.bready)  ||
            (nsu_mem_axi.arvalid && nsu_mem_axi.arready) ||
            (nsu_mem_axi.rvalid  && nsu_mem_axi.rready))
            last_event_time = $time;
        if (ARESETn && !u_tester.done && ($time - last_event_time) > 5000) begin
            $display("[%0t] WATCHDOG: 5us without AXI handshake progress (stage=%0d)",
                     $time, u_tester.stage);
            $fatal(1, "WATCHDOG: AXI silence");
        end
    end

endmodule
