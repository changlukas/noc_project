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
    parameter int unsigned DEFAULT_NUM_WRITES = 8,
    // AWUSER width (see nmu_wrap.sv AWUSER_WIDTH). Master-side DV interfaces
    // carry it (stimulus user field = AWUSER); the flat axi_req_t struct has no
    // awuser member, so it leaves on the dedicated port below.
    parameter int unsigned AWUSER_WIDTH = ni_params_pkg::AXI_AWUSER_WIDTH_DFLT
) (
    input  logic                       clk_i,
    input  logic                       rst_ni,
    output ni_signals_pkg::axi_req_t   master_axi_req_o,
    output logic [AWUSER_WIDTH-1:0]    master_awuser_o,
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
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) master_dv (clk_i);

    // Same AXI_USER_WIDTH as master_dv (user bits tied 0 on this face):
    // mixed user widths would create two axi_test class specializations,
    // and the file_master's class-scope beat typedefs mis-resolve (v5.048)
    // when more than one axi_driver specialization exists.
    AXI_BUS_DV #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(AWUSER_WIDTH)
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
    // AWUSER sideband (58 b, collective op + address mask): the file_master's
    // stimulus user field, forwarded whole to the NMU beside the flat struct.
    assign master_awuser_o = master_dv.aw_user;
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
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(AWUSER_WIDTH),
        .TA(ApplTime), .TT(TestTime)
    ) file_master_t;
    // Zero response wait: an ideal sink so the FABRIC is the bottleneck, not the
    // slave. The pulp default (AX_MAX_WAIT_CYCLES=100, RESP=20, R=5) throttles
    // responses and hides fabric saturation (measured util ~1.2% at greedy
    // injection). Standard NoC-eval practice (booksim2 consumes at the sink).
    // The directed two-phase run is a data-integrity gate (scoreboard compares
    // read data vs golden, timing-independent), so a fast slave keeps it passing.
    typedef axi_test::axi_rand_slave #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(AWUSER_WIDTH),
        .TA(ApplTime), .TT(TestTime), .MAPPED(1'b1),
        .AX_MAX_WAIT_CYCLES(0), .R_MAX_WAIT_CYCLES(0), .RESP_MAX_WAIT_CYCLES(0)
    ) rand_slave_t;
    typedef axi_test::axi_scoreboard #(
        .IW(ID_WIDTH), .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .UW(AWUSER_WIDTH), .TT(TestTime)
    ) scoreboard_t;

    // Preload-capable scoreboard: a multicast replica is written by a REMOTE
    // node's fabric-replicated AW, so it never crosses this node's master
    // AW/W wires and the base class golden model cannot learn it from
    // monitoring -- its read check then flags the (correct) replica readback
    // as unexpected. preload_byte() seeds the protected golden model
    // directly; the multicast checker below calls it for every replica byte
    // it captures, after which the base class checks replica reads exactly
    // like locally written ones.
    class mcast_preload_scoreboard extends scoreboard_t;
        function new(
            virtual AXI_BUS_DV #(
                .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
                .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(AWUSER_WIDTH)
            ) axi
        );
            super.new(axi);
        endfunction
        function void preload_byte(axi_addr_t addr, byte_t data);
            this.memory_q[addr].push_back(data);
        endfunction
    endclass

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
    mcast_preload_scoreboard scoreboard;

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
        // Mode 1 interleaves reads and writes with no pairing, so a read may
        // precede the write to its address and the scoreboard's
        // write-before-read precondition fails. Modes 0 and 2 order every read
        // after its write (phase barrier / per-pair B interlock), so both arm.
        // In mode 1 skip BOTH enable_all_checks() and monitor(): construction
        // hooks nothing, but monitor() forks the sampling tasks and mutates the
        // memory model even with checks off.
        if (get_injection_mode() != 1) begin
            scoreboard.enable_all_checks();
            scoreboard.monitor();
        end
    end

    // Continuous-injection pacing, shared by modes 1 and 2 (mode selection is
    // the case dispatch in the stimulus initial below). Pacing uses
    // $urandom_range (PRNG, no constraint solver => no z3): one Bernoulli
    // trial per cycle at p = injection_rate (booksim2 injection process).
    // Paced copies of run_aw/run_ar: same body as axi_test.sv:2540-2565 plus a
    // per-cycle idle before each send.
    real injection_rate;
    int  unsigned injection_rate_pct;

    task automatic run_aw_paced();
        while (file_master.aw_queue.size() > 0) begin
            while ($urandom_range(0, 99) >= injection_rate_pct) @(posedge clk_i);
            file_master.drv.send_aw(file_master.aw_queue[0]);
            void'(file_master.aw_queue.pop_front());
        end
    endtask

    task automatic run_ar_paced();
        while (file_master.ar_queue.size() > 0) begin
            while ($urandom_range(0, 99) >= injection_rate_pct) @(posedge clk_i);
            file_master.drv.send_ar(file_master.ar_queue[0]);
            void'(file_master.ar_queue.pop_front());
        end
    endtask

    // Mode-2 interlock state: B responses returned per AXI id, snooped off the
    // flat wires (same sampling pattern as txn_cnt_o). Per-id, not total: with
    // stimulus ids_per_tile > 1, B responses reorder across ids; within one id
    // AXI returns B in AW issue order, so a per-id count identifies the paired
    // write's B exactly.
    int unsigned b_returned[2**ID_WIDTH];

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            b_returned <= '{default: '0};
        end else if (master_axi_rsp_i.bvalid && master_axi_req_o.bready) begin
            b_returned[master_axi_rsp_i.bid] <= b_returned[master_axi_rsp_i.bid] + 1;
            // Directed runs use an always-OKAY MAPPED slave, so any error
            // response here is a fabric bug (e.g. a corrupted merged B).
            if (master_axi_rsp_i.bresp != axi_pkg::RESP_OKAY)
                $fatal(1, "[mcast_sb] node%0d: BRESP=%0h on id=%0h, expected OKAY",
                       NODE_ID, master_axi_rsp_i.bresp, master_axi_rsp_i.bid);
        end
    end

    // Debug handshake trace: +hs_trace_node=<id> dumps per-cycle AW/W/B
    // valid/ready pairs of that node's master port to hs_trace_node<id>.log.
    // Off unless the plusarg names this node.
    int unsigned hs_trace_node;
    int hs_fd = 0;
    int unsigned hs_cyc = 0;
    initial begin
        if ($value$plusargs("hs_trace_node=%d", hs_trace_node) && hs_trace_node == NODE_ID)
            hs_fd = $fopen($sformatf("hs_trace_node%0d.log", NODE_ID), "w");
    end
    always_ff @(posedge clk_i) begin
        hs_cyc <= hs_cyc + 1;
        if (hs_fd != 0 && rst_ni)
            $fdisplay(hs_fd, "%0d %b%b %b%b %b%b", hs_cyc,
                      master_axi_req_o.awvalid, master_axi_rsp_i.awready,
                      master_axi_req_o.wvalid,  master_axi_rsp_i.wready,
                      master_axi_rsp_i.bvalid,  master_axi_req_o.bready);
    end

    // ------------------------------------------------------------------
    // Multicast scoreboard (S4 collectives): replica golden + readback
    // compare, keyed by full byte address (== (dst_id, local_addr): the tile
    // base encodes dst_id, the offset is the node-local address). The pulp
    // scoreboard models only the ANCHOR address of a multicast AW; the other
    // replicas land at addresses it never saw written, which its x-wildcard
    // read check accepts vacuously. This checker snoops AW/W to build golden
    // for EVERY member replica (enumerated from the AWUSER address mask) and
    // AR/R to compare the readback. Idle in runs without collective writes.
    // +mcast_fault=1 XORs 0x01 into the captured golden (fault injection:
    // proves the compare fires; standing red-test rule).
    // ------------------------------------------------------------------
    localparam int unsigned MC_BUS_SIZE  = $clog2(DATA_WIDTH / 8);
    localparam int unsigned MC_BUS_BYTES = DATA_WIDTH / 8;

    typedef struct {
        logic [ADDR_WIDTH-1:0] addr;
        logic [7:0]            len;
        logic [2:0]            size;
        longint unsigned       mask;  // AWUSER address mask; 0 = unicast
    } mcast_txn_t;

    bit mcast_fault = 1'b0;
    initial void'($value$plusargs("mcast_fault=%d", mcast_fault));

    logic [7:0]  mcast_mem [longint unsigned];       // replica byte golden
    mcast_txn_t  mcast_wr_q [$];                     // W bursts follow AW order (AXI4)
    int unsigned mcast_wr_beat = 0;
    mcast_txn_t  mcast_ar_q [2**ID_WIDTH][$];        // same-id R follows AR order
    mcast_txn_t  mcast_rd_active [2**ID_WIDTH];
    bit          mcast_rd_busy [2**ID_WIDTH];
    int unsigned mcast_rd_beat [2**ID_WIDTH];
    // Non-vacuity: replica bytes actually compared. A node that captured
    // multicast golden must also have compared some readback, or the check
    // never ran (see the epilogue below).
    int unsigned mcast_checked = 0;

    // Plain always + blocking assignments: this is testbench bookkeeping
    // (queues + associative array), not registered hardware state.
    always @(posedge clk_i) begin
        if (rst_ni) begin
            // AW: descriptor capture (unicast too -- the W association below
            // must walk every burst in AW order).
            if (master_axi_req_o.awvalid && master_axi_rsp_i.awready) begin
                mcast_wr_q.push_back('{addr: master_axi_req_o.awaddr,
                                       len:  master_axi_req_o.awlen,
                                       size: master_axi_req_o.awsize,
                                       mask: (master_awuser_o[9:8] == 2'd1)
                                             ? longint'(master_awuser_o[57:10]) : 0});
            end
            // W: golden capture for every member replica of a multicast burst.
            if (master_axi_req_o.wvalid && master_axi_rsp_i.wready) begin
                if (mcast_wr_q.size() == 0)
                    $fatal(1, "[mcast_sb] node%0d: W beat with no open AW", NODE_ID);
                if (mcast_wr_q[0].mask != 0) begin
                    automatic longint unsigned mc_mask = mcast_wr_q[0].mask;
                    automatic longint unsigned bus_addr = longint'(axi_pkg::aligned_addr(
                        axi_pkg::beat_addr(axi_pkg::largest_addr_t'(mcast_wr_q[0].addr),
                                           mcast_wr_q[0].size, mcast_wr_q[0].len,
                                           axi_pkg::BURST_INCR,
                                           shortint'(mcast_wr_beat)), MC_BUS_SIZE));
                    for (int unsigned j = 0; j < MC_BUS_BYTES; j++) begin
                        if (master_axi_req_o.wstrb[j]) begin
                            automatic longint unsigned byte_addr = bus_addr + j;
                            automatic logic [7:0] golden =
                                master_axi_req_o.wdata[8*j +: 8] ^ (mcast_fault ? 8'h01 : 8'h00);
                            automatic longint unsigned sub = mc_mask;
                            automatic bit done = 1'b0;
                            while (!done) begin
                                mcast_mem[(byte_addr & ~mc_mask) | sub] = golden;
                                // Seed the pulp scoreboard too: replicas at
                                // remote nodes never appear on this node's
                                // master wires, so without this its read
                                // check has no golden for the replica
                                // readback (under +mcast_fault the corrupted
                                // byte flows here as well -- both checkers
                                // then flag, which is the red test's point).
                                scoreboard.preload_byte((byte_addr & ~mc_mask) | sub, golden);
                                if (sub == 0) done = 1'b1;
                                else sub = (sub - 1) & mc_mask;
                            end
                        end
                    end
                end
                if (master_axi_req_o.wlast) begin
                    void'(mcast_wr_q.pop_front());
                    mcast_wr_beat = 0;
                end else begin
                    mcast_wr_beat = mcast_wr_beat + 1;
                end
            end
            // AR: read descriptor capture, per id.
            if (master_axi_req_o.arvalid && master_axi_rsp_i.arready) begin
                mcast_ar_q[master_axi_req_o.arid].push_back('{addr: master_axi_req_o.araddr,
                                                              len:  master_axi_req_o.arlen,
                                                              size: master_axi_req_o.arsize,
                                                              mask: 0});
            end
            // R: compare any byte the multicast golden knows.
            if (master_axi_rsp_i.rvalid && master_axi_req_o.rready) begin
                automatic int unsigned rid = 32'(master_axi_rsp_i.rid);
                automatic longint unsigned beat_address;
                automatic int unsigned first_byte;
                if (!mcast_rd_busy[rid]) begin
                    if (mcast_ar_q[rid].size() == 0)
                        $fatal(1, "[mcast_sb] node%0d: R beat with no open AR (id=%0d)",
                               NODE_ID, rid);
                    mcast_rd_active[rid] = mcast_ar_q[rid].pop_front();
                    mcast_rd_beat[rid]   = 0;
                    mcast_rd_busy[rid]   = 1'b1;
                end
                beat_address = longint'(axi_pkg::aligned_addr(
                    axi_pkg::beat_addr(axi_pkg::largest_addr_t'(mcast_rd_active[rid].addr),
                                       mcast_rd_active[rid].size, mcast_rd_active[rid].len,
                                       axi_pkg::BURST_INCR,
                                       shortint'(mcast_rd_beat[rid])),
                    mcast_rd_active[rid].size));
                first_byte = (mcast_rd_beat[rid] == 0)
                    ? int'(mcast_rd_active[rid].addr - beat_address) : 0;
                for (int unsigned j = first_byte;
                     j < axi_pkg::num_bytes(mcast_rd_active[rid].size); j++) begin
                    automatic longint unsigned ba = beat_address + j;
                    if (mcast_mem.exists(ba)) begin
                        automatic logic [7:0] act =
                            master_axi_rsp_i.rdata[8 * (ba % MC_BUS_BYTES) +: 8];
                        mcast_checked = mcast_checked + 1;
                        if (act !== mcast_mem[ba])
                            $fatal(1, "[mcast_sb] node%0d: replica readback mismatch addr=%h exp=%h act=%h (id=%0d beat=%0d)",
                                   NODE_ID, ba, mcast_mem[ba], act, rid, mcast_rd_beat[rid]);
                    end
                end
                if (master_axi_rsp_i.rlast) mcast_rd_busy[rid] = 1'b0;
                else mcast_rd_beat[rid] = mcast_rd_beat[rid] + 1;
            end
        end
    end

    // Mode 2: read i issues only after write i's B returns. Pair i is
    // transaction i of write.txt/read.txt (same id, same address), so the
    // paired write is the issued[id]-th write with that id and
    // b_returned[id] >= issued[id] is the exact release condition.
    task automatic run_ar_after_b();
        int unsigned issued[2**ID_WIDTH];
        int unsigned id;
        issued = '{default: '0};
        while (file_master.ar_queue.size() > 0) begin
            id = 32'(file_master.ar_queue[0].ax_id);
            issued[id] += 1;
            while (b_returned[id] < issued[id]) @(posedge clk_i);
            file_master.drv.send_ar(file_master.ar_queue[0]);
            void'(file_master.ar_queue.pop_front());
        end
    endtask

    // Stimulus driver, one shape per +injection_mode. load_files() fills the
    // queues (do NOT call run(): it re-forks all five and double-consumes the
    // queues, spec Two-phase). join (not join_none) everywhere so B/R are
    // consumed and the pass terminates cleanly.
    initial begin
        void'($value$plusargs("stim_dir=%s", stim_dir));
        write_path = $sformatf("%s/node%0d/write.txt", stim_dir, NODE_ID);
        read_path  = $sformatf("%s/node%0d/read.txt",  stim_dir, NODE_ID);
        file_master = new(master_dv);
        file_master.load_files(read_path, write_path);
        injection_rate = 1.0;
        void'($value$plusargs("injection_rate=%f", injection_rate));
        injection_rate_pct = int'(injection_rate * 100.0);
        @(posedge rst_ni);
        case (get_injection_mode())
            0: begin
                // Directed two-phase: phase 1 drains all writes (wait_b =>
                // committed at the slave); phase 2 issues reads, checked by the
                // scoreboard against golden.
                fork file_master.run_aw(); file_master.run_w(); file_master.wait_b(); join
                fork file_master.run_ar(); file_master.wait_r(); join
            end
            1: begin
                // Continuous: one phase, reads and writes interleaved, each
                // send paced per cycle on injection_rate.
                fork
                    run_aw_paced();
                    file_master.run_w();
                    run_ar_paced();
                    file_master.wait_b();
                    file_master.wait_r();
                join
            end
            2: begin
                // Checked-continuous: AW paced as mode 1; each read waits for
                // its paired write's B, so the armed scoreboard checks exact
                // data under continuous write load.
                fork
                    run_aw_paced();
                    file_master.run_w();
                    run_ar_after_b();
                    file_master.wait_b();
                    file_master.wait_r();
                join
            end
            default: begin
                $fatal(1, "unknown +injection_mode=%0d (0, 1, 2 supported)",
                    get_injection_mode());
            end
        endcase
        run_done = 1'b1;
        // Single-merged-B invariant (S4 collectives): exactly one B reached
        // the initiator per issued AW -- a multicast AW's member Bs must have
        // merged in the fabric. Checked wire-side: total B handshakes equals
        // the AW count, and no extra B is still pending after a settle window
        // (a duplicate B past wait_b() has no consumer, so it parks as a held
        // bvalid). The model-side write_txns_ underflow assert is the deeper
        // net; this is the tb-visible half.
        begin
            int unsigned b_total;
            repeat (50) @(posedge clk_i);
            b_total = 0;
            for (int i = 0; i < 2**ID_WIDTH; i++) b_total += b_returned[i];
            if (b_total != int'(file_master.num_writes))
                $fatal(1, "[mcast_sb] node%0d: %0d B handshakes for %0d AWs -- duplicate or lost B",
                       NODE_ID, b_total, file_master.num_writes);
            if (master_axi_rsp_i.bvalid)
                $fatal(1, "[mcast_sb] node%0d: B still asserted after all writes retired -- extra B",
                       NODE_ID);
            // Non-vacuity: a node that captured multicast golden read its own
            // member replicas back (the pattern's readback phase), so zero
            // compares means the checker never saw the readback -- vacuous.
            if (mcast_mem.num() > 0 && mcast_checked == 0)
                $fatal(1, "[mcast_sb] node%0d: multicast golden captured but zero replica bytes compared",
                       NODE_ID);
            if (mcast_mem.num() > 0)
                $display("[mcast_sb] node%0d: %0d replica byte compares against %0d golden bytes",
                         NODE_ID, mcast_checked, mcast_mem.num());
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
