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

    // ---- AXI + NoC bundles as packed structs (interfaces removed in Task 4) ----
    // bfm_nmu : tester (master) -> NMU (slave). req = tester-driven, rsp = NMU-driven.
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

    // ---- gen_amba axi_tester (upstream sequence) ↔ bfm_nmu_{req,rsp} ----
    // tester is the master: req fields (AW/W/AR + ready) are tester-driven,
    // rsp fields (awready/wready/arready + B/R) are NMU-driven. AWREGION/
    // ARREGION are driven by the tester (it inits them to 0); REGION is not
    // marshalled by DPI, so the values stop at the struct.
    axi_tester #(
        .P_MST_ID(0), .P_NUM_MST(1), .P_NUM_SLV(1),
        .WIDTH_CID(1), .WIDTH_ID(8), .WIDTH_AD(64), .WIDTH_DA(256),
        .EN(1), .P_SIZE_IN_BYTES(1024)
    ) u_tester (
        .ARESETn(ARESETn), .ACLK(ACLK),
        .AWID(bfm_nmu_req.awid), .AWADDR(bfm_nmu_req.awaddr),
        .AWLEN(bfm_nmu_req.awlen), .AWLOCK(bfm_nmu_req.awlock),
        .AWSIZE(bfm_nmu_req.awsize), .AWBURST(bfm_nmu_req.awburst),
        .AWCACHE(bfm_nmu_req.awcache), .AWPROT(bfm_nmu_req.awprot),
        .AWVALID(bfm_nmu_req.awvalid), .AWREADY(bfm_nmu_rsp.awready),
        .AWQOS(bfm_nmu_req.awqos), .AWREGION(bfm_nmu_req.awregion),
        .WDATA(bfm_nmu_req.wdata), .WSTRB(bfm_nmu_req.wstrb),
        .WLAST(bfm_nmu_req.wlast),
        .WVALID(bfm_nmu_req.wvalid), .WREADY(bfm_nmu_rsp.wready),
        .BID(bfm_nmu_rsp.bid), .BRESP(bfm_nmu_rsp.bresp),
        .BVALID(bfm_nmu_rsp.bvalid), .BREADY(bfm_nmu_req.bready),
        .ARID(bfm_nmu_req.arid), .ARADDR(bfm_nmu_req.araddr),
        .ARLEN(bfm_nmu_req.arlen), .ARLOCK(bfm_nmu_req.arlock),
        .ARSIZE(bfm_nmu_req.arsize), .ARBURST(bfm_nmu_req.arburst),
        .ARCACHE(bfm_nmu_req.arcache), .ARPROT(bfm_nmu_req.arprot),
        .ARVALID(bfm_nmu_req.arvalid), .ARREADY(bfm_nmu_rsp.arready),
        .ARQOS(bfm_nmu_req.arqos), .ARREGION(bfm_nmu_req.arregion),
        .RID(bfm_nmu_rsp.rid), .RDATA(bfm_nmu_rsp.rdata),
        .RRESP(bfm_nmu_rsp.rresp), .RLAST(bfm_nmu_rsp.rlast),
        .RVALID(bfm_nmu_rsp.rvalid), .RREADY(bfm_nmu_req.rready),
        .CSYSREQ(1'b1), .CSYSACK(), .CACTIVE(),
        .busy_out(), .busy_in(1'b0)
    );

    // ---- DUT bridge: NMU + NSU connected via packed-struct NoC links ----
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
        if (ARESETn && !u_tester.done && ($time - last_event_time) > 5000) begin
            $display("[%0t] WATCHDOG: 5us without AXI handshake progress (stage=%0d)",
                     $time, u_tester.stage);
            $fatal(1, "WATCHDOG: AXI silence");
        end
    end

endmodule
