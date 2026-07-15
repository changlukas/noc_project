// user_node_endpoint — per-node test endpoint: pulp axi_file_master +
// axi_rand_slave (MAPPED tile memory) + in-endpoint axi_scoreboard +
// FlooNoC axi_bw_monitor. Bridges the fabric's flat ni_signals_pkg structs
// to pulp AXI_BUS_DV interfaces with explicit per-field wiring (no protocol
// logic).
// pulp axi_scoreboard is usable on the Verilator directed axis: the 8'hxx->8'h00
// 2-state collapse only bites reads of never-written addresses, which a
// full-readback directed run never issues. Wired in-endpoint on master_dv.
//
// Run flavor: data integrity — axi_file_master two-phase (write -> barrier ->
// read) + in-endpoint axi_scoreboard on master_dv, MAPPED rand_slave as tile
// memory. Stimulus from <stim_dir>/node<ID>/{write,read}.txt (+stim_dir=).
//
// Plusargs: +num_reads=<n> +num_writes=<n> (per node, defaults below).

`ifndef USER_NODE_ENDPOINT_SV
`define USER_NODE_ENDPOINT_SV

module user_node_endpoint #(
    parameter int unsigned NODE_ID      = 0,
    parameter int unsigned NUM_NODES    = 1,
    parameter int unsigned ID_WIDTH     = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH   = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH   = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    // region contract (spec): master m targets REGION_BASE[NUM_NODES-1-m] only
    // (permutation pairing). REGION_BASE[s] = coord_id(s) << 32 (dst tile in addr
    // bits 32+), stamped by gen_tb_top.py from the topology YAML. Packed (not
    // unpacked) array: Verilator 5.048 rejects an override assignment pattern on an
    // unpacked array param whose size depends on a sibling param override.
    parameter logic [NUM_NODES-1:0][63:0] REGION_BASE = '0,
    parameter longint unsigned REGION_BYTES = 64'h1000,
    parameter int unsigned DEFAULT_NUM_READS  = 8,
    parameter int unsigned DEFAULT_NUM_WRITES = 8
) (
    input  logic                       clk_i,
    input  logic                       rst_ni,
    output ni_signals_pkg::axi_req_t   master_axi_req_o,
    input  ni_signals_pkg::axi_rsp_t   master_axi_rsp_i,
    input  ni_signals_pkg::axi_req_t   slave_axi_req_i,
    output ni_signals_pkg::axi_rsp_t   slave_axi_rsp_o,
    output logic                       end_of_sim_o,
    output int unsigned                txn_cnt_o
);

    localparam time ApplTime = 2ns;   // FlooNoC values; clk is 10 ns
    localparam time TestTime = 8ns;

    // ------------------------------------------------------------------
    // DV interfaces + flat-struct bridging (explicit wiring, both faces)
    // ------------------------------------------------------------------
    AXI_BUS_DV #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(1)
    ) master_dv (clk_i);

    AXI_BUS_DV #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(1)
    ) slave_dv (clk_i);

    // master face: file_master drives master_dv; forward to the flat NMU port.
    always_comb begin
        master_axi_req_o = '0;
        master_axi_req_o.awid     = master_dv.aw_id;
        master_axi_req_o.awaddr   = master_dv.aw_addr;
        master_axi_req_o.awlen    = master_dv.aw_len;
        master_axi_req_o.awsize   = master_dv.aw_size;
        master_axi_req_o.awburst  = master_dv.aw_burst;
        master_axi_req_o.awlock   = master_dv.aw_lock;
        master_axi_req_o.awcache  = master_dv.aw_cache;
        master_axi_req_o.awprot   = master_dv.aw_prot;
        master_axi_req_o.awqos    = master_dv.aw_qos;
        master_axi_req_o.awregion = master_dv.aw_region;
        master_axi_req_o.awvalid  = master_dv.aw_valid;
        master_axi_req_o.wdata    = master_dv.w_data;
        master_axi_req_o.wstrb    = master_dv.w_strb;
        master_axi_req_o.wlast    = master_dv.w_last;
        master_axi_req_o.wvalid   = master_dv.w_valid;
        master_axi_req_o.bready   = master_dv.b_ready;
        master_axi_req_o.arid     = master_dv.ar_id;
        master_axi_req_o.araddr   = master_dv.ar_addr;
        master_axi_req_o.arlen    = master_dv.ar_len;
        master_axi_req_o.arsize   = master_dv.ar_size;
        master_axi_req_o.arburst  = master_dv.ar_burst;
        master_axi_req_o.arlock   = master_dv.ar_lock;
        master_axi_req_o.arcache  = master_dv.ar_cache;
        master_axi_req_o.arprot   = master_dv.ar_prot;
        master_axi_req_o.arqos    = master_dv.ar_qos;
        master_axi_req_o.arregion = master_dv.ar_region;
        master_axi_req_o.arvalid  = master_dv.ar_valid;
        master_axi_req_o.rready   = master_dv.r_ready;
    end
    assign master_dv.aw_ready = master_axi_rsp_i.awready;
    assign master_dv.w_ready  = master_axi_rsp_i.wready;
    assign master_dv.b_id     = master_axi_rsp_i.bid;
    assign master_dv.b_resp   = master_axi_rsp_i.bresp;
    assign master_dv.b_user   = '0;
    assign master_dv.b_valid  = master_axi_rsp_i.bvalid;
    assign master_dv.ar_ready = master_axi_rsp_i.arready;
    assign master_dv.r_id     = master_axi_rsp_i.rid;
    assign master_dv.r_data   = master_axi_rsp_i.rdata;
    assign master_dv.r_resp   = master_axi_rsp_i.rresp;
    assign master_dv.r_last   = master_axi_rsp_i.rlast;
    assign master_dv.r_user   = '0;
    assign master_dv.r_valid  = master_axi_rsp_i.rvalid;
    // aw_atop / *_user driven by the class are dropped (out of scope).

    // slave face: forward the flat NSU port into slave_dv; rand_slave responds.
    assign slave_dv.aw_id     = slave_axi_req_i.awid;
    assign slave_dv.aw_addr   = slave_axi_req_i.awaddr;
    assign slave_dv.aw_len    = slave_axi_req_i.awlen;
    assign slave_dv.aw_size   = slave_axi_req_i.awsize;
    assign slave_dv.aw_burst  = slave_axi_req_i.awburst;
    assign slave_dv.aw_lock   = slave_axi_req_i.awlock;
    assign slave_dv.aw_cache  = slave_axi_req_i.awcache;
    assign slave_dv.aw_prot   = slave_axi_req_i.awprot;
    assign slave_dv.aw_qos    = slave_axi_req_i.awqos;
    assign slave_dv.aw_region = slave_axi_req_i.awregion;
    assign slave_dv.aw_atop   = '0;
    assign slave_dv.aw_user   = '0;
    assign slave_dv.aw_valid  = slave_axi_req_i.awvalid;
    assign slave_dv.w_data    = slave_axi_req_i.wdata;
    assign slave_dv.w_strb    = slave_axi_req_i.wstrb;
    assign slave_dv.w_last    = slave_axi_req_i.wlast;
    assign slave_dv.w_user    = '0;
    assign slave_dv.w_valid   = slave_axi_req_i.wvalid;
    assign slave_dv.b_ready   = slave_axi_req_i.bready;
    assign slave_dv.ar_id     = slave_axi_req_i.arid;
    assign slave_dv.ar_addr   = slave_axi_req_i.araddr;
    assign slave_dv.ar_len    = slave_axi_req_i.arlen;
    assign slave_dv.ar_size   = slave_axi_req_i.arsize;
    assign slave_dv.ar_burst  = slave_axi_req_i.arburst;
    assign slave_dv.ar_lock   = slave_axi_req_i.arlock;
    assign slave_dv.ar_cache  = slave_axi_req_i.arcache;
    assign slave_dv.ar_prot   = slave_axi_req_i.arprot;
    assign slave_dv.ar_qos    = slave_axi_req_i.arqos;
    assign slave_dv.ar_region = slave_axi_req_i.arregion;
    assign slave_dv.ar_user   = '0;
    assign slave_dv.ar_valid  = slave_axi_req_i.arvalid;
    assign slave_dv.r_ready   = slave_axi_req_i.rready;
    always_comb begin
        slave_axi_rsp_o = '0;
        slave_axi_rsp_o.awready = slave_dv.aw_ready;
        slave_axi_rsp_o.wready  = slave_dv.w_ready;
        slave_axi_rsp_o.bid     = slave_dv.b_id;
        slave_axi_rsp_o.bresp   = slave_dv.b_resp;
        slave_axi_rsp_o.bvalid  = slave_dv.b_valid;
        slave_axi_rsp_o.arready = slave_dv.ar_ready;
        slave_axi_rsp_o.rid     = slave_dv.r_id;
        slave_axi_rsp_o.rdata   = slave_dv.r_data;
        slave_axi_rsp_o.rresp   = slave_dv.r_resp;
        slave_axi_rsp_o.rlast   = slave_dv.r_last;
        slave_axi_rsp_o.rvalid  = slave_dv.r_valid;
    end

    // ------------------------------------------------------------------
    // VIP classes
    // ------------------------------------------------------------------
    typedef axi_test::axi_file_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime)
    ) file_master_t;
    // Zero response wait: an ideal sink so the FABRIC is the bottleneck, not the
    // slave. The pulp default (AX_MAX_WAIT_CYCLES=100, RESP=20, R=5) throttles
    // responses and hides fabric saturation (measured util ~1.2% at greedy
    // injection). Standard NoC-eval practice (booksim2 consumes at the sink).
    // The directed two-phase run is a data-integrity gate (scoreboard compares
    // read data vs golden, timing-independent), so a fast slave keeps it passing.
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b1),
        .AX_MAX_WAIT_CYCLES(0), .R_MAX_WAIT_CYCLES(0), .RESP_MAX_WAIT_CYCLES(0)
    ) rand_slave_t;
    typedef axi_test::axi_scoreboard #(
        .IW(ID_WIDTH), .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .UW(1), .TT(TestTime)
    ) scoreboard_t;

    // run_done drives end_of_sim_o for ALL flavors: declare it exactly ONCE here,
    // above the ifdef, and delete the per-arm copies. run_done is set by the
    // stimulus initial (procedural, blocking); the output port is refreshed
    // through a clocked register because Verilator does not reliably propagate
    // a procedurally-assigned output-port variable to the instantiating scope.
    logic run_done = 1'b0;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) end_of_sim_o <= 1'b0;
        else end_of_sim_o <= run_done;
    end

    file_master_t file_master;
    rand_slave_t  rand_slave;
    scoreboard_t  scoreboard;

    // Stimulus root: <stim_dir>/node<NODE_ID>/{write,read}.txt (emitter output).
    string stim_dir = "sim/test_patterns/directed";
    string write_path;
    string read_path;

    // MAPPED memory slave = this node's tile memory (persists across both phases).
    initial begin
        rand_slave = new(slave_dv);
        rand_slave.reset();
        @(posedge rst_ni);
        rand_slave.run();
    end

    // In-endpoint scoreboard on master_dv: golden from this node's W, check on
    // its R (end-to-end round trip through the NoC). enable_all_checks turns on
    // read-data + B-resp + R-resp checks; monitor() forks the sampling.
    // Read once per caller, into a local. A shared module-scope variable written
    // by one initial block and read by another has no defined ordering.
    function automatic int unsigned get_injection_mode();
        int unsigned m = 0;
        void'($value$plusargs("injection_mode=%d", m));
        return m;
    endfunction

    initial begin
        scoreboard = new(master_dv);
        scoreboard.reset();
        @(posedge rst_ni);
        // Mode 1 interleaves reads and writes, so a read may precede the write to
        // its address and the scoreboard's write-before-read precondition fails.
        // Skip BOTH enable_all_checks() and monitor(): construction hooks nothing,
        // but monitor() forks the sampling tasks and mutates the memory model even
        // with checks off.
        if (get_injection_mode() == 0) begin
            scoreboard.enable_all_checks();
            scoreboard.monitor();
        end
    end

    // Traffic mode (perf sweep): continuous interleaved injection paced by a
    // per-cycle gate. Selected at runtime by +injection_mode=1, paced by
    // +injection_rate; mode 0 (default) runs the two-phase directed run below
    // unchanged. Gate uses $urandom_range (PRNG, no constraint solver => no z3).
    // Gated copies of run_aw/run_ar: same body as axi_test.sv:2540-2565 plus a
    // per-cycle idle before each send.
    real injection_rate;
    int  unsigned inj_gate_pct;

    task automatic gated_run_aw();
        while (file_master.aw_queue.size() > 0) begin
            while ($urandom_range(0, 99) >= inj_gate_pct) @(posedge clk_i);
            file_master.drv.send_aw(file_master.aw_queue[0]);
            void'(file_master.aw_queue.pop_front());
        end
    endtask

    task automatic gated_run_ar();
        while (file_master.ar_queue.size() > 0) begin
            while ($urandom_range(0, 99) >= inj_gate_pct) @(posedge clk_i);
            file_master.drv.send_ar(file_master.ar_queue[0]);
            void'(file_master.ar_queue.pop_front());
        end
    endtask

    // Directed driver: per-node two-phase. load_files() fills the queues (do NOT
    // call run(): it re-forks all five and double-consumes the queues, spec
    // Two-phase). Phase 1 drains all writes (wait_b => committed at the slave);
    // phase 2 issues reads, checked by the scoreboard against golden.
    initial begin
        void'($value$plusargs("stim_dir=%s", stim_dir));
        write_path = $sformatf("%s/node%0d/write.txt", stim_dir, NODE_ID);
        read_path  = $sformatf("%s/node%0d/read.txt",  stim_dir, NODE_ID);
        file_master = new(master_dv);
        file_master.load_files(read_path, write_path);
        @(posedge rst_ni);
        if (get_injection_mode() == 1) begin
            injection_rate = 1.0;
            void'($value$plusargs("injection_rate=%f", injection_rate));
            inj_gate_pct = int'(injection_rate * 100.0);
            // Continuous injection: one phase, reads and writes interleaved, each
            // send gated per cycle on injection_rate. join (not join_none) so B/R
            // are consumed and the pass terminates cleanly.
            fork
                gated_run_aw();
                file_master.run_w();
                gated_run_ar();
                file_master.wait_b();
                file_master.wait_r();
            join
            run_done = 1'b1;
        end else begin
            fork file_master.run_aw(); file_master.run_w(); file_master.wait_b(); join
            fork file_master.run_ar(); file_master.wait_r(); join
            run_done = 1'b1;
        end
    end

    // ------------------------------------------------------------------
    // FlooNoC bw monitor (endpoint perf; $display at end_of_sim) +
    // non-vacuous handshake counter.
    // ------------------------------------------------------------------
    axi_vip_types_pkg::vip_req_t  mon_mst_req;
    axi_vip_types_pkg::vip_resp_t mon_mst_rsp;
    assign mon_mst_req = axi_vip_types_pkg::vip_req_from_flat(master_axi_req_o);
    assign mon_mst_rsp = axi_vip_types_pkg::vip_rsp_from_flat(master_axi_rsp_i);

    axi_bw_monitor #(
        .req_t(axi_vip_types_pkg::vip_req_t),
        .rsp_t(axi_vip_types_pkg::vip_resp_t),
        .AxiIdWidth(ID_WIDTH),
        .Name($sformatf("node%0d.master", NODE_ID))
    ) u_bw_mst (
        .clk_i(clk_i), .en_i(rst_ni), .end_of_sim_i(end_of_sim_o),
        .req_i(mon_mst_req), .rsp_i(mon_mst_rsp),
        .ar_in_flight_o(), .aw_in_flight_o()
    );

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) txn_cnt_o <= 0;
        else begin
            txn_cnt_o <= txn_cnt_o
                + 32'(master_axi_req_o.awvalid && master_axi_rsp_i.awready)
                + 32'(master_axi_req_o.arvalid && master_axi_rsp_i.arready);
        end
    end

endmodule

`endif  // USER_NODE_ENDPOINT_SV
