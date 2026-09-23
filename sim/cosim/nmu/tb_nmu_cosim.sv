`timescale 1ns / 1ps
module tb_nmu_cosim;
    import ni_params_pkg::*;
    localparam int NUM_PORTS = 5;
    localparam int NMU_PORT = 0;
    localparam int NSU_PORT = 4;
    bit corrupt_rsp = 0;
    logic clk = 0, rst_n = 0;
    wire axi_rst_n, noc_rst_n;
    always #5 clk = ~clk;
    cc_rstgen_bypass #(.NumRegs(2)) i_axi_reset_sync (
        .clk_i            (clk),
        .rst_ni           (rst_n),
        .rst_test_mode_ni (rst_n),
        .test_mode_i      (1'b0),
        .rst_no           (axi_rst_n),
        .init_no          ()
    );
    cc_rstgen_bypass #(.NumRegs(2)) i_noc_reset_sync (
        .clk_i            (clk),
        .rst_ni           (rst_n),
        .rst_test_mode_ni (rst_n),
        .test_mode_i      (1'b0),
        .rst_no           (noc_rst_n),
        .init_no          ()
    );
    AXI_BUS_DV #(.AXI_ADDR_WIDTH(AXI_ADDR_WIDTH), .AXI_DATA_WIDTH(AXI_DATA_WIDTH),
        .AXI_ID_WIDTH   (AXI_ID_WIDTH),
        .AXI_USER_WIDTH (AXI_AWUSER_WIDTH)) vip(clk);
    axi_if #(.ADDR_W(AXI_ADDR_WIDTH), .DATA_W(AXI_DATA_WIDTH),
        .ID_W     (AXI_ID_WIDTH),
        .AWUSER_W (AXI_AWUSER_WIDTH)) bus();
    AXI_BUS #(.AXI_ADDR_WIDTH(AXI_ADDR_WIDTH), .AXI_DATA_WIDTH(AXI_DATA_WIDTH),
        .AXI_ID_WIDTH   (NSU_AXI_ID_WIDTH),
        .AXI_USER_WIDTH (AXI_AWUSER_WIDTH)) mem_bus();
    ni_signals_pkg::axi_req_t mem_req;
    ni_signals_pkg::axi_rsp_t mem_rsp;
    longint unsigned router_ctx, nsu_ctx;
    wire [NUM_PORTS-1:0] tx_req_valid, rx_req_valid;
    wire [NOC_REQ_FLIT_WIDTH-1:0] tx_req_flit [NUM_PORTS], rx_req_flit [NUM_PORTS];
    wire [NUM_PORTS-1:0] tx_rsp_valid, rx_rsp_valid;
    wire [NOC_RSP_FLIT_WIDTH-1:0] tx_rsp_flit [NUM_PORTS], rx_rsp_flit [NUM_PORTS];
    wire [NUM_PORTS-1:0] tx_dat_valid, rx_dat_valid;
    wire [NOC_DAT_FLIT_WIDTH-1:0] tx_dat_flit [NUM_PORTS], rx_dat_flit [NUM_PORTS];
    wire [NUM_PORTS-1:0] tx_req_ready, rx_req_ready, tx_rsp_ready, rx_rsp_ready;
    wire [NUM_DAT_VC-1:0] tx_dat_credit [NUM_PORTS], rx_dat_credit [NUM_PORTS];
    assign bus.awid     = vip.aw_id;
    assign bus.awaddr   = vip.aw_addr;
    assign bus.awlen    = vip.aw_len;
    assign bus.awsize   = vip.aw_size;
    assign bus.awburst  = vip.aw_burst;
    assign bus.awlock   = vip.aw_lock;
    assign bus.awcache  = vip.aw_cache;
    assign bus.awprot   = vip.aw_prot;
    assign bus.awqos    = vip.aw_qos;
    assign bus.awregion = vip.aw_region;
    assign bus.awuser   = vip.aw_user;
    assign bus.awvalid  = vip.aw_valid;
    assign vip.aw_ready = bus.awready;
    assign bus.wdata    = vip.w_data;
    assign bus.wstrb    = vip.w_strb;
    assign bus.wlast    = vip.w_last;
    assign bus.wvalid   = vip.w_valid;
    assign vip.w_ready  = bus.wready;
    assign bus.arid     = vip.ar_id;
    assign bus.araddr   = vip.ar_addr;
    assign bus.arlen    = vip.ar_len;
    assign bus.arsize   = vip.ar_size;
    assign bus.arburst  = vip.ar_burst;
    assign bus.arlock   = vip.ar_lock;
    assign bus.arcache  = vip.ar_cache;
    assign bus.arprot   = vip.ar_prot;
    assign bus.arqos    = vip.ar_qos;
    assign bus.arregion = vip.ar_region;
    assign bus.arvalid  = vip.ar_valid;
    assign vip.ar_ready = bus.arready;
    assign bus.wuser    = '0;
    assign bus.aruser   = '0;
    assign bus.bready   = vip.b_ready;
    assign vip.b_valid  = bus.bvalid;
    assign vip.b_id     = bus.bid;
    assign vip.b_resp   = bus.bresp;
    assign vip.b_user   = '0;
    assign bus.rready   = vip.r_ready;
    assign vip.r_valid  = bus.rvalid;
    assign vip.r_id     = bus.rid;
    assign vip.r_data   = bus.rdata ^ (corrupt_rsp ? AXI_DATA_WIDTH'(1) : '0);
    assign vip.r_resp   = bus.rresp;
    assign vip.r_last   = bus.rlast;
    assign vip.r_user   = '0;
    nmu dut (
        .ACLK              (clk),
        .ARESETn           (axi_rst_n),
        .noc_clk           (clk),
        .noc_rst_n         (noc_rst_n),
        .axi_wr_i          (bus),
        .axi_rd_i          (bus),
        .tx_req_valid_o    (rx_req_valid[NMU_PORT]),
        .tx_req_flit_o     (rx_req_flit[NMU_PORT]),
        .tx_req_ready_i    (rx_req_ready[NMU_PORT]),
        .rx_rsp_valid_i    (tx_rsp_valid[NMU_PORT]),
        .rx_rsp_flit_i     (tx_rsp_flit[NMU_PORT]),
        .rx_rsp_ready_o    (tx_rsp_ready[NMU_PORT]),
        .tx_dat_valid_o    (rx_dat_valid[NMU_PORT]),
        .tx_dat_flit_o     (rx_dat_flit[NMU_PORT]),
        .tx_dat_crdvalid_i (rx_dat_credit[NMU_PORT]),
        .rx_dat_valid_i    (tx_dat_valid[NMU_PORT]),
        .rx_dat_flit_i     (tx_dat_flit[NMU_PORT]),
        .rx_dat_crdvalid_o (tx_dat_credit[NMU_PORT])
    );
    router_wrap i_router (
        .clk_i           (clk),
        .rst_n_i         (noc_rst_n),
        .ctx_i           (router_ctx),
        .tx_req_valid    (tx_req_valid),
        .tx_req_flit     (tx_req_flit),
        .tx_req_ready    (tx_req_ready),
        .rx_req_valid    (rx_req_valid),
        .rx_req_flit     (rx_req_flit),
        .rx_req_ready    (rx_req_ready),
        .tx_rsp_valid    (tx_rsp_valid),
        .tx_rsp_flit     (tx_rsp_flit),
        .tx_rsp_ready    (tx_rsp_ready),
        .rx_rsp_valid    (rx_rsp_valid),
        .rx_rsp_flit     (rx_rsp_flit),
        .rx_rsp_ready    (rx_rsp_ready),
        .tx_dat_valid    (tx_dat_valid),
        .tx_dat_flit     (tx_dat_flit),
        .tx_dat_crdvalid (tx_dat_credit),
        .rx_dat_valid    (rx_dat_valid),
        .rx_dat_flit     (rx_dat_flit),
        .rx_dat_crdvalid (rx_dat_credit)
    );
    nsu_wrap i_nsu (
        .clk_i             (clk),
        .rst_n_i           (noc_rst_n),
        .ctx_i             (nsu_ctx),
        .rx_req_valid_i    (tx_req_valid[NSU_PORT]),
        .rx_req_flit_i     (tx_req_flit[NSU_PORT]),
        .rx_req_ready_o    (tx_req_ready[NSU_PORT]),
        .tx_rsp_valid_o    (rx_rsp_valid[NSU_PORT]),
        .tx_rsp_flit_o     (rx_rsp_flit[NSU_PORT]),
        .tx_rsp_ready_i    (rx_rsp_ready[NSU_PORT]),
        .tx_dat_valid_o    (rx_dat_valid[NSU_PORT]),
        .tx_dat_flit_o     (rx_dat_flit[NSU_PORT]),
        .tx_dat_crdvalid_i (rx_dat_credit[NSU_PORT]),
        .rx_dat_valid_i    (tx_dat_valid[NSU_PORT]),
        .rx_dat_flit_i     (tx_dat_flit[NSU_PORT]),
        .rx_dat_crdvalid_o (tx_dat_credit[NSU_PORT]),
        .axi_req_o         (mem_req),
        .axi_rsp_i         (mem_rsp)
    );
    for (genvar port = 0; port < NUM_PORTS; port++) begin : gen_tieoff
        if (port != NMU_PORT) begin
            assign rx_req_valid[port] = 1'b0;
            assign rx_req_flit[port] = '0;
            assign tx_rsp_ready[port] = 1'b0;
        end
        if (port != NSU_PORT) begin
            assign rx_rsp_valid[port] = 1'b0;
            assign rx_rsp_flit[port] = '0;
            assign tx_req_ready[port] = 1'b0;
        end
        if (port != NMU_PORT && port != NSU_PORT) begin
            assign rx_dat_valid[port] = 1'b0;
            assign rx_dat_flit[port] = '0;
            assign tx_dat_credit[port] = '0;
        end
        always @(posedge clk) begin
            if (noc_rst_n && ((port != NSU_PORT && tx_req_valid[port]) ||
                (port != NMU_PORT && tx_rsp_valid[port]) ||
                (port != NMU_PORT && port != NSU_PORT && tx_dat_valid[port])))
                $fatal(1, "Unexpected Router egress port %0d", port);
        end
    end
    assign mem_bus.aw_id = mem_req.awid;
    assign mem_bus.aw_addr = mem_req.awaddr;
    assign mem_bus.aw_len = mem_req.awlen;
    assign mem_bus.aw_size = mem_req.awsize;
    assign mem_bus.aw_burst = mem_req.awburst;
    assign mem_bus.aw_lock = mem_req.awlock;
    assign mem_bus.aw_cache = mem_req.awcache;
    assign mem_bus.aw_prot = mem_req.awprot;
    assign mem_bus.aw_qos = mem_req.awqos;
    assign mem_bus.aw_region = mem_req.awregion;
    assign mem_bus.aw_valid = mem_req.awvalid;
    assign mem_bus.w_data = mem_req.wdata;
    assign mem_bus.w_strb = mem_req.wstrb;
    assign mem_bus.w_last = mem_req.wlast;
    assign mem_bus.w_valid = mem_req.wvalid;
    assign mem_bus.ar_id = mem_req.arid;
    assign mem_bus.ar_addr = mem_req.araddr;
    assign mem_bus.ar_len = mem_req.arlen;
    assign mem_bus.ar_size = mem_req.arsize;
    assign mem_bus.ar_burst = mem_req.arburst;
    assign mem_bus.ar_lock = mem_req.arlock;
    assign mem_bus.ar_cache = mem_req.arcache;
    assign mem_bus.ar_prot = mem_req.arprot;
    assign mem_bus.ar_qos = mem_req.arqos;
    assign mem_bus.ar_region = mem_req.arregion;
    assign mem_bus.ar_valid = mem_req.arvalid;
    assign mem_bus.aw_user = '0;
    assign mem_bus.aw_atop = '0;
    assign mem_bus.w_user = '0;
    assign mem_bus.ar_user = '0;
    assign mem_bus.b_ready = mem_req.bready;
    assign mem_bus.r_ready = mem_req.rready;
    assign mem_rsp.awready = mem_bus.aw_ready;
    assign mem_rsp.wready = mem_bus.w_ready;
    assign mem_rsp.arready = mem_bus.ar_ready;
    assign mem_rsp.bid = mem_bus.b_id;
    assign mem_rsp.bresp = mem_bus.b_resp;
    assign mem_rsp.bvalid = mem_bus.b_valid;
    assign mem_rsp.rid = mem_bus.r_id;
    assign mem_rsp.rdata = mem_bus.r_data;
    assign mem_rsp.rresp = mem_bus.r_resp;
    assign mem_rsp.rlast = mem_bus.r_last;
    assign mem_rsp.rvalid = mem_bus.r_valid;
    axi_sim_mem_intf #(
        .AXI_ADDR_WIDTH     (AXI_ADDR_WIDTH),
        .AXI_DATA_WIDTH     (AXI_DATA_WIDTH),
        .AXI_ID_WIDTH       (NSU_AXI_ID_WIDTH),
        .AXI_USER_WIDTH     (AXI_AWUSER_WIDTH),
        .WARN_UNINITIALIZED (1'b1),
        .UNINITIALIZED_DATA ("undefined"),
        .APPL_DELAY         (1ns),
        .ACQ_DELAY          (2ns)
    ) i_memory (
        .clk_i              (clk),
        .rst_ni             (axi_rst_n),
        .axi_slv            (mem_bus),
        .mon_w_valid_o      (),
        .mon_w_addr_o       (),
        .mon_w_data_o       (),
        .mon_w_id_o         (),
        .mon_w_user_o       (),
        .mon_w_beat_count_o (),
        .mon_w_last_o       (),
        .mon_r_valid_o      (),
        .mon_r_addr_o       (),
        .mon_r_data_o       (),
        .mon_r_id_o         (),
        .mon_r_user_o       (),
        .mon_r_beat_count_o (),
        .mon_r_last_o       ()
    );
    typedef axi_test::axi_file_master #(
        .AW (AXI_ADDR_WIDTH),
        .DW (AXI_DATA_WIDTH),
        .IW (AXI_ID_WIDTH),
        .UW (AXI_AWUSER_WIDTH),
        .TA (1ns),
        .TT (2ns)
    ) master_t;
    typedef axi_test::axi_scoreboard #(
        .AW (AXI_ADDR_WIDTH),
        .DW (AXI_DATA_WIDTH),
        .IW (AXI_ID_WIDTH),
        .UW (AXI_AWUSER_WIDTH),
        .TT (2ns)
    ) scoreboard_t;
    import "DPI-C" context function int cmodel_check_error(output string message);
    always @(negedge clk) begin : check_model_error
        string message;
        if (noc_rst_n && cmodel_check_error(message) != 0)
            $fatal(1, "C++ model error: %s", message);
    end
    int b_count = 0, r_count = 0, r_beats = 0, checked_bytes = 0;
    int expected_writes, expected_reads, expected_beats;
    int live_w[2**AXI_ID_WIDTH] = '{default:0};
    int live_r[2**AXI_ID_WIDTH] = '{default:0};
    int peak_w = 0, peak_r = 0, peak_unique_w = 0, peak_unique_r = 0;
    int min_outstanding = 1, min_unique = 1;
    master_t::ax_beat_t expected_ar[2**AXI_ID_WIDTH][$];
    int read_beat[2**AXI_ID_WIDTH] = '{default:0};
    master_t master;
    scoreboard_t scoreboard;
    import "DPI-C" context function void cmodel_init();
    import "DPI-C" context function void cmodel_finalize();
    import "DPI-C" context function longint unsigned cmodel_router_create(
        input string name, input int x, y, mesh_x, mesh_y, num_vc);
    import "DPI-C" context function longint unsigned cmodel_nsu_create(
        input string name, input int src_id, num_vc, max_ids, max_outstanding,
        port_id, input string config_path);
    import "DPI-C" context function void cmodel_nsu_set_dat_credit_depth(
        input longint unsigned ctx, input int depth);
    initial begin : run
        string stim_dir;
        cmodel_init();
        router_ctx = cmodel_router_create("router", 0, 0, 2, 2, NUM_DAT_VC);
        nsu_ctx = cmodel_nsu_create("nsu", 0, NUM_DAT_VC,
            NSU_META_BUFFER_MAX_UNIQUE_IDS, NSU_META_BUFFER_MAX_OUTSTANDING, 1, "");
        cmodel_nsu_set_dat_credit_depth(nsu_ctx, NOC_ROUTER_VC_DEPTH);
        $display("DAT_CREDIT_DEPTH router=%0d nmu_rx=%0d nsu_rx=%0d",
            NOC_ROUTER_VC_DEPTH, NOC_ROUTER_VC_DEPTH, NOC_NI_DAT_RX_VC_DEPTH);
        master = new(vip);
        scoreboard = new(vip);
        if (!$value$plusargs("stim_dir=%s", stim_dir)) $fatal(1, "Missing stim_dir");
        master.load_files({stim_dir, "/read.txt"}, {stim_dir, "/write.txt"});
        corrupt_rsp = $test$plusargs("corrupt_rsp");
        void'($value$plusargs("min_outstanding=%d", min_outstanding));
        void'($value$plusargs("min_unique=%d", min_unique));
        expected_writes = master.num_writes;
        expected_reads = master.num_reads;
        expected_beats = 0;
        foreach (master.ar_queue[i]) begin
            expected_ar[master.ar_queue[i].ax_id].push_back(master.ar_queue[i]);
            expected_beats += int'(master.ar_queue[i].ax_len) + 1;
        end
        if (expected_writes == 0 || expected_reads == 0)
            $fatal(1, "Empty memory test");
        repeat (5) @(negedge clk);
        rst_n = 1;
        wait (axi_rst_n && noc_rst_n);
        @(posedge clk);
        scoreboard.enable_all_checks();
        scoreboard.monitor();
        fork master.run_aw(); master.run_w(); master.wait_b(); join
        fork master.run_ar(); master.wait_r(); join
        repeat (10) @(posedge clk);
        if (b_count != expected_writes || r_count != expected_reads ||
            r_beats != expected_beats || checked_bytes == 0)
            $fatal(1, "Transaction count mismatch");
        foreach (expected_ar[id])
            if (expected_ar[id].size() != 0) $fatal(1, "Unreturned read ID %0d", id);
        if (peak_w < min_outstanding || peak_r < min_outstanding ||
            peak_unique_w < min_unique || peak_unique_r < min_unique)
            $fatal(1, "Outstanding/ID coverage not reached");
        $display("COVERAGE peak_w=%0d peak_r=%0d unique_w=%0d unique_r=%0d",
            peak_w, peak_r, peak_unique_w, peak_unique_r);
        scoreboard.reset();
        $display("NMU_COSIM_COUNTS writes=%0d reads=%0d r_beats=%0d checked_bytes=%0d",
            b_count, r_count, r_beats, checked_bytes);
        cmodel_finalize();
        $finish;
    end
    initial begin
        repeat (100000) @(posedge clk);
        $fatal(1, "NMU co-simulation timeout");
    end
    always @(posedge clk) begin : check_responses
        master_t::ax_beat_t request;
        logic [AXI_ADDR_WIDTH-1:0] address;
        logic [7:0] expected_byte;
        int lane, total_w, total_r, unique_w, unique_r;
        #2ns;
        if (axi_rst_n && vip.aw_valid && vip.aw_ready) live_w[vip.aw_id]++;
        if (axi_rst_n && vip.ar_valid && vip.ar_ready) live_r[vip.ar_id]++;
        if (axi_rst_n && vip.b_valid && vip.b_ready) begin
            if ($isunknown(vip.b_id) || vip.b_resp !== axi_pkg::RESP_OKAY)
                $fatal(1, "Invalid B response");
            if (live_w[vip.b_id] == 0) $fatal(1, "Unsolicited B response");
            live_w[vip.b_id]--;
            b_count++;
        end
        if (axi_rst_n && vip.r_valid && vip.r_ready) begin
            if ($isunknown(vip.r_id) || expected_ar[vip.r_id].size() == 0)
                $fatal(1, "Unexpected read ID");
            request = expected_ar[vip.r_id][0];
            if (vip.r_resp !== axi_pkg::RESP_OKAY ||
                vip.r_last !== (read_beat[vip.r_id] == int'(request.ax_len)))
                $fatal(1, "Invalid RRESP/RLAST");
            address = request.ax_addr + AXI_ADDR_WIDTH'(read_beat[vip.r_id] << request.ax_size);
            for (int byte_idx = 0; byte_idx < (1 << request.ax_size); byte_idx++) begin
                lane = int'(address % (AXI_DATA_WIDTH/8)) + byte_idx;
                scoreboard.get_byte(address + AXI_ADDR_WIDTH'(byte_idx), expected_byte);
                if ($isunknown(expected_byte) || $isunknown(vip.r_data[lane*8 +: 8]))
                    $fatal(1, "Read comparison contains uninitialized data");
                checked_bytes++;
            end
            r_beats++;
            if (vip.r_last) begin
                void'(expected_ar[vip.r_id].pop_front());
                read_beat[vip.r_id] = 0;
                if (live_r[vip.r_id] == 0) $fatal(1, "Unsolicited R response");
                live_r[vip.r_id]--;
                r_count++;
            end else read_beat[vip.r_id]++;
        end
        total_w = 0; total_r = 0; unique_w = 0; unique_r = 0;
        foreach (live_w[id]) begin
            total_w += live_w[id]; total_r += live_r[id];
            if (live_w[id] != 0) unique_w++;
            if (live_r[id] != 0) unique_r++;
        end
        if (total_w > peak_w) peak_w = total_w;
        if (total_r > peak_r) peak_r = total_r;
        if (unique_w > peak_unique_w) peak_unique_w = unique_w;
        if (unique_r > peak_unique_r) peak_unique_r = unique_r;
    end
`ifdef DUMP_WAVE
    initial begin : dump_wave
        string wave_file;
        if (!$value$plusargs("wave_file=%s", wave_file)) wave_file = "nmu_cosim.fsdb";
        $fsdbDumpfile(wave_file);
        $fsdbDumpvars(0, tb_nmu_cosim, "+all");
    end
`endif
endmodule
