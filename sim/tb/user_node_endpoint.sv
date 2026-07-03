// user_node_endpoint — per-node test endpoint: pulp axi_rand_master +
// axi_rand_slave + FlooNoC axi_bw_monitor. Bridges the fabric's flat
// ni_signals_pkg structs to pulp AXI_BUS_DV interfaces with explicit per-field
// wiring (no protocol logic). Checking lives at tb level: one FlooNoC
// axi_reorder_compare per master (permutation pairing — master m targets only
// node NUM_NODES-1-m, so the compare can attribute streams). Single region per
// master is a compare precondition beyond attribution: the pulp master sends
// AW/W in parallel, so a W beat can handshake before its AW; the compare's
// w_slv_idx then reads an empty queue (default 0), which stays correct only
// while every W belongs to decode slot 0. pulp
// axi_scoreboard is withdrawn from the Verilator flow: its 8'hxx uninitialized
// -byte wildcard collapses to 8'h00 under 2-state simulation, turning every
// protocol-legal read/write race into a false mismatch (VCS-only, backlog).
//
// Run flavors (compile-time):
//   default            : data-integrity — MAPPED memory-model slave, INCR bursts.
//   +define+TB_TRANSPORT_RUN : transport — MAPPED=0 RAND_RESP=1, WRAP+EXC on.
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
    parameter int unsigned DEFAULT_NUM_WRITES = 8,
    // Reads single-outstanding: axi_rand_slave services outstanding ARs in
    // RANDOM id order (rand_id_queue), which breaks axi_reorder_compare's
    // in-AR-order slave-face R attribution when >1 read id is in flight at a
    // slave. B responses are issued in AW order (plain queue), so writes keep
    // full pipelining.
    parameter int unsigned MAX_READ_TXNS_IN_FLIGHT  = 1,
    parameter int unsigned MAX_WRITE_TXNS_IN_FLIGHT = 8
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

    // master face: rand_master drives master_dv; forward to the flat NMU port.
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
    // MAX_BURST_LEN: new_rand_burst randomizes len/size BEFORE addr; an
    // unconstrained len (256 beats x 32 B = 8 KiB) cannot fit a REGION_BYTES
    // window, making the addr constraint unsat. Cap beats so the largest
    // burst exactly fits the region.
    localparam int unsigned MAX_BURST_LEN = REGION_BYTES / (DATA_WIDTH / 8);
`ifdef TB_TRANSPORT_RUN
    typedef axi_test::axi_rand_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime),
        .MAX_READ_TXNS(MAX_READ_TXNS_IN_FLIGHT), .MAX_WRITE_TXNS(MAX_WRITE_TXNS_IN_FLIGHT),
        .AXI_MAX_BURST_LEN(MAX_BURST_LEN),
        .AXI_EXCLS(1'b1), .AXI_ATOPS(1'b0), .UNIQUE_IDS(1'b0),
        .AXI_BURST_FIXED(1'b1), .AXI_BURST_INCR(1'b1), .AXI_BURST_WRAP(1'b1)
    ) rand_master_t;
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b0), .RAND_RESP(1'b1)
    ) rand_slave_t;
`else
    typedef axi_test::axi_rand_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime),
        .MAX_READ_TXNS(MAX_READ_TXNS_IN_FLIGHT), .MAX_WRITE_TXNS(MAX_WRITE_TXNS_IN_FLIGHT),
        .AXI_MAX_BURST_LEN(MAX_BURST_LEN),
        .AXI_EXCLS(1'b0), .AXI_ATOPS(1'b0), .UNIQUE_IDS(1'b0),
        // INCR only per the data-integrity run-class definition; FIXED/WRAP
        // are exercised by the transport flavor.
        .AXI_BURST_FIXED(1'b0), .AXI_BURST_INCR(1'b1), .AXI_BURST_WRAP(1'b0)
    ) rand_master_t;
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(1),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b1)
    ) rand_slave_t;
`endif

    rand_master_t rand_master;
    rand_slave_t  rand_slave;

    int unsigned num_reads;
    int unsigned num_writes;
    // run_done is set by the stimulus initial (procedural, blocking); the
    // output port is refreshed through a clocked register because Verilator
    // does not reliably propagate a procedurally-assigned output-port variable
    // to the instantiating scope.
    logic run_done = 1'b0;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) end_of_sim_o <= 1'b0;
        else end_of_sim_o <= run_done;
    end

    initial begin
        num_reads  = DEFAULT_NUM_READS;
        num_writes = DEFAULT_NUM_WRITES;
        void'($value$plusargs("num_reads=%d", num_reads));
        void'($value$plusargs("num_writes=%d", num_writes));

        rand_master = new(master_dv);
        // Permutation pairing (both flavors): this master targets ONLY node
        // (NUM_NODES-1-NODE_ID) so the tb-level axi_reorder_compare can
        // attribute every slave-face handshake to exactly one master.
        rand_master.add_memory_region(
            REGION_BASE[NUM_NODES-1-NODE_ID],
            REGION_BASE[NUM_NODES-1-NODE_ID] + REGION_BYTES,
            axi_pkg::DEVICE_NONBUFFERABLE);
        rand_master.reset();
        @(posedge rst_ni);
        rand_master.run(num_reads, num_writes);
        run_done = 1'b1;
    end

    initial begin
        rand_slave = new(slave_dv);
        rand_slave.reset();
        @(posedge rst_ni);
        rand_slave.run();
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
        .Name($sformatf("node%0d.manager", NODE_ID))
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
