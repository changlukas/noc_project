`timescale 1ns/1ps

// The testbench body every configuration shares: clock and reset, the DPI
// handle lifecycle, the fabric, one user_node_endpoint per endpoint, the
// watchdog, the perf instrumentation and the exit logic. A configuration is
// then a parameter list rather than a file -- see sim/tb/tb_noc_mesh.sv, which
// supplies the geometry and the address map from topology_pkg and instantiates
// this module.
//
// Checking: pulp axi_scoreboard lives inside each endpoint on master_dv,
// comparing read data end-to-end through the NoC against golden write data.
//
// Self-clocked: clk_i/rst_ni are internal logic (10 ns clock, 4-cycle reset).
// Plusargs: +num_reads=<n> +num_writes=<n> (per endpoint); seed via
// +verilator+seed+<N>.

`ifndef NOC_TB_TOP_SV
`define NOC_TB_TOP_SV

`include "noc_fabric.sv"

module noc_tb_top #(
    // Mesh geometry. The linear node index IS the array position: X = i % X_DIM,
    // Y = i / X_DIM.
    parameter int unsigned X_DIM = 2,
    parameter int unsigned Y_DIM = 2,
    // Endpoints, not nodes: a peripheral has an NI and an endpoint but no
    // router, so NUM_ENDPOINTS = X_DIM*Y_DIM + N_PERIPH. Every loop that walks
    // the initiators walks endpoints -- each one injects and each one can wedge.
    parameter int unsigned NUM_ENDPOINTS = 4,
    parameter int unsigned ID_WIDTH   = ni_params_pkg::NOC_ID_WIDTH,
    parameter int unsigned ADDR_WIDTH = ni_params_pkg::AXI_ADDR_WIDTH,
    parameter int unsigned DATA_WIDTH = ni_params_pkg::AXI_DATA_WIDTH,
    // Tile crossbar windows, one field per target in port order (m0 = config,
    // last = data), one row per endpoint. Each endpoint decodes on its OWN
    // windows: a hit is tile-local and never touches the NoC, a miss falls
    // through to the default master port and goes onto the NoC. PACKED, as in
    // noc_fabric: Verilator 5.048 sizes an unpacked-array parameter from its
    // DEFAULT, so an override whose length follows a sibling parameter is
    // rejected.
    parameter int unsigned TILE_TARGETS = 2,
    parameter logic [NUM_ENDPOINTS-1:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_BASE_ADDR = '0,
    parameter logic [NUM_ENDPOINTS-1:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_SIZE = '0,
    // NoC egress aperture: where a collective write is offset to so the tile
    // crossbar routes it to the NI instead of answering it locally.
    parameter logic [ADDR_WIDTH-1:0] NOC_EGRESS_BASE = '0,
    // REGION_BYTES = the DV region_bytes constant (NOT a tile size -- that would
    // blow up MAX_BURST_BEATS below).
    parameter longint unsigned REGION_BYTES = 64'h1000,
    // Off-mesh peripherals, in endpoint order; forwarded to noc_fabric, which
    // documents the encoding. Field p is peripheral p, so element [0] is the LSB
    // byte and the order must survive this module boundary unreversed.
    parameter int unsigned N_PERIPH     = 0,
    parameter int unsigned N_PERIPH_MAX = (N_PERIPH > 0) ? N_PERIPH : 1,
    parameter logic [N_PERIPH_MAX-1:0][7:0] PERIPH_NODE = '0,
    parameter logic [N_PERIPH_MAX-1:0][7:0] PERIPH_PORT = '0,
    // DAT face VC count. Default from where the parameter is defined:
    // noc.NUM_DAT_VC in specgen/source/constants.yaml, the same value
    // build_config.mk picks the noc_types_pkg_vc<N> flit package with.
    parameter int unsigned NUM_DAT_VC = ni_params_pkg::NUM_DAT_VC,
    // NMU read reorder buffer: 1 = the reorder-buffer response path
    // docs/noc-target-spec.md section 3 describes, 0 = the RoBless bypass with
    // its per-id single-outstanding interlock. int unsigned, not bit: it goes
    // straight into cmodel_nmu_create_ex's `input int rob_enabled`. Default from
    // where the parameter is defined: nmu.R_ROB_EN in
    // specgen/source/constants.yaml.
    parameter int unsigned R_ROB_EN = ni_params_pkg::NMU_R_ROB_EN,
    // Tile-memory latency profile. Every endpoint's two memories sit behind an
    // axi_delayer carrying these settings; input covers AW/W/AR, output B/R.
    parameter bit          MEM_STALL_RANDOM_INPUT  = 1'b0,
    parameter bit          MEM_STALL_RANDOM_OUTPUT = 1'b0,
    parameter int unsigned MEM_FIXED_DELAY_INPUT   = 0,
    parameter int unsigned MEM_FIXED_DELAY_OUTPUT  = 0,
    // Master-face backpressure profile. One axi_delayer per endpoint between the
    // file master and the tile crossbar, response side only.
    parameter bit          MST_STALL_RANDOM_OUTPUT = 1'b1,
    parameter int unsigned MST_FIXED_DELAY_OUTPUT  = 0
) ();

    logic clk_i  = 1'b0;
    logic rst_ni = 1'b0;
    always #5 clk_i = ~clk_i;
    initial begin
        repeat (4) @(posedge clk_i);
        rst_ni = 1'b1;
    end

    localparam int unsigned NUM_NODES      = X_DIM * Y_DIM;
    localparam int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH;
    localparam int unsigned RSP_FLIT_WIDTH = ni_params_pkg::NOC_RSP_FLIT_WIDTH;
    localparam int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH;
    // ROUTER_VC_DEPTH: credit window for inter-router links; passed to fabric so
    // link_perf_monitor tracks the actual receiving buffer depth.
    localparam int unsigned ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH;

    // -------------------------------------------------------------------------
    // Liveness trace. The watchdog below reports that time ran out; these two
    // registers say where it stopped. last_progress is the cycle of an
    // endpoint's most recent master-side handshake, axi_outstanding the AW/AR it
    // has issued and not yet retired. A wedged endpoint reads as a stale
    // last_progress with axi_outstanding > 0; one that simply finished reads as
    // axi_outstanding == 0. Without these a timeout snapshot cannot separate a
    // freeze at cycle 500 from one at cycle 99999.
    // -------------------------------------------------------------------------
    int unsigned live_cyc = 0;
    int unsigned last_progress  [NUM_ENDPOINTS];
    int unsigned axi_outstanding[NUM_ENDPOINTS];

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            live_cyc <= 0;
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                last_progress[i]   <= 0;
                axi_outstanding[i] <= 0;
            end
        end else begin
            live_cyc <= live_cyc + 1;
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                automatic logic aw = master_axi_req[i].awvalid && master_axi_rsp[i].awready;
                automatic logic ar = master_axi_req[i].arvalid && master_axi_rsp[i].arready;
                automatic logic wb = master_axi_req[i].wvalid  && master_axi_rsp[i].wready;
                automatic logic bh = master_axi_rsp[i].bvalid  && master_axi_req[i].bready;
                automatic logic rl = master_axi_rsp[i].rvalid  && master_axi_req[i].rready
                                     && master_axi_rsp[i].rlast;
                // W beats count as progress: a node mid-burst is moving, not wedged.
                if (aw || ar || wb || bh || rl) last_progress[i] <= live_cyc;
                axi_outstanding[i] <= axi_outstanding[i]
                                      + int'(aw) + int'(ar) - int'(bh) - int'(rl);
            end
        end
    end

    // -------------------------------------------------------------------------
    // Watchdog - sized by worst-case beats in flight. Three per-beat costs,
    // named separately because only two of them move with the latency profile:
    //   fabric  measured vc1 rate is ~15-30 cycles per R/W beat (credit window
    //           4, all nodes contending); 40 adds margin.
    //   memory  the axi_delayer in front of each tile memory. stream_delay holds
    //           one handshake at a time on each channel, so its bound is per
    //           beat on the request side and again on the response side, not per
    //           transaction.
    //   master  the axi_delayer on the master face, response side only.
    // The two delayer costs are DERIVED from the profile parameters rather than
    // passed in beside them, so a testbench cannot arm a stall and leave the
    // watchdog budgeted for none. STALL_RANDOM_MAX_CYCLES is lfsr_16bit's own
    // bound: a $clog2(16) = 4-bit reload stalls a handshake 0-15 cycles.
    // MAX_BURST_BEATS caps the largest burst per REGION_BYTES.
    // -------------------------------------------------------------------------
    localparam int unsigned STALL_RANDOM_MAX_CYCLES = 15;
    localparam int unsigned MEM_IN_CYC  = MEM_STALL_RANDOM_INPUT
                                        ? STALL_RANDOM_MAX_CYCLES : MEM_FIXED_DELAY_INPUT;
    localparam int unsigned MEM_OUT_CYC = MEM_STALL_RANDOM_OUTPUT
                                        ? STALL_RANDOM_MAX_CYCLES : MEM_FIXED_DELAY_OUTPUT;
    localparam int unsigned TIMEOUT_BASE        = 100000;
    localparam int unsigned FABRIC_CYC_PER_BEAT = 40;
    localparam int unsigned MEM_CYC_PER_BEAT    = (MEM_IN_CYC > MEM_OUT_CYC)
                                                ? MEM_IN_CYC : MEM_OUT_CYC;
    localparam int unsigned MST_CYC_PER_BEAT    = MST_STALL_RANDOM_OUTPUT
                                                ? STALL_RANDOM_MAX_CYCLES : MST_FIXED_DELAY_OUTPUT;
    localparam int unsigned K_CYC_PER_BEAT  = FABRIC_CYC_PER_BEAT + MEM_CYC_PER_BEAT
                                            + MST_CYC_PER_BEAT;
    localparam int unsigned MAX_BURST_BEATS = int'(REGION_BYTES) / (DATA_WIDTH / 8);
    import "DPI-C" context function void cmodel_dump_fabric_state();
    initial begin
        int unsigned timeout_cycles;
        int unsigned expected_total;
        @(posedge rst_ni);
        expected_total = 0;
        for (int i = 0; i < NUM_ENDPOINTS; i++)
            expected_total += expected_txn_cnt[i];
        timeout_cycles = TIMEOUT_BASE
            + K_CYC_PER_BEAT * expected_total * MAX_BURST_BEATS;
        // Forensics override: fire the watchdog just past a known freeze
        // point so the state dump lands without waiting out the formula.
        void'($value$plusargs("timeout_cycles=%d", timeout_cycles));
        repeat (timeout_cycles) @(posedge clk_i);
        // Per-node SV-side summary, then the c_model fabric state dump.
        for (int i = 0; i < NUM_ENDPOINTS; i++) begin
            $display("[WATCHDOG] node%0d txn_cnt=%0d end_of_sim=%0d outstanding=%0d last_progress=%0d (idle %0d cyc) mst[awv=%0d wv=%0d arv=%0d rr=%0d br=%0d] slv[awv=%0d wv=%0d arv=%0d rv=%0d bv=%0d]",
                     i, txn_cnt[i], end_of_sim[i],
                     axi_outstanding[i], last_progress[i], live_cyc - last_progress[i],
                     master_axi_req[i].awvalid, master_axi_req[i].wvalid,
                     master_axi_req[i].arvalid, master_axi_req[i].rready,
                     master_axi_req[i].bready,
                     slave_axi_req[i].awvalid, slave_axi_req[i].wvalid,
                     slave_axi_req[i].arvalid,
                     slave_axi_rsp[i].rvalid, slave_axi_rsp[i].bvalid);
        end
        cmodel_dump_fabric_state();
        $fatal(1, "tb_top: timeout after %0d cycles", timeout_cycles);
    end

    // -------------------------------------------------------------------------
    // DPI lifecycle
    // -------------------------------------------------------------------------
    import "DPI-C" context function void    cmodel_init();
    import "DPI-C" context function void    cmodel_finalize();
    import "DPI-C" context function longint unsigned cmodel_router_create(input string name,
                                                                  input int x_coord, input int y_coord,
                                                                  input int mesh_x_dim, input int mesh_y_dim,
                                                                  input int num_vc);
    import "DPI-C" context function int unsigned cmodel_nmu_read_slot_hwm(input longint unsigned ctx);
    import "DPI-C" context function void cmodel_nmu_admission_stats(input longint unsigned ctx,
                                                                 output int unsigned aw_idle_bypass,
                                                                 output int unsigned aw_same_dest_bypass,
                                                                 output int unsigned aw_fallback_alloc,
                                                                 output int unsigned ar_idle_bypass,
                                                                 output int unsigned ar_same_dest_bypass,
                                                                 output int unsigned ar_fallback_alloc,
                                                                 output int unsigned order_list_hwm,
                                                                 output int unsigned write_txns_hwm,
                                                                 output int unsigned read_txns_hwm);
    import "DPI-C" context function longint unsigned cmodel_nmu_create_ex(input string name,
                                                                 input int src_id, input int num_vc,
                                                                 input int rob_enabled,
                                                                 input int b_rob_depth,
                                                                 input int r_rob_depth,
                                                                 input int max_txns_per_id,
                                                                 input int port_id,
                                                                 input string config_path);
    import "DPI-C" context function void cmodel_nmu_set_channel_mode(input longint unsigned ctx,
                                                                        input int mode);
    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name,
                                                              input int src_id, input int num_vc,
                                                              input int max_unique_ids,
                                                              input int max_outstanding,
                                                              input int port_id,
                                                              input string config_path);
    import "DPI-C" context function void cmodel_nsu_set_channel_mode(input longint unsigned ctx,
                                                                        input int mode);
    import "DPI-C" context function void cmodel_nsu_meta_buffer_hwm(
                                                                 input longint unsigned ctx,
                                                                 output int unsigned write_hwm,
                                                                 output int unsigned read_hwm);
    import "DPI-C" context function longint unsigned cmodel_dat_merge_create(input string name,
                                                                    input int dat_num_vc);

    longint unsigned router_ctx     [NUM_NODES];
    longint unsigned nmu_ctx        [NUM_ENDPOINTS];
    longint unsigned nsu_ctx        [NUM_ENDPOINTS];
    longint unsigned dat_merge_ctx  [NUM_ENDPOINTS];

    // SAM config: the sim/configs/ file, with its endpoints block. Empty (the
    // default) keeps each NMU's default 16x16 uniform, 4 GB/tile SAM.
    string sam_config_path = "";

    // NSU knobs. max_unique_ids=1 collapses every master onto one downstream
    // AXI id (FlooNoC default); 2**AXI_ID_WIDTH passes the master's id through.
    // max_outstanding is the shared MetaBuffer pool per direction.
    int unsigned max_unique_ids  = ni_params_pkg::NSU_META_BUFFER_MAX_UNIQUE_IDS;
    int unsigned max_outstanding = ni_params_pkg::NSU_META_BUFFER_MAX_OUTSTANDING;

    // NMU RoB pool depths, per direction. Both <= 256 (ordering_tag is 8 bits).
    int unsigned b_rob_depth = ni_params_pkg::NMU_ROB_B_DEPTH;
    int unsigned r_rob_depth = ni_params_pkg::NMU_ROB_R_DEPTH;
    // Per-AXI-ID order-list depth (FlooNoC MaxRoTxnsPerId).
    int unsigned max_txns_per_id = ni_params_pkg::NMU_MAX_OUTSTANDING_PER_ID;
    int unsigned channel_mode = 0;
    int unsigned injection_mode = 0;
    string traffic_direction = "write";

    initial begin
        cmodel_init();
        void'($value$plusargs("channel_mode=%d", channel_mode));
        void'($value$plusargs("injection_mode=%d", injection_mode));
        void'($value$plusargs("traffic_direction=%s", traffic_direction));
        if (channel_mode != 0 && channel_mode != 2 && channel_mode != 3)
            $fatal(1, "noc_tb_top: channel_mode must be 0, 2 or 3");
        void'($value$plusargs("sam_config=%s", sam_config_path));
        void'($value$plusargs("max_unique_ids=%d", max_unique_ids));
        void'($value$plusargs("max_outstanding=%d", max_outstanding));
        // dat_num_vc is printed rather than derived by the reader: it comes from
        // specgen/source/constants.yaml and no longer appears in the config name,
        // so the log is the only place it is bound to the run that used it.
        $display("[Config] max_unique_ids=%0d max_outstanding=%0d dat_num_vc=%0d router_vc_depth=%0d mst_stall_random=%0d ni_dat_rx_vc_depth=%0d",
                 max_unique_ids, max_outstanding, NUM_DAT_VC,
                 ni_params_pkg::NOC_ROUTER_VC_DEPTH, MST_STALL_RANDOM_OUTPUT,
                 ni_params_pkg::NOC_NI_DAT_RX_VC_DEPTH);
        void'($value$plusargs("b_rob_depth=%d", b_rob_depth));
        void'($value$plusargs("r_rob_depth=%d", r_rob_depth));
        void'($value$plusargs("max_txns_per_id=%d", max_txns_per_id));
        for (int unsigned i = 0; i < NUM_NODES; i++)
            router_ctx[i] = cmodel_router_create($sformatf("router_%0d", i),
                                                 int'(i % X_DIM), int'(i / X_DIM),
                                                 int'(X_DIM), int'(Y_DIM), int'(NUM_DAT_VC));
        // NI creates cover the ENDPOINT space: the routers first, then one per
        // peripheral. src_id is the route coordinate (y<<X_WIDTH)|x, stamped
        // into every request the endpoint emits and what its responses come back
        // to. A peripheral SHARES its host router's coordinate, so src_id does
        // not tell the two apart: port_id does, and it is the same port the SAM
        // entry for this endpoint's region carries.
        for (int unsigned e = 0; e < NUM_ENDPOINTS; e++) begin
            automatic int unsigned node = (e < NUM_NODES) ? e
                                        : int'(PERIPH_NODE[e - NUM_NODES]);
            automatic int src_id = int'(((node / X_DIM) << ni_flit_pkg::X_WIDTH)
                                        | (node % X_DIM));
            automatic int port_id = (e < NUM_NODES) ? 0 : int'(PERIPH_PORT[e - NUM_NODES]);
            nmu_ctx[e] = cmodel_nmu_create_ex($sformatf("nmu_%0d", e), src_id, int'(NUM_DAT_VC),
                                              int'(R_ROB_EN), b_rob_depth, r_rob_depth,
                                              max_txns_per_id, port_id, sam_config_path);
            nsu_ctx[e] = cmodel_nsu_create($sformatf("nsu_%0d", e), src_id, int'(NUM_DAT_VC),
                                           max_unique_ids, max_outstanding, port_id,
                                           sam_config_path);
            cmodel_nmu_set_channel_mode(nmu_ctx[e], channel_mode);
            cmodel_nsu_set_channel_mode(nsu_ctx[e], channel_mode);
            dat_merge_ctx[e] = cmodel_dat_merge_create($sformatf("dat_merge_%0d", e),
                                                       int'(NUM_DAT_VC));
        end
    end

    // -------------------------------------------------------------------------
    // Per-node AXI buses (struct arrays): master-side into NMU, slave-side out of NSU
    // -------------------------------------------------------------------------
    ni_signals_pkg::axi_req_t  master_axi_req [NUM_ENDPOINTS];  // tb master -> NMU
    logic [ni_params_pkg::AXI_AWUSER_WIDTH-1:0] master_awuser [NUM_ENDPOINTS];  // AWUSER sideband
    ni_signals_pkg::axi_rsp_t  master_axi_rsp [NUM_ENDPOINTS];  // NMU -> tb master
    ni_signals_pkg::axi_req_t  slave_axi_req  [NUM_ENDPOINTS];  // NSU -> tb slave
    ni_signals_pkg::axi_rsp_t  slave_axi_rsp  [NUM_ENDPOINTS];  // tb slave -> NSU

    // -------------------------------------------------------------------------
    // NoC fabric (NUM_NODES routers, directional links, N_PERIPH off-mesh NIs)
    // -------------------------------------------------------------------------
    noc_fabric #(
        .X_DIM(X_DIM), .Y_DIM(Y_DIM),
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_DAT_VC(NUM_DAT_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),
        .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH),
        .ROUTER_VC_DEPTH(ROUTER_VC_DEPTH),
        .N_PERIPH(N_PERIPH),
        .PERIPH_NODE(PERIPH_NODE),
        .PERIPH_PORT(PERIPH_PORT)
    ) u_fabric (
        .clk_i(clk_i), .rst_ni(rst_ni), .measure_en(perf_measure_en),
        .router_ctx(router_ctx), .nmu_ctx(nmu_ctx), .nsu_ctx(nsu_ctx),
        .dat_merge_ctx(dat_merge_ctx),
        .master_axi_req(master_axi_req), .master_awuser(master_awuser),
        .master_axi_rsp(master_axi_rsp),
        .slave_axi_req(slave_axi_req),   .slave_axi_rsp(slave_axi_rsp)
    );

    // -------------------------------------------------------------------------
    // Test endpoints - one user_node_endpoint per ENDPOINT (pulp file_master +
    // axi_xbar tile crossbar + two axi_delayer/axi_sim_mem targets +
    // in-endpoint scoreboard + bw monitor). Peripherals are endpoints
    // NUM_NODES..NUM_ENDPOINTS-1: they have an NI and an endpoint but no router,
    // they carry their own stimulus, and the exit logic below gates on them too.
    // -------------------------------------------------------------------------
    logic        end_of_sim [NUM_ENDPOINTS];
    int unsigned txn_cnt    [NUM_ENDPOINTS];
    int unsigned expected_txn_cnt [NUM_ENDPOINTS];
    int unsigned expected_write_cnt [NUM_ENDPOINTS];
    longint unsigned stimulus_start_cycle [NUM_ENDPOINTS];
    longint unsigned stimulus_done_cycle [NUM_ENDPOINTS];
    logic        compare_ready [NUM_ENDPOINTS];
    logic        compare_start;
    logic        compare_finish;
    logic        compare_work_done [NUM_ENDPOINTS];
    logic        compare_done [NUM_ENDPOINTS];
    logic        compare_background [NUM_ENDPOINTS];
    logic        source_done [NUM_ENDPOINTS];
    logic        mode3_start = 1'b0;
    logic        mode4_start = 1'b0;
    logic        all_barrier_ready;
    logic        any_active_source;
    logic        all_active_sources_done;
    logic        all_compare_work_done;
    logic        all_compare_done;
    longint unsigned compare_barrier_cycle = 0;
    always_comb begin
        all_barrier_ready = 1'b1;
        any_active_source = 1'b0;
        all_active_sources_done = 1'b1;
        all_compare_work_done = 1'b1;
        all_compare_done = 1'b1;
        for (int i = 0; i < NUM_ENDPOINTS; i++) begin
            all_barrier_ready &= compare_ready[i];
            all_compare_work_done &= compare_work_done[i];
            all_compare_done &= compare_done[i];
            if (expected_txn_cnt[i] > 0) begin
                any_active_source = 1'b1;
                all_active_sources_done &= source_done[i];
            end
        end
        compare_start = injection_mode == 3 ? mode3_start : mode4_start;
        compare_finish = injection_mode == 3 && all_compare_work_done;
    end

    always @(posedge compare_finish) begin
        if (rst_ni && injection_mode == 3) begin
            compare_barrier_cycle = live_cyc;
            $display("[ChannelCompareBarrier] release_cycle=%0d",
                     compare_barrier_cycle);
        end
    end
    for (genvar i = 0; i < NUM_ENDPOINTS; i++) begin : g_endpoint
        user_node_endpoint #(
            .NODE_ID(i),
            // #57 split the endpoint's id width into AXI (external, its own
            // ni_params default) and NOC (this tb's ID_WIDTH knob).
            .NOC_ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
            .TILE_TARGETS(TILE_TARGETS), .TILE_BASE_ADDR(TILE_BASE_ADDR[i]),
            .TILE_SIZE(TILE_SIZE[i]), .NOC_EGRESS_BASE(NOC_EGRESS_BASE),
            .MEM_STALL_RANDOM_INPUT(MEM_STALL_RANDOM_INPUT),
            .MEM_STALL_RANDOM_OUTPUT(MEM_STALL_RANDOM_OUTPUT),
            .MEM_FIXED_DELAY_INPUT(MEM_FIXED_DELAY_INPUT),
            .MEM_FIXED_DELAY_OUTPUT(MEM_FIXED_DELAY_OUTPUT),
            .MST_STALL_RANDOM_OUTPUT(MST_STALL_RANDOM_OUTPUT),
            .MST_FIXED_DELAY_OUTPUT(MST_FIXED_DELAY_OUTPUT)
        ) u_endpoint (
            .clk_i(clk_i), .rst_ni(rst_ni),
            .master_axi_req_o(master_axi_req[i]), .master_awuser_o(master_awuser[i]),
            .master_axi_rsp_i(master_axi_rsp[i]),
            .slave_axi_req_i(slave_axi_req[i]),   .slave_axi_rsp_o(slave_axi_rsp[i]),
            .end_of_sim_o(end_of_sim[i]), .txn_cnt_o(txn_cnt[i]),
            .expected_txn_cnt_o(expected_txn_cnt[i]),
            .expected_write_cnt_o(expected_write_cnt[i]),
            .stimulus_start_cycle_o(stimulus_start_cycle[i]),
            .stimulus_done_cycle_o(stimulus_done_cycle[i]),
            .compare_ready_o(compare_ready[i]), .compare_start_i(compare_start),
            .compare_finish_i(compare_finish), .compare_work_done_o(compare_work_done[i]),
            .compare_done_o(compare_done[i]),
            .compare_background_o(compare_background[i]), .source_done_o(source_done[i])
        );
    end : g_endpoint

    // -------------------------------------------------------------------------
    // Perf instrumentation - sample every rising edge; dump on final
    // -------------------------------------------------------------------------
    import "DPI-C" context function void cmodel_perf_sample_tick();
    import "DPI-C" context function void cmodel_perf_set_run(input string scenario,
                                                             input longint total_cyc);
    import "DPI-C" context function void cmodel_perf_dump(input string path);
    import "DPI-C" context function void cmodel_perf_begin(input longint start_cyc);
    import "DPI-C" context function void cmodel_perf_end(input longint end_cyc);

    string        perf_out_path = "perf.json";
    string        perf_scn      = "";
    int unsigned  perf_cycle    = 0;
    logic         perf_measure_en = 1'b0;

    // Optional passive observations use the existing offline packet reader.
    `include "axi_perf_trace.svh"
    integer packet_trace_fd = 0;
    integer link_trace_fd = 0;
    string packet_trace_path;
    localparam int unsigned TRACE_FLIT_W =
        (DAT_FLIT_WIDTH > REQ_FLIT_WIDTH ? DAT_FLIT_WIDTH : REQ_FLIT_WIDTH) > RSP_FLIT_WIDTH ?
        (DAT_FLIT_WIDTH > REQ_FLIT_WIDTH ? DAT_FLIT_WIDTH : REQ_FLIT_WIDTH) : RSP_FLIT_WIDTH;

    task automatic trace_packet(input string event_name, input int endpoint,
                                input string plane, input logic [TRACE_FLIT_W-1:0] flit);
        automatic logic [TRACE_FLIT_W-1:0] normalized = flit;
        normalized[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB] = '0;
        $fdisplay(packet_trace_fd, "%0d,%s,%0d,%s,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0h",
            live_cyc, event_name, endpoint, plane,
            flit[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB],
            flit[ni_flit_pkg::FLIT_TAIL_MSB:ni_flit_pkg::FLIT_TAIL_LSB],
            flit[ni_flit_pkg::SRC_ID_MSB:ni_flit_pkg::SRC_ID_LSB],
            flit[ni_flit_pkg::DST_ID_MSB:ni_flit_pkg::DST_ID_LSB],
            flit[ni_flit_pkg::DST_PORT_ID_MSB:ni_flit_pkg::DST_PORT_ID_LSB],
            flit[ni_flit_pkg::COLLECTIVE_OP_MSB:ni_flit_pkg::COLLECTIVE_OP_LSB],
            flit[ni_flit_pkg::COLLECTIVE_MASK_MSB:ni_flit_pkg::COLLECTIVE_MASK_LSB],
            perf_measure_en, normalized);
    endtask

    initial begin
        if ($test$plusargs("packet_trace")) begin
            if (!$value$plusargs("perf_out=%s", packet_trace_path)) packet_trace_path = "perf.json";
            packet_trace_fd = $fopen({packet_trace_path, ".packets.csv"}, "w");
            if (!packet_trace_fd) $fatal(1, "Cannot open packet trace");
            $fdisplay(packet_trace_fd, "cycle,event,node,plane,vc,tail,src,dst,dst_port,collective,mask,in_window,flit");
            link_trace_fd = $fopen({packet_trace_path, ".links.csv"}, "w");
            if (!link_trace_fd) $fatal(1, "Cannot open link trace");
            $fdisplay(link_trace_fd, "cycle,event,link,in_window");
        end
    end

    always @(posedge clk_i) begin
        if (packet_trace_fd) begin
            if (!rst_ni) begin
                $fdisplay(packet_trace_fd, "%0d,RESET,0,REQ,0,0,0,0,0,0,0,0,0", live_cyc);
            end else begin
                $fdisplay(link_trace_fd, "%0d,TICK,,%0d", live_cyc, perf_measure_en);
                for (int node = 0; node < NUM_NODES; node++) begin
                    for (int port_id = 0; port_id < u_fabric.LINK_PORTS; port_id++) begin
                        automatic int target = -1;
                        case (port_id)
                            u_fabric.RP_EAST: if (node % X_DIM < X_DIM-1) target = node+1;
                            u_fabric.RP_WEST: if (node % X_DIM > 0) target = node-1;
                            u_fabric.RP_NORTH: if (node / X_DIM < Y_DIM-1) target = node+X_DIM;
                            u_fabric.RP_SOUTH: if (node / X_DIM > 0) target = node-X_DIM;
                            default: target = -1;
                        endcase
                        if (target >= 0) begin
                            if (u_fabric.tx_req_valid[node][port_id] && u_fabric.tx_req_ready[node][port_id])
                                $fdisplay(link_trace_fd, "%0d,LINK,req_%0dto%0d,%0d", live_cyc, node, target, perf_measure_en);
                            if (u_fabric.tx_rsp_valid[node][port_id] && u_fabric.tx_rsp_ready[node][port_id])
                                $fdisplay(link_trace_fd, "%0d,LINK,rsp_%0dto%0d,%0d", live_cyc, node, target, perf_measure_en);
                            if (u_fabric.tx_dat_valid[node][port_id])
                                $fdisplay(link_trace_fd, "%0d,LINK,dat_%0dto%0d,%0d", live_cyc, node, target, perf_measure_en);
                        end
                    end
                end
                for (int ep = 0; ep < NUM_ENDPOINTS; ep++) begin
                    automatic int host = ep < NUM_NODES ? ep : int'(PERIPH_NODE[ep-NUM_NODES]);
                    automatic int port_id = u_fabric.RP_LOCAL;
                    if (ep >= NUM_NODES) begin
                        if (PERIPH_PORT[ep-NUM_NODES] == 1)
                            port_id = host % X_DIM == 0 ? u_fabric.RP_WEST : u_fabric.RP_EAST;
                        else
                            port_id = host / X_DIM == 0 ? u_fabric.RP_SOUTH : u_fabric.RP_NORTH;
                    end
                    if (u_fabric.rx_req_valid[host][port_id] && u_fabric.rx_req_ready[host][port_id])
                        trace_packet("IN", ep, "REQ", TRACE_FLIT_W'(u_fabric.rx_req_flit[host][port_id]));
                    if (u_fabric.tx_req_valid[host][port_id] && u_fabric.tx_req_ready[host][port_id])
                        trace_packet("OUT", ep, "REQ", TRACE_FLIT_W'(u_fabric.tx_req_flit[host][port_id]));
                    if (u_fabric.rx_rsp_valid[host][port_id] && u_fabric.rx_rsp_ready[host][port_id])
                        trace_packet("IN", ep, "RSP", TRACE_FLIT_W'(u_fabric.rx_rsp_flit[host][port_id]));
                    if (u_fabric.tx_rsp_valid[host][port_id] && u_fabric.tx_rsp_ready[host][port_id])
                        trace_packet("OUT", ep, "RSP", TRACE_FLIT_W'(u_fabric.tx_rsp_flit[host][port_id]));
                    if (u_fabric.rx_dat_valid[host][port_id])
                        trace_packet("IN", ep, "DAT", TRACE_FLIT_W'(u_fabric.rx_dat_flit[host][port_id]));
                    if (u_fabric.tx_dat_valid[host][port_id])
                        trace_packet("OUT", ep, "DAT", TRACE_FLIT_W'(u_fabric.tx_dat_flit[host][port_id]));
                end
            end
        end
    end

    final begin
        if (packet_trace_fd) begin
            $fdisplay(packet_trace_fd, "%0d,END,0,REQ,0,0,0,0,0,0,0,0,0", live_cyc);
            $fclose(packet_trace_fd);
            $fdisplay(link_trace_fd, "%0d,END,,0", live_cyc);
            $fclose(link_trace_fd);
        end
    end
    logic         mode4_ended = 1'b0;
    logic         mode3_started = 1'b0;
    logic         mode3_ended = 1'b0;
    longint unsigned measurement_start_cycle = 0;
    longint unsigned measurement_end_cycle = 0;
    initial begin
        void'($value$plusargs("perf_out=%s", perf_out_path));
        void'($value$plusargs("perf_scenario=%s", perf_scn));
    end
    always @(posedge clk_i) begin
        // Sample after model ticks and NBA outputs at this clock edge.
        #1ps;
        cmodel_perf_sample_tick();
        perf_cycle = perf_cycle + 1;
    end

    always @(posedge clk_i) begin
        if (!rst_ni) begin
            perf_measure_en <= 1'b0;
            mode3_start <= 1'b0;
            mode4_start <= 1'b0;
            mode4_ended <= 1'b0;
            mode3_started <= 1'b0;
            mode3_ended <= 1'b0;
        end else if (injection_mode == 3 && !mode3_started && all_barrier_ready) begin
            measurement_start_cycle = live_cyc + 1;
            cmodel_perf_begin(longint'(measurement_start_cycle));
            perf_measure_en <= 1'b1;
            mode3_start <= 1'b1;
            mode3_started <= 1'b1;
        end else if (injection_mode == 3 && mode3_started && !mode3_ended &&
                     all_compare_done) begin
            measurement_end_cycle = 0;
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                if (compare_background[i] &&
                    stimulus_done_cycle[i] > measurement_end_cycle)
                    measurement_end_cycle = stimulus_done_cycle[i];
            end
            perf_measure_en <= 1'b0;
            mode3_ended <= 1'b1;
            #1ps cmodel_perf_end(longint'(measurement_end_cycle));
        end else if (injection_mode != 3 && injection_mode != 4 && !perf_measure_en) begin
            cmodel_perf_begin(0);
            perf_measure_en <= 1'b1;
        end else if (injection_mode == 4 && !mode4_start && all_barrier_ready && any_active_source) begin
            measurement_start_cycle = live_cyc + 1;
            cmodel_perf_begin(longint'(measurement_start_cycle));
            perf_measure_en <= 1'b1;
            mode4_start <= 1'b1;
        end else if (injection_mode == 4 && !mode4_ended && all_active_sources_done) begin
            measurement_end_cycle = live_cyc;
            perf_measure_en <= 1'b0;
            mode4_ended <= 1'b1;
            #1ps cmodel_perf_end(longint'(measurement_end_cycle));
            $display("[MeasurementWindow] start_cycle=%0d completion_cycle=%0d",
                     measurement_start_cycle, measurement_end_cycle);
        end
    end

    final begin
        cmodel_perf_set_run(perf_scn, longint'(perf_cycle));
        cmodel_perf_dump(perf_out_path);
        cmodel_finalize();
    end

    // FSDB waveform dump (VCS only; +define+FSDB_DUMP). tb_top is the enclosing
    // testbench module, reached by the upward name resolution every checked-in
    // testbench keeps available by carrying that name.
`ifdef FSDB_DUMP
    initial begin
        string fsdb_path;
        if (!$value$plusargs("fsdb=%s", fsdb_path))
            fsdb_path = "dump.fsdb";
        $fsdbDumpfile(fsdb_path);
        $fsdbDumpvars(0, tb_top);
    end
`endif

    // -------------------------------------------------------------------------
    // Exit logic - non-vacuous PASS guard
    // -------------------------------------------------------------------------
    localparam int unsigned SETTLE_CYCLES = 100;
    bit round_perf = 1'b0;
    initial void'($value$plusargs("round_perf=%d", round_perf));
    initial begin
        bit vacuous;
        bit all_done;
        int unsigned expected_total;
        int unsigned active_sources;
        int unsigned write_bursts;
        longint unsigned round_start;
        longint unsigned round_completion;
        int unsigned aw_idle, aw_same, aw_alloc, ar_idle, ar_same, ar_alloc;
        int unsigned list_hwm, wtxn_hwm, rtxn_hwm;
        int unsigned nsu_write_hwm, nsu_read_hwm;
        int unsigned background_nodes;
        bit all_completion_at_barrier;
        // clock-polled (not wait()): end_of_sim is driven through port
        // aliases; Verilator --timing wait() on it does not wake reliably.
        do begin
            @(posedge clk_i);
            all_done = rst_ni;
            for (int i = 0; i < NUM_ENDPOINTS; i++)
                all_done &= injection_mode == 3 ? compare_done[i] : end_of_sim[i];
        end while (!all_done);
        if (injection_mode == 4 && !mode4_ended)
            $fatal(1, "tb_top: Mode 4 completed without closing the measurement window");
        if (injection_mode == 3) begin
            background_nodes = 0;
            round_completion = 0;
            all_completion_at_barrier = stimulus_done_cycle[0] == compare_barrier_cycle;
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                if (compare_background[i]) begin
                    background_nodes++;
                    if (stimulus_start_cycle[i] == 0 ||
                        stimulus_start_cycle[0] == 0 ||
                        stimulus_done_cycle[i] < stimulus_start_cycle[i] ||
                        stimulus_start_cycle[i] > stimulus_start_cycle[0] ||
                        stimulus_done_cycle[i] < stimulus_done_cycle[0])
                        $fatal(1, "tb_top: background node%0d interval does not contain Control", i);
                    if (stimulus_done_cycle[i] > round_completion)
                        round_completion = stimulus_done_cycle[i];
                    all_completion_at_barrier &=
                        stimulus_done_cycle[i] == compare_barrier_cycle;
                end
            end
            if (background_nodes == 0)
                $fatal(1, "tb_top: channel comparison has no Pipeline background");
            if (compare_barrier_cycle < round_completion || all_completion_at_barrier)
                $fatal(1, "tb_top: lifetime barrier replaced an actual ChannelCompare completion");
            $display("[ChannelCompareOverlap] control_node=0 background_nodes=%0d status=PASS",
                     background_nodes);
        end
        if (injection_mode == 4) begin
            round_completion = 0;
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                if (expected_txn_cnt[i] > 0) begin
                    if (stimulus_start_cycle[i] != measurement_start_cycle)
                        $fatal(1, "tb_top: Mode 4 sources did not start together");
                    if (stimulus_done_cycle[i] > round_completion)
                        round_completion = stimulus_done_cycle[i];
                end
            end
            if (measurement_end_cycle != round_completion)
                $fatal(1, "tb_top: Mode 4 measurement window missed final completion");
        end
        repeat (SETTLE_CYCLES) @(posedge clk_i);
        vacuous = 1'b0;
        expected_total = 0;
        for (int i = 0; i < NUM_ENDPOINTS; i++) begin
            expected_total += expected_txn_cnt[i];
            if (expected_txn_cnt[i] > 0 && txn_cnt[i] == 0) begin
                vacuous = 1'b1;
                $display("FAIL: node%0d completed zero of %0d loaded transactions",
                         i, expected_txn_cnt[i]);
            end
        end
        if (expected_total == 0) $fatal(1, "tb_top: empty stimulus");
        if (vacuous) $fatal(1, "tb_top: vacuous run");
        active_sources = 0;
        write_bursts = 0;
        if (injection_mode == 4) begin
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                if (expected_txn_cnt[i] > 0) active_sources++;
                write_bursts += expected_txn_cnt[i];
            end
        end else begin
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                if (expected_write_cnt[i] > 0) active_sources++;
                write_bursts += expected_write_cnt[i];
            end
        end
        if (injection_mode == 4)
            $display("[TrafficMeta] direction=%s axi_initiators=%0d source_requests=%0d",
                     traffic_direction, active_sources, write_bursts);
        else
            $display("[TrafficMeta] active_sources=%0d write_bursts=%0d",
                     active_sources, write_bursts);
        if (round_perf) begin
            if (injection_mode != 0)
                $fatal(1, "RoundPerf requires injection_mode=0");
            round_start = 0;
            round_completion = 0;
            active_sources = 0;
            for (int i = 0; i < NUM_ENDPOINTS; i++) begin
                if (expected_txn_cnt[i] > 0) begin
                    if (expected_txn_cnt[i] != expected_write_cnt[i])
                        $fatal(1, "RoundPerf requires write-only stimulus at node%0d", i);
                    if (active_sources == 0)
                        round_start = stimulus_start_cycle[i];
                    else if (stimulus_start_cycle[i] != round_start)
                        $fatal(1, "RoundPerf sources did not start together");
                    if (stimulus_done_cycle[i] > round_completion)
                        round_completion = stimulus_done_cycle[i];
                    active_sources++;
                end
            end
            $display("[RoundPerf] start_cycle=%0d completion_cycle=%0d round_cycles=%0d active_sources=%0d write_bursts=%0d",
                     round_start, round_completion, round_completion - round_start,
                     active_sources, write_bursts);
        end
        // Sizing statistics per node: RoB slot peak, the SPEC 17 admission
        // clause split, the per-id order-list peak and the shared-pool peaks.
        for (int i = 0; i < NUM_ENDPOINTS; i++) begin
            cmodel_nmu_admission_stats(nmu_ctx[i], aw_idle, aw_same, aw_alloc,
                                           ar_idle, ar_same, ar_alloc,
                                           list_hwm, wtxn_hwm, rtxn_hwm);
            $display("[HWM] node=%0d read_slot_hwm=%0d order_list_hwm=%0d write_txns_hwm=%0d read_txns_hwm=%0d aw_clause={idle=%0d same_dest=%0d alloc=%0d} ar_clause={idle=%0d same_dest=%0d alloc=%0d}",
                     i, cmodel_nmu_read_slot_hwm(nmu_ctx[i]),
                     list_hwm, wtxn_hwm, rtxn_hwm,
                     aw_idle, aw_same, aw_alloc, ar_idle, ar_same, ar_alloc);
            cmodel_nsu_meta_buffer_hwm(nsu_ctx[i], nsu_write_hwm, nsu_read_hwm);
            $display("[NSU_HWM] node=%0d write_meta_hwm=%0d read_meta_hwm=%0d",
                     i, nsu_write_hwm, nsu_read_hwm);
        end
        $display("PASS: all %0d nodes done, non-vacuous", NUM_ENDPOINTS);
        $finish(0);
    end

    // -------------------------------------------------------------------------
    // Centralized DPI error poll
    // -------------------------------------------------------------------------
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
                cmodel_finalize();
                $fatal(1, "tb_top: DPI error, simulation aborted");
            end
        end
        /* verilator lint_on WIDTHTRUNC */
    end

endmodule

`endif  // NOC_TB_TOP_SV
