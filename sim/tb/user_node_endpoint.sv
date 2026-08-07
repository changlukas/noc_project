// user_node_endpoint — per-node test endpoint: pulp axi_file_master on the
// master face, a taxi tile crossbar with two memory models on the slave face,
// plus an in-endpoint axi_scoreboard and a FlooNoC axi_bw_monitor. Bridges the
// fabric's flat ni_signals_pkg structs to interfaces with explicit per-field
// wiring (no protocol logic).
//
// Slave face (tile decode): the NSU's tile-local address selects one of the
// node's address spaces, config at 0x0 and memory above it, exactly the
// windows the c_model SAM rebases into. Two memory models, one per role, is
// deliberate. The data target keeps pulp axi_rand_slave for the three
// properties that historically surfaced fabric bugs: randomized backpressure
// and response delay, multiple outstanding with cross-ID selection, and X on
// unwritten addresses. The config target is a taxi_axi_ram, deterministic,
// dense, always-OKAY, single-outstanding, which is all a low-rate control path
// needs.
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
    parameter int unsigned ID_WIDTH     = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH   = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH   = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    // Tile crossbar windows, stamped by gen_tb_top.py from the topology YAML
    // (address_map.tile_layout). Port order and field packing are ONE coupled
    // invariant: field t is target t, m0 = config, LAST = data. Both targets
    // are base-agnostic -- taxi_axi_ram truncates the forwarded address to its
    // own ADDR_W (taxi_axi_ram.sv:145 write, :251 read) and axi_rand_slave is
    // address-agnostic -- so what the invariant protects is the ROLE-to-target
    // assignment, not any particular base. gen_tb_top.tile_targets() asserts
    // that order so an address_map.py SPACE_ORDER edit cannot transpose the two
    // silently.
    // Packed (not unpacked) arrays: Verilator 5.048 rejects an override
    // assignment pattern on an unpacked array param whose size depends on a
    // sibling param override (here TILE_TARGETS).
    parameter int unsigned TILE_TARGETS = 1,
    parameter logic [TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_BASE_ADDR = '0,
    parameter logic [TILE_TARGETS-1:0][31:0]           TILE_ADDR_W = '0,
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

    // ------------------------------------------------------------------
    // Slave face: NSU port -> taxi tile crossbar -> config RAM / data memory
    // ------------------------------------------------------------------
    // taxi's wr and rd are modports of ONE interface instance, so a single
    // taxi_axi_if feeds both halves of the crossbar and both halves of a
    // target. Field names already match the flat struct (taxi uses plain AXI
    // names), so this face is a straight rename-free forward.
    taxi_axi_if #(
        .DATA_W(DATA_WIDTH), .ADDR_W(ADDR_WIDTH), .ID_W(ID_WIDTH)
    ) tile_axi ();

    taxi_axi_if #(
        .DATA_W(DATA_WIDTH), .ADDR_W(ADDR_WIDTH), .ID_W(ID_WIDTH)
    ) target_axi [TILE_TARGETS] ();

    localparam int unsigned DATA_TARGET = TILE_TARGETS - 1;

    // Fault injection for the DECERR gate (standing red-test rule, same shape
    // as +mcast_fault above): +decerr_fault=1 sets the top address bit on this
    // node's inbound AR, which lands outside every crossbar window exactly as a
    // Python/C++ tile-layout divergence would. The crossbar answers RRESP =
    // DECERR and the master-face fatal below names it.
    localparam logic [ADDR_WIDTH-1:0] DECERR_FAULT_BIT = 1 << (ADDR_WIDTH - 1);
    bit decerr_fault = 1'b0;
    initial void'($value$plusargs("decerr_fault=%d", decerr_fault));

    assign tile_axi.awid     = slave_axi_req_i.awid;
    assign tile_axi.awaddr   = slave_axi_req_i.awaddr;
    assign tile_axi.awlen    = slave_axi_req_i.awlen;
    assign tile_axi.awsize   = slave_axi_req_i.awsize;
    assign tile_axi.awburst  = slave_axi_req_i.awburst;
    assign tile_axi.awlock   = slave_axi_req_i.awlock;
    assign tile_axi.awcache  = slave_axi_req_i.awcache;
    assign tile_axi.awprot   = slave_axi_req_i.awprot;
    assign tile_axi.awqos    = slave_axi_req_i.awqos;
    assign tile_axi.awregion = slave_axi_req_i.awregion;
    assign tile_axi.awuser   = '0;
    assign tile_axi.awvalid  = slave_axi_req_i.awvalid;
    assign tile_axi.wdata    = slave_axi_req_i.wdata;
    assign tile_axi.wstrb    = slave_axi_req_i.wstrb;
    assign tile_axi.wlast    = slave_axi_req_i.wlast;
    assign tile_axi.wuser    = '0;
    assign tile_axi.wvalid   = slave_axi_req_i.wvalid;
    assign tile_axi.bready   = slave_axi_req_i.bready;
    assign tile_axi.arid     = slave_axi_req_i.arid;
    assign tile_axi.araddr   = slave_axi_req_i.araddr | (decerr_fault ? DECERR_FAULT_BIT : '0);
    assign tile_axi.arlen    = slave_axi_req_i.arlen;
    assign tile_axi.arsize   = slave_axi_req_i.arsize;
    assign tile_axi.arburst  = slave_axi_req_i.arburst;
    assign tile_axi.arlock   = slave_axi_req_i.arlock;
    assign tile_axi.arcache  = slave_axi_req_i.arcache;
    assign tile_axi.arprot   = slave_axi_req_i.arprot;
    assign tile_axi.arqos    = slave_axi_req_i.arqos;
    assign tile_axi.arregion = slave_axi_req_i.arregion;
    assign tile_axi.aruser   = '0;
    assign tile_axi.arvalid  = slave_axi_req_i.arvalid;
    assign tile_axi.rready   = slave_axi_req_i.rready;
    always_comb begin
        slave_axi_rsp_o = '0;
        slave_axi_rsp_o.awready = tile_axi.awready;
        slave_axi_rsp_o.wready  = tile_axi.wready;
        slave_axi_rsp_o.bid     = tile_axi.bid;
        slave_axi_rsp_o.bresp   = tile_axi.bresp;
        slave_axi_rsp_o.bvalid  = tile_axi.bvalid;
        slave_axi_rsp_o.arready = tile_axi.arready;
        slave_axi_rsp_o.rid     = tile_axi.rid;
        slave_axi_rsp_o.rdata   = tile_axi.rdata;
        slave_axi_rsp_o.rresp   = tile_axi.rresp;
        slave_axi_rsp_o.rlast   = tile_axi.rlast;
        slave_axi_rsp_o.rvalid  = tile_axi.rvalid;
    end

    // Crossbar sizing. These are testbench limits, provisioned generously so
    // none of them becomes the bottleneck: the pressure is supposed to come
    // from rand_slave's randomized delays. S_ACCEPT 64 is total accepted
    // transactions at the tile port -- one NMU's pool is 32, but under hotspot
    // every node targets one tile, so 32 would throttle; overflow stalls, it
    // never errors. S_THREADS 8 is concurrent unique IDs and cannot exceed
    // S_ACCEPT (the RTL clamps and warns); today NSU_META_BUFFER_MAX_UNIQUE_IDS
    // collapses everything onto one id, so it never binds. M_ISSUE 32 is the
    // per-target in-flight limit -- deliberately NOT 1 on the config port: the
    // RAM backpressures itself and the crossbar should not second-guess a
    // target. M_*_REG_TYPE is left alone: taxi_axi_crossbar_1s declares those
    // parameters but never forwards them, so the lower defaults (AW/AR simple
    // buffer, W skid, B/R bypass) stand and setting them would have no effect.
    localparam int unsigned XBAR_S_ACCEPT  = 64;
    localparam int unsigned XBAR_S_THREADS = 8;
    localparam int unsigned XBAR_M_ISSUE   = 32;

    taxi_axi_crossbar_1s #(
        .M_COUNT(TILE_TARGETS),
        .ADDR_W(ADDR_WIDTH),
        .S_THREADS(XBAR_S_THREADS),
        .S_ACCEPT(XBAR_S_ACCEPT),
        .M_REGIONS(1),
        .M_BASE_ADDR(TILE_BASE_ADDR),
        .M_ADDR_W(TILE_ADDR_W),
        .M_ISSUE({TILE_TARGETS{XBAR_M_ISSUE[31:0]}}),
        .M_SECURE({TILE_TARGETS{1'b0}})
    ) u_tile_xbar (
        .clk(clk_i), .rst(!rst_ni),
        .s_axi_wr(tile_axi),   .s_axi_rd(tile_axi),
        .m_axi_wr(target_axi), .m_axi_rd(target_axi)
    );

    // At TILE_TARGETS = 1 the stamped array is all-zero, and taxi reads a zero
    // M_BASE_ADDR as "use auto-addressing" (taxi_axi_crossbar_addr.sv:135), so
    // the window in force there is calcBaseAddrs()'s, not the emitted array.
    // Same answer for one region at base 0, but do not read the array as
    // authoritative in that shape.

    // Config target (m0). ADDR_W is the CONFIG REGION width, never the system
    // width: taxi_axi_ram's mem is a dense 2**(ADDR_W-$clog2(STRB_W)) array, so
    // the 4 KB window is 64 x 512 b. Present only when the topology gives this
    // node a config space; with one space the memory target is m0 instead.
    if (TILE_TARGETS > 1) begin : g_config_ram
        taxi_axi_ram #(.ADDR_W(int'(TILE_ADDR_W[0]))) u_config_ram (
            .clk(clk_i), .rst(!rst_ni),
            .s_axi_wr(target_axi[0]), .s_axi_rd(target_axi[0])
        );
    end

    // Data target -> slave_dv: the one permitted adapter, a per-field rename
    // between two AXI4 faces at identical widths (taxi `awid` vs pulp `aw_id`).
    // No protocol or width conversion, so its only failure mode is a mis-wired
    // field, which the master-face scoreboard catches on the first readback.
    // aw_atop / *_user have no taxi counterpart in this configuration.
    assign slave_dv.aw_id     = target_axi[DATA_TARGET].awid;
    assign slave_dv.aw_addr   = target_axi[DATA_TARGET].awaddr;
    assign slave_dv.aw_len    = target_axi[DATA_TARGET].awlen;
    assign slave_dv.aw_size   = target_axi[DATA_TARGET].awsize;
    assign slave_dv.aw_burst  = target_axi[DATA_TARGET].awburst;
    assign slave_dv.aw_lock   = target_axi[DATA_TARGET].awlock;
    assign slave_dv.aw_cache  = target_axi[DATA_TARGET].awcache;
    assign slave_dv.aw_prot   = target_axi[DATA_TARGET].awprot;
    assign slave_dv.aw_qos    = target_axi[DATA_TARGET].awqos;
    assign slave_dv.aw_region = target_axi[DATA_TARGET].awregion;
    assign slave_dv.aw_atop   = '0;
    assign slave_dv.aw_user   = '0;
    assign slave_dv.aw_valid  = target_axi[DATA_TARGET].awvalid;
    assign slave_dv.w_data    = target_axi[DATA_TARGET].wdata;
    assign slave_dv.w_strb    = target_axi[DATA_TARGET].wstrb;
    assign slave_dv.w_last    = target_axi[DATA_TARGET].wlast;
    assign slave_dv.w_user    = '0;
    assign slave_dv.w_valid   = target_axi[DATA_TARGET].wvalid;
    assign slave_dv.b_ready   = target_axi[DATA_TARGET].bready;
    assign slave_dv.ar_id     = target_axi[DATA_TARGET].arid;
    assign slave_dv.ar_addr   = target_axi[DATA_TARGET].araddr;
    assign slave_dv.ar_len    = target_axi[DATA_TARGET].arlen;
    assign slave_dv.ar_size   = target_axi[DATA_TARGET].arsize;
    assign slave_dv.ar_burst  = target_axi[DATA_TARGET].arburst;
    assign slave_dv.ar_lock   = target_axi[DATA_TARGET].arlock;
    assign slave_dv.ar_cache  = target_axi[DATA_TARGET].arcache;
    assign slave_dv.ar_prot   = target_axi[DATA_TARGET].arprot;
    assign slave_dv.ar_qos    = target_axi[DATA_TARGET].arqos;
    assign slave_dv.ar_region = target_axi[DATA_TARGET].arregion;
    assign slave_dv.ar_user   = '0;
    assign slave_dv.ar_valid  = target_axi[DATA_TARGET].arvalid;
    assign slave_dv.r_ready   = target_axi[DATA_TARGET].rready;

    assign target_axi[DATA_TARGET].awready = slave_dv.aw_ready;
    assign target_axi[DATA_TARGET].wready  = slave_dv.w_ready;
    assign target_axi[DATA_TARGET].bid     = slave_dv.b_id;
    assign target_axi[DATA_TARGET].bresp   = slave_dv.b_resp;
    assign target_axi[DATA_TARGET].buser   = '0;
    assign target_axi[DATA_TARGET].bvalid  = slave_dv.b_valid;
    assign target_axi[DATA_TARGET].arready = slave_dv.ar_ready;
    assign target_axi[DATA_TARGET].rid     = slave_dv.r_id;
    assign target_axi[DATA_TARGET].rdata   = slave_dv.r_data;
    assign target_axi[DATA_TARGET].rresp   = slave_dv.r_resp;
    assign target_axi[DATA_TARGET].rlast   = slave_dv.r_last;
    assign target_axi[DATA_TARGET].ruser   = '0;
    assign target_axi[DATA_TARGET].rvalid  = slave_dv.r_valid;

    // ------------------------------------------------------------------
    // VIP classes
    // ------------------------------------------------------------------
    typedef axi_test::axi_file_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(ID_WIDTH), .UW(AWUSER_WIDTH),
        .TA(ApplTime), .TT(TestTime)
    ) file_master_t;
    // Zero response wait: an ideal sink so the FABRIC is the bottleneck, not the
    // slave. It now sits behind the tile crossbar, whose master-port AW/AR
    // simple buffer and W skid (taxi defaults, not settable through the _1s
    // wrapper) add a cycle or two and smooth the randomized backpressure a
    // little before it reaches the NSU. The pulp default
    // (AX_MAX_WAIT_CYCLES=100, RESP=20, R=5) throttles
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

    // The one "not an error response" predicate, {OKAY, EXOKAY}, the set pulp
    // uses (axi_test.sv:2133-2134). Both readers below call it -- the RRESP
    // fatal and the multicast replica compare -- so the two can never disagree
    // about a beat. EXOKAY is unreachable today: no pattern issues exclusive
    // accesses.
    function automatic bit resp_ok(input logic [1:0] resp);
        return resp inside {axi_pkg::RESP_OKAY, axi_pkg::RESP_EXOKAY};
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

    // RRESP twin of the BRESP fatal above, and the read half of the tile-layout
    // gate: the tile crossbar DECERRs any address outside every window, so a
    // disagreement between the SAM's space_base and the generated
    // TILE_BASE_ADDR / TILE_ADDR_W surfaces here by name instead of as an
    // unexplained data mismatch. +decerr_fault=1 proves it fires.
    //
    // Nothing else checked RRESP in ANY mode: pulp's RRespCheck asserts only
    // r_id and r_last (axi_test.sv:2154-2157), and its read-data compare is
    // SKIPPED when r_resp leaves {OKAY, EXOKAY} (:2133-2134), so an error
    // response silently passed the scoreboard rather than failing it.
    //
    // What this gate covers, precisely: an address that matches NO window. Two
    // layout-divergence shapes escape it and need the model-side checks
    // instead. A config/memory transposition only DECERRs its data half --
    // config traffic at 0x100000+off lands inside the memory window, where the
    // write and its readback agree. And a config access overrunning its SAM
    // entry falls into the NEXT entry, routes to a different node's config RAM,
    // rebases to a legal offset there, and also agrees; that one is held off
    // upstream by gen_test_patterns' probe-window guard.
    always_ff @(posedge clk_i) begin
        if (rst_ni && master_axi_rsp_i.rvalid && master_axi_req_o.rready &&
                !resp_ok(master_axi_rsp_i.rresp))
            $fatal(1, "[tile_decode] node%0d: RRESP=%0h on id=%0h, expected OKAY (address outside every tile window?)",
                   NODE_ID, master_axi_rsp_i.rresp, master_axi_rsp_i.rid);
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
                // An error response carries no read data, so comparing it would
                // report a decode failure as a data mismatch. The RRESP fatal
                // above is what actually reports the error.
                if (resp_ok(master_axi_rsp_i.rresp)) begin
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
