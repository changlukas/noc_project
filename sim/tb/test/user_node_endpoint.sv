// user_node_endpoint — per-node test endpoint: pulp axi_file_master on the
// master face, a pulp axi_xbar tile crossbar with a memory per space on the
// slave face, plus an in-endpoint axi_scoreboard and a FlooNoC axi_bw_monitor.
// Bridges the fabric's flat ni_signals_pkg structs to interfaces with explicit
// per-field wiring (no protocol logic).
//
// Slave face (tile decode): the NSU forwards the request's own address --
// nothing rebases anywhere -- so the crossbar decodes on this node's two
// windows exactly as the topology address_map placed them, config and memory
// at their own global bases. Each target is a pulp axi_sim_mem behind a pulp
// axi_delayer: storage and timing are separate modules, so the latency profile
// moves without touching the memory and a DRAM behavioural model can later
// replace the memory without touching the timing.
// pulp axi_scoreboard is usable on the Verilator directed axis: the 8'hxx->8'h00
// 2-state collapse only bites reads of never-written addresses, which a
// full-readback directed run never issues. Wired in-endpoint on master_dv.
//
// Run flavor: data integrity — axi_file_master two-phase (write -> barrier ->
// read) + in-endpoint axi_scoreboard on master_dv, axi_sim_mem as tile memory.
// Stimulus from <stim_dir>/node<ID>/{write,read}.txt (+stim_dir=).
//
// Plusargs: +num_reads=<n> +num_writes=<n> (per node, defaults below).

`ifndef USER_NODE_ENDPOINT_SV
`define USER_NODE_ENDPOINT_SV

`include "axi/assign.svh"

module user_node_endpoint #(
    parameter int unsigned NODE_ID      = 0,
    parameter int unsigned ID_WIDTH     = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH   = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH   = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    // THIS node's own crossbar windows, stamped by gen_tb_top.py from the
    // config file (address_map.node_windows). Port order and field packing
    // are ONE coupled invariant: field t is target t, m0 = config, LAST = data.
    // gen_tb_top.tile_targets() asserts that order so an address_map.py
    // SPACE_ORDER edit cannot transpose the two silently.
    // Packed (not unpacked) arrays: Verilator 5.048 rejects an override
    // assignment pattern on an unpacked array param whose size depends on a
    // sibling param override (here TILE_TARGETS).
    // No defaults: only the generator knows a topology's tile layout, so a
    // missing override is an elaboration error (IEEE 1800-2017 6.20.1) rather
    // than a silent fallback to some window set nothing ships.
    parameter int unsigned TILE_TARGETS,
    parameter logic [TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_BASE_ADDR,
    parameter logic [TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_SIZE,
    // Where a collective write is offset to so the crossbar routes it to the
    // NI. Derived from the address map (address_map.noc_egress_base), so every
    // node window sits below it by construction. See g_tile_xbar below.
    parameter logic [ADDR_WIDTH-1:0] NOC_EGRESS_BASE,
    // Tile-memory latency, one profile stamped by gen_tb_top.py from
    // _MEM_LATENCY. Input covers AW/W/AR, output covers B/R (axi_delayer.sv
    // splits its stream_delay instances exactly that way). The defaults are the
    // "ideal" profile: FIXED_DELAY 0 with no random stall takes
    // stream_delay's gen_pass_through branch, so the memory answers the cycle
    // it is asked and the FABRIC is the bottleneck -- standard NoC-eval
    // practice, booksim2 consumes at the sink.
    // STALL_RANDOM draws from lfsr_16bit, whose refill_way_bin is
    // $clog2(WIDTH)=4 bits, so a stalled handshake waits 0-15 cycles. The
    // watchdog in tb_top is sized off that bound.
    parameter bit          MEM_STALL_RANDOM_INPUT  = 1'b0,
    parameter bit          MEM_STALL_RANDOM_OUTPUT = 1'b0,
    parameter int unsigned MEM_FIXED_DELAY_INPUT   = 0,
    parameter int unsigned MEM_FIXED_DELAY_OUTPUT  = 0,
    // Master-face consumer backpressure, response side only. Same profile
    // mechanism and the same 0-15 cycle bound as the memory pair above; "ideal"
    // is (0, 0) and takes the same gen_pass_through branch.
    parameter bit          MST_STALL_RANDOM_OUTPUT = 1'b0,
    parameter int unsigned MST_FIXED_DELAY_OUTPUT  = 0,
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

    // Tile id widths, declared here because the master-face VIP below is sized
    // by them. Slave-port ID width is one initiator's own share of the field,
    // and the master-port width adds the $clog2(NoSlvPorts) index axi_xbar
    // appends to route responses back (axi/doc/axi_xbar.md:15, the master-port
    // index of AXI4 IHI 0022 A5.3.5). Neither follows ID_WIDTH: the tile's id
    // space is a property of its initiators, the NI's is a property of the NoC,
    // and i_noc_id_remap below converts between them -- the tile's is the WIDER
    // of the two.
    localparam int unsigned XBAR_SLV_PORTS = 2;
    localparam int unsigned XBAR_SLV_ID_W  = ni_params_pkg::AXI_INITIATOR_ID_WIDTH_DFLT;
    localparam int unsigned XBAR_MST_ID_W  = XBAR_SLV_ID_W + $clog2(XBAR_SLV_PORTS);

    // ------------------------------------------------------------------
    // DV interfaces + flat-struct bridging (explicit wiring, both faces)
    // ------------------------------------------------------------------
    // The master face is upstream of the crossbar and of the id remap, so its
    // id width is the tile initiator's, not the NI's.
    AXI_BUS_DV #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),   .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_SLV_ID_W),  .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) master_dv (clk_i);

    // Consumer backpressure. The file master never stalls its R channel --
    // wait_r consumes a beat whenever r_outst is non-empty (axi_test.sv) -- so
    // without this the NMU always sinks R, R never backs up into DAT, and the
    // dependency cycle noc-target-spec.md argues against cannot form. The
    // delayer sits where a slow compute engine would, between the master and
    // the tile crossbar, so a stall travels the crossbar to
    // master_axi_req_o.rready and back into the fabric.
    // Response side only: stalling AW/W/AR here is injection-rate control,
    // which INJECTION_MODE owns.
    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),   .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_SLV_ID_W),  .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) mst_pre_delay ();

    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),   .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_SLV_ID_W),  .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) mst_post_delay ();

    `AXI_ASSIGN(mst_pre_delay, master_dv)

    axi_delayer_intf #(
        .AXI_ID_WIDTH(XBAR_SLV_ID_W),
        .AXI_ADDR_WIDTH(ADDR_WIDTH),
        .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_USER_WIDTH(AWUSER_WIDTH),
        .STALL_RANDOM_INPUT(1'b0),
        .STALL_RANDOM_OUTPUT(MST_STALL_RANDOM_OUTPUT),
        .FIXED_DELAY_INPUT(0),
        .FIXED_DELAY_OUTPUT(MST_FIXED_DELAY_OUTPUT)
    ) i_mst_backpressure (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .slv(mst_pre_delay),
        .mst(mst_post_delay)
    );

    // Evidence the knob acts, reported once per node at $finish. Counts cycles
    // where the NMU has read data to hand over and this endpoint refuses it.
    // Zero by construction under "ideal", where the delayer is wires, so a run
    // that reports zero under "random" means the backpressure never engaged and
    // the run proves nothing about a stalling consumer.
    int unsigned mst_r_stall_cycles = 0;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) mst_r_stall_cycles <= 0;
        else if (master_axi_rsp_i.rvalid && !master_axi_req_o.rready)
            mst_r_stall_cycles <= mst_r_stall_cycles + 1;
    end
    final $display("[mst_bp] node%0d: R backpressure held %0d cycles",
                   NODE_ID, mst_r_stall_cycles);

    // Master face. The file_master's own traffic is what every in-endpoint
    // checker watches, so it is flattened once here and the checkers read that
    // view. It is NOT the NoC-bound port: a request this node addresses to its
    // own tile is answered by the crossbar and never leaves (see g_tile_xbar),
    // so master_axi_req_o below carries only the share that goes on the NoC.
    ni_signals_pkg::axi_req_t mst_flat_req;
    ni_signals_pkg::axi_rsp_t mst_flat_rsp;
    logic [AWUSER_WIDTH-1:0]  mst_flat_awuser;
    // The flat structs are the NI's types, so their id fields are ID_WIDTH
    // wide -- narrower than one initiator's share. The master face's four ids
    // therefore ride BESIDE the structs at XBAR_SLV_ID_W and the corresponding
    // struct fields stay unused (zero). Every id-bearing reader below takes
    // these instead.
    logic [XBAR_SLV_ID_W-1:0] mst_awid, mst_arid, mst_bid, mst_rid;

    assign mst_awid = mst_post_delay.aw_id;
    assign mst_arid = mst_post_delay.ar_id;

    always_comb begin
        mst_flat_req = '0;
        mst_flat_req.awaddr   = mst_post_delay.aw_addr;
        mst_flat_req.awlen    = mst_post_delay.aw_len;
        mst_flat_req.awsize   = mst_post_delay.aw_size;
        mst_flat_req.awburst  = mst_post_delay.aw_burst;
        mst_flat_req.awlock   = mst_post_delay.aw_lock;
        mst_flat_req.awcache  = mst_post_delay.aw_cache;
        mst_flat_req.awprot   = mst_post_delay.aw_prot;
        mst_flat_req.awqos    = mst_post_delay.aw_qos;
        mst_flat_req.awregion = mst_post_delay.aw_region;
        mst_flat_req.awvalid  = mst_post_delay.aw_valid;
        mst_flat_req.wdata    = mst_post_delay.w_data;
        mst_flat_req.wstrb    = mst_post_delay.w_strb;
        mst_flat_req.wlast    = mst_post_delay.w_last;
        mst_flat_req.wvalid   = mst_post_delay.w_valid;
        mst_flat_req.bready   = mst_post_delay.b_ready;
        mst_flat_req.araddr   = mst_post_delay.ar_addr;
        mst_flat_req.arlen    = mst_post_delay.ar_len;
        mst_flat_req.arsize   = mst_post_delay.ar_size;
        mst_flat_req.arburst  = mst_post_delay.ar_burst;
        mst_flat_req.arlock   = mst_post_delay.ar_lock;
        mst_flat_req.arcache  = mst_post_delay.ar_cache;
        mst_flat_req.arprot   = mst_post_delay.ar_prot;
        mst_flat_req.arqos    = mst_post_delay.ar_qos;
        mst_flat_req.arregion = mst_post_delay.ar_region;
        mst_flat_req.arvalid  = mst_post_delay.ar_valid;
        mst_flat_req.rready   = mst_post_delay.r_ready;
    end
    // AWUSER sideband (58 b, collective op + address mask): the file_master's
    // stimulus user field. It rides the crossbar to reach the NMU.
    assign mst_flat_awuser = mst_post_delay.aw_user;
    assign mst_post_delay.aw_ready = mst_flat_rsp.awready;
    assign mst_post_delay.w_ready  = mst_flat_rsp.wready;
    assign mst_post_delay.b_id     = mst_bid;
    assign mst_post_delay.b_resp   = mst_flat_rsp.bresp;
    assign mst_post_delay.b_user   = '0;
    assign mst_post_delay.b_valid  = mst_flat_rsp.bvalid;
    assign mst_post_delay.ar_ready = mst_flat_rsp.arready;
    assign mst_post_delay.r_id     = mst_rid;
    assign mst_post_delay.r_data   = mst_flat_rsp.rdata;
    assign mst_post_delay.r_resp   = mst_flat_rsp.rresp;
    assign mst_post_delay.r_last   = mst_flat_rsp.rlast;
    assign mst_post_delay.r_user   = '0;
    assign mst_post_delay.r_valid  = mst_flat_rsp.rvalid;
    // aw_atop / *_user driven by the class are dropped (out of scope).

    // ------------------------------------------------------------------
    // Tile crossbar: file_master + NSU in, config / data memory / NMU out
    // ------------------------------------------------------------------
    // Two initiators share one decoder, so a request this node addresses to its
    // own tile is answered here and never enters the fabric. Two rules, both
    // THIS node's windows:
    //
    //   s0  file_master   hit -> m0/m1     miss -> m2 (default) -> NMU -> NoC
    //   s1  NSU           hit -> m0/m1     miss -> DECERR (no default)
    //
    // s1 has no default on purpose: an address arriving from the fabric that is
    // not this node's means the fabric misrouted, and DECERR is the honest
    // answer -- that is what the +decerr_fault plusargs below exercise.
    //
    // A third rule covers the NoC egress aperture and points at m2. A collective
    // write names a SET of nodes, so "is this address mine" has no answer, but
    // the crossbar decodes addresses and nothing else -- and a collective
    // write whose address names this node's own region would be answered
    // locally and never reach the NI. s0 offsets such a write into the aperture
    // and m2 takes the offset back off, so the crossbar stays stock and the
    // address the NMU sees is the one the request named. The local replica
    // still arrives, the long way: the router
    // keeps LOCAL in a multicast's output set (route_mask.hpp:108-112), so the
    // fabric delivers this node's copy back through its own NSU.
    //
    // Connectivity is load-bearing here, not a backstop. That third rule is
    // reachable from s1 as well -- +decerr_fault sets the top address bit, which
    // lands inside the aperture -- and s1 reaching m2 would put a delivered
    // request back on the NoC. Forbidding s1 -> m2 sends it to the crossbar's
    // per-pair axi_err_slv instead (axi_xbar_unmuxed.sv:253-266), which is the
    // DECERR the fault test is looking for.
    localparam int unsigned DATA_TARGET    = TILE_TARGETS - 1;
    localparam int unsigned NMU_TARGET     = TILE_TARGETS;  // last master port
    localparam int unsigned XBAR_MST_PORTS = TILE_TARGETS + 1;

    // DESCENDING ranges, not [N]. axi_xbar_intf declares its ports
    // [NoSlvPorts-1:0] / [NoMstPorts-1:0] and SystemVerilog binds an interface
    // array port element-by-element in declared order, so an ascending [N]
    // instance array binds port 1 to element 0 -- silently transposing them.
    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),  .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_SLV_ID_W), .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) tile_axi [XBAR_SLV_PORTS-1:0] ();

    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),  .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_MST_ID_W), .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) tile_mst [XBAR_MST_PORTS-1:0] ();

    // m2 after the id remap: the NoC-facing face of the tile, at the NI's id
    // width. A tile initiator may drive 2**XBAR_SLV_ID_W ids and the crossbar
    // appends its index, so up to 2**XBAR_MST_ID_W distinct ids arrive here and
    // fold into the NI's space, 2**ID_WIDTH of them concurrently -- which is
    // what AXI_SLV_PORT_MAX_UNIQ_IDS below sizes the remap's tables for, since
    // the master port cannot encode more than that at once. axi_id_remap stalls
    // an id that finds no free downstream id rather than erroring (its own
    // AxiSlvPortMaxUniqIds doc, axi_id_remap.sv:39-41), and unlike
    // axi_id_serialize it never puts two distinct upstream ids on one
    // downstream id, so per-id ordering survives the fold.
    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) noc_mst ();

    // Fault injection for the DECERR gate (standing red-test rule, same shape
    // as +mcast_fault above): +decerr_fault=1 sets the top address bit on this
    // node's inbound AR, which lands outside both windows exactly as a
    // Python/C++ tile-layout divergence would. The crossbar routes it to its
    // internal axi_err_slv and the master-face fatal below names the RRESP.
    // The write twin, +decerr_fault_wr=1, is a SEPARATE plusarg on purpose:
    // faulting AW and AR together would rebase a pair to the same wrong window,
    // where the readback still agrees and nothing fires. On the AW alone the
    // err slave absorbs the W beats and answers BRESP = DECERR, which travels
    // the NSU -> NMU B path and is named by the BRESP fatal below.
    localparam logic [ADDR_WIDTH-1:0] DECERR_FAULT_BIT = 1 << (ADDR_WIDTH - 1);
    bit decerr_fault = 1'b0;
    bit decerr_fault_wr = 1'b0;
    initial void'($value$plusargs("decerr_fault=%d", decerr_fault));
    initial void'($value$plusargs("decerr_fault_wr=%d", decerr_fault_wr));

    // s0: the file_master's own traffic.
    assign tile_axi[0].aw_id     = mst_awid;
    // COLLECTIVE_OP_UNICAST is 0 (ni_flit_pkg), so any non-zero op is a collective.
    assign tile_axi[0].aw_addr   = mst_flat_req.awaddr
                                   + ((mst_flat_awuser[9:8] != 2'd0) ? NOC_EGRESS_BASE
                                                                    : ADDR_WIDTH'(0));
    assign tile_axi[0].aw_len    = mst_flat_req.awlen;
    assign tile_axi[0].aw_size   = mst_flat_req.awsize;
    assign tile_axi[0].aw_burst  = mst_flat_req.awburst;
    assign tile_axi[0].aw_lock   = mst_flat_req.awlock;
    assign tile_axi[0].aw_cache  = mst_flat_req.awcache;
    assign tile_axi[0].aw_prot   = mst_flat_req.awprot;
    assign tile_axi[0].aw_qos    = mst_flat_req.awqos;
    assign tile_axi[0].aw_region = mst_flat_req.awregion;
    assign tile_axi[0].aw_atop   = '0;
    assign tile_axi[0].aw_user   = mst_flat_awuser;
    assign tile_axi[0].aw_valid  = mst_flat_req.awvalid;
    assign tile_axi[0].w_data    = mst_flat_req.wdata;
    assign tile_axi[0].w_strb    = mst_flat_req.wstrb;
    assign tile_axi[0].w_last    = mst_flat_req.wlast;
    assign tile_axi[0].w_valid   = mst_flat_req.wvalid;
    assign tile_axi[0].b_ready   = mst_flat_req.bready;
    assign tile_axi[0].w_user    = '0;
    assign tile_axi[0].ar_id     = mst_arid;
    assign tile_axi[0].ar_addr   = mst_flat_req.araddr;
    assign tile_axi[0].ar_len    = mst_flat_req.arlen;
    assign tile_axi[0].ar_size   = mst_flat_req.arsize;
    assign tile_axi[0].ar_burst  = mst_flat_req.arburst;
    assign tile_axi[0].ar_lock   = mst_flat_req.arlock;
    assign tile_axi[0].ar_cache  = mst_flat_req.arcache;
    assign tile_axi[0].ar_prot   = mst_flat_req.arprot;
    assign tile_axi[0].ar_qos    = mst_flat_req.arqos;
    assign tile_axi[0].ar_region = mst_flat_req.arregion;
    assign tile_axi[0].ar_valid  = mst_flat_req.arvalid;
    assign tile_axi[0].ar_user   = '0;
    assign tile_axi[0].r_ready   = mst_flat_req.rready;
    always_comb begin
        mst_flat_rsp = '0;
        mst_flat_rsp.awready = tile_axi[0].aw_ready;
        mst_flat_rsp.wready  = tile_axi[0].w_ready;
        mst_flat_rsp.bresp   = tile_axi[0].b_resp;
        mst_flat_rsp.bvalid  = tile_axi[0].b_valid;
        mst_flat_rsp.arready = tile_axi[0].ar_ready;
        mst_flat_rsp.rdata   = tile_axi[0].r_data;
        mst_flat_rsp.rresp   = tile_axi[0].r_resp;
        mst_flat_rsp.rlast   = tile_axi[0].r_last;
        mst_flat_rsp.rvalid  = tile_axi[0].r_valid;
    end
    assign mst_bid = tile_axi[0].b_id;
    assign mst_rid = tile_axi[0].r_id;

    // s1: requests delivered by the fabric. The NSU has already rewritten the
    // node-coordinate field to this node (nsu::Depacketize::rebase_), so a
    // collective replica carrying the request's own address decodes here like any
    // unicast.
    assign tile_axi[1].aw_id     = slave_axi_req_i.awid;
    assign tile_axi[1].aw_addr   = slave_axi_req_i.awaddr | (decerr_fault_wr ? DECERR_FAULT_BIT : '0);
    assign tile_axi[1].aw_len    = slave_axi_req_i.awlen;
    assign tile_axi[1].aw_size   = slave_axi_req_i.awsize;
    assign tile_axi[1].aw_burst  = slave_axi_req_i.awburst;
    assign tile_axi[1].aw_lock   = slave_axi_req_i.awlock;
    assign tile_axi[1].aw_cache  = slave_axi_req_i.awcache;
    assign tile_axi[1].aw_prot   = slave_axi_req_i.awprot;
    assign tile_axi[1].aw_qos    = slave_axi_req_i.awqos;
    assign tile_axi[1].aw_region = slave_axi_req_i.awregion;
    assign tile_axi[1].aw_atop   = '0;
    assign tile_axi[1].aw_user   = '0;
    assign tile_axi[1].aw_valid  = slave_axi_req_i.awvalid;
    assign tile_axi[1].w_data    = slave_axi_req_i.wdata;
    assign tile_axi[1].w_strb    = slave_axi_req_i.wstrb;
    assign tile_axi[1].w_last    = slave_axi_req_i.wlast;
    assign tile_axi[1].w_valid   = slave_axi_req_i.wvalid;
    assign tile_axi[1].b_ready   = slave_axi_req_i.bready;
    assign tile_axi[1].w_user    = '0;
    assign tile_axi[1].ar_id     = slave_axi_req_i.arid;
    assign tile_axi[1].ar_addr   = slave_axi_req_i.araddr | (decerr_fault ? DECERR_FAULT_BIT : '0);
    assign tile_axi[1].ar_len    = slave_axi_req_i.arlen;
    assign tile_axi[1].ar_size   = slave_axi_req_i.arsize;
    assign tile_axi[1].ar_burst  = slave_axi_req_i.arburst;
    assign tile_axi[1].ar_lock   = slave_axi_req_i.arlock;
    assign tile_axi[1].ar_cache  = slave_axi_req_i.arcache;
    assign tile_axi[1].ar_prot   = slave_axi_req_i.arprot;
    assign tile_axi[1].ar_qos    = slave_axi_req_i.arqos;
    assign tile_axi[1].ar_region = slave_axi_req_i.arregion;
    assign tile_axi[1].ar_valid  = slave_axi_req_i.arvalid;
    assign tile_axi[1].ar_user   = '0;
    assign tile_axi[1].r_ready   = slave_axi_req_i.rready;
    always_comb begin
        slave_axi_rsp_o = '0;
        slave_axi_rsp_o.awready = tile_axi[1].aw_ready;
        slave_axi_rsp_o.wready  = tile_axi[1].w_ready;
        slave_axi_rsp_o.bid     = tile_axi[1].b_id;
        slave_axi_rsp_o.bresp   = tile_axi[1].b_resp;
        slave_axi_rsp_o.bvalid  = tile_axi[1].b_valid;
        slave_axi_rsp_o.arready = tile_axi[1].ar_ready;
        slave_axi_rsp_o.rid     = tile_axi[1].r_id;
        slave_axi_rsp_o.rdata   = tile_axi[1].r_data;
        slave_axi_rsp_o.rresp   = tile_axi[1].r_resp;
        slave_axi_rsp_o.rlast   = tile_axi[1].r_last;
        slave_axi_rsp_o.rvalid  = tile_axi[1].r_valid;
    end

    // Both initiators are held to their share of the field: the stimulus
    // generator caps its ids at 2**INITIATOR_ID_WIDTH (gen_test_patterns.py
    // axi_widths) and the NSU's collapsed downstream id is all-ones of ID_WIDTH
    // (nsu::remap_downstream_id). Nothing asserts that here, and nothing needs
    // to: the master face is XBAR_SLV_ID_W wide, which is exactly the cap, and
    // the NSU's id is narrower still, so an over-range id cannot be represented
    // on either wire rather than merely going unchecked.

    // Crossbar sizing. Testbench limits, provisioned so none of them becomes the
    // bottleneck: the pressure is supposed to come from the fabric, or from the
    // tile memory's delayer. MaxMstTrans 64 is what one initiator may have in flight, above
    // NMU_MAX_TXNS_PER_ID (32): under hotspot every node targets one tile, so sizing at that
    // depth would throttle; overflow stalls, it never errors. MaxSlvTrans 32 is the
    // per-target in-flight limit -- deliberately NOT 1 on the config port: the
    // memory backpressures itself and the crossbar should not second-guess a
    // target. AxiIdUsedSlvPorts 3 is the ID portion axi_demux tracks per target,
    // so 8 concurrent IDs may sit at different targets at once; aliasing above
    // that stalls, it never errors. CUT_ALL_AX puts a spill register on AW/AR at
    // both ends.
    localparam axi_pkg::xbar_cfg_t TileXbarCfg = '{
        NoSlvPorts:         XBAR_SLV_PORTS,
        NoMstPorts:         XBAR_MST_PORTS,
        MaxMstTrans:        32'd64,
        MaxSlvTrans:        32'd32,
        FallThrough:        1'b0,
        LatencyMode:        axi_pkg::CUT_ALL_AX,
        PipelineStages:     32'd0,
        AxiIdWidthSlvPorts: XBAR_SLV_ID_W,
        AxiIdUsedSlvPorts:  32'd3,
        UniqueIds:          1'b0,
        AxiAddrWidth:       ADDR_WIDTH,
        AxiDataWidth:       DATA_WIDTH,
        NoAddrRules:        TILE_TARGETS + 1
    };

    // Own rule_t rather than axi_pkg::xbar_rule_64_t: the address fields have to
    // follow ADDR_WIDTH, not a fixed 64.
    typedef struct packed {
        int unsigned           idx;
        logic [ADDR_WIDTH-1:0] start_addr;
        logic [ADDR_WIDTH-1:0] end_addr;
    } tile_rule_t;

    // One rule per memory target, end exclusive. Sizes are exact: axi_xbar
    // states a rule as start/end, so a window need not be a power of two.
    tile_rule_t [TILE_TARGETS:0] tile_addr_map;
    for (genvar t = 0; t < TILE_TARGETS; t++) begin : g_addr_map
        assign tile_addr_map[t] = '{
            idx:        t,
            start_addr: TILE_BASE_ADDR[t],
            end_addr:   TILE_BASE_ADDR[t] + TILE_SIZE[t]
        };
    end
    // The aperture spans as much again as it is based at, which covers the whole
    // map offset into it: NOC_EGRESS_BASE is the first power of two at or above
    // the map's top, so every offset address lands in [BASE, 2*BASE).
    assign tile_addr_map[TILE_TARGETS] = '{
        idx:        NMU_TARGET,
        start_addr: NOC_EGRESS_BASE,
        end_addr:   NOC_EGRESS_BASE + NOC_EGRESS_BASE
    };

    // s0 reaches every target; s1 must not reach the NMU (see the header note).
    localparam bit [XBAR_SLV_PORTS-1:0][XBAR_MST_PORTS-1:0] TileConnectivity =
        {{1'b0, {TILE_TARGETS{1'b1}}}, {XBAR_MST_PORTS{1'b1}}};

    // s0 falls through to the NMU, s1 does not fall through at all.
    localparam int unsigned MST_IDX_W = cf_math_pkg::idx_width(XBAR_MST_PORTS);
    logic [XBAR_SLV_PORTS-1:0]                tile_en_default;
    logic [XBAR_SLV_PORTS-1:0][MST_IDX_W-1:0] tile_default_mst;
    assign tile_en_default     = 2'b01;
    assign tile_default_mst[0] = MST_IDX_W'(NMU_TARGET);
    assign tile_default_mst[1] = '0;  // unused, s1's default is disabled

    axi_xbar_intf #(
        .AXI_USER_WIDTH(AWUSER_WIDTH),
        .Cfg(TileXbarCfg),
        .ATOPS(1'b0),
        .CONNECTIVITY(TileConnectivity),
        .rule_t(tile_rule_t)
    ) u_tile_xbar (
        .clk_i,
        .rst_ni,
        .test_i(1'b0),
        .slv_ports(tile_axi),
        .mst_ports(tile_mst),
        .addr_map_i(tile_addr_map),
        .en_default_mst_port_i(tile_en_default),
        .default_mst_port_i(tile_default_mst)
    );

    // m0 / m1 -> the two tile memories, each behind its own delayer. Storage and
    // timing are separate modules on purpose: what sits behind each window is
    // undecided -- an SRAM or a DRAM model are both on the table -- and when a
    // DRAM behavioural model arrives it replaces axi_sim_mem alone, leaving the
    // decode, the delayer and this wiring untouched.
    //
    // axi_sim_mem addresses every beat through axi_pkg::beat_addr, so INCR,
    // FIXED and WRAP all land correctly. A never-written address returns the
    // associative array's type default, which this simulator renders 0 rather
    // than X, since axi_sim_mem creates no entry for a byte nothing wrote. The
    // directed run never reads one either way.
    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_MST_ID_W), .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) tile_mem [TILE_TARGETS-1:0] ();

    for (genvar t = 0; t < TILE_TARGETS; t++) begin : g_tile_mem
        axi_delayer_intf #(
            .AXI_ID_WIDTH(XBAR_MST_ID_W), .AXI_ADDR_WIDTH(ADDR_WIDTH),
            .AXI_DATA_WIDTH(DATA_WIDTH),  .AXI_USER_WIDTH(AWUSER_WIDTH),
            .STALL_RANDOM_INPUT(MEM_STALL_RANDOM_INPUT),
            .STALL_RANDOM_OUTPUT(MEM_STALL_RANDOM_OUTPUT),
            .FIXED_DELAY_INPUT(MEM_FIXED_DELAY_INPUT),
            .FIXED_DELAY_OUTPUT(MEM_FIXED_DELAY_OUTPUT)
        ) i_delayer (
            .clk_i(clk_i), .rst_ni(rst_ni),
            .slv(tile_mst[t]), .mst(tile_mem[t])
        );

        axi_sim_mem_intf #(
            .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
            .AXI_ID_WIDTH(XBAR_MST_ID_W), .AXI_USER_WIDTH(AWUSER_WIDTH),
            .WARN_UNINITIALIZED(1'b0), .UNINITIALIZED_DATA("undefined"),
            .APPL_DELAY(ApplTime), .ACQ_DELAY(TestTime)
        ) i_mem (
            .clk_i(clk_i), .rst_ni(rst_ni),
            .axi_slv(tile_mem[t]),
            .mon_w_valid_o(), .mon_w_addr_o(), .mon_w_data_o(),
            .mon_w_id_o(), .mon_w_user_o(), .mon_w_beat_count_o(), .mon_w_last_o(),
            .mon_r_valid_o(), .mon_r_addr_o(), .mon_r_data_o(),
            .mon_r_id_o(), .mon_r_user_o(), .mon_r_beat_count_o(), .mon_r_last_o()
        );
    end

    // m2 -> the NMU: this node's share of the traffic that goes on the NoC,
    // through the id remap that converts the tile's id space into the NI's.
    axi_id_remap_intf #(
        .AXI_SLV_PORT_ID_WIDTH(XBAR_MST_ID_W),
        .AXI_SLV_PORT_MAX_UNIQ_IDS(1 << ID_WIDTH),
        .AXI_MAX_TXNS_PER_ID(ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT),
        .AXI_MST_PORT_ID_WIDTH(ID_WIDTH),
        .AXI_ADDR_WIDTH(ADDR_WIDTH),
        .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) i_noc_id_remap (
        .clk_i(clk_i),
        .rst_ni(rst_ni),
        .slv(tile_mst[NMU_TARGET]),
        .mst(noc_mst)
    );

    assign master_axi_req_o.awid     = noc_mst.aw_id;
    // Stateless restore: no real region reaches NOC_EGRESS_BASE, so an address
    // at or above it can only be one this endpoint offset on the way in.
    assign master_axi_req_o.awaddr   =
        (noc_mst.aw_addr >= NOC_EGRESS_BASE)
            ? noc_mst.aw_addr - NOC_EGRESS_BASE
            : noc_mst.aw_addr;
    assign master_axi_req_o.awlen    = noc_mst.aw_len;
    assign master_axi_req_o.awsize   = noc_mst.aw_size;
    assign master_axi_req_o.awburst  = noc_mst.aw_burst;
    assign master_axi_req_o.awlock   = noc_mst.aw_lock;
    assign master_axi_req_o.awcache  = noc_mst.aw_cache;
    assign master_axi_req_o.awprot   = noc_mst.aw_prot;
    assign master_axi_req_o.awqos    = noc_mst.aw_qos;
    assign master_axi_req_o.awregion = noc_mst.aw_region;
    assign master_axi_req_o.awvalid  = noc_mst.aw_valid;
    assign master_awuser_o           = noc_mst.aw_user;
    assign master_axi_req_o.wdata    = noc_mst.w_data;
    assign master_axi_req_o.wstrb    = noc_mst.w_strb;
    assign master_axi_req_o.wlast    = noc_mst.w_last;
    assign master_axi_req_o.wvalid   = noc_mst.w_valid;
    assign master_axi_req_o.bready   = noc_mst.b_ready;
    assign master_axi_req_o.arid     = noc_mst.ar_id;
    assign master_axi_req_o.araddr   = noc_mst.ar_addr;
    assign master_axi_req_o.arlen    = noc_mst.ar_len;
    assign master_axi_req_o.arsize   = noc_mst.ar_size;
    assign master_axi_req_o.arburst  = noc_mst.ar_burst;
    assign master_axi_req_o.arlock   = noc_mst.ar_lock;
    assign master_axi_req_o.arcache  = noc_mst.ar_cache;
    assign master_axi_req_o.arprot   = noc_mst.ar_prot;
    assign master_axi_req_o.arqos    = noc_mst.ar_qos;
    assign master_axi_req_o.arregion = noc_mst.ar_region;
    assign master_axi_req_o.arvalid  = noc_mst.ar_valid;
    assign master_axi_req_o.rready   = noc_mst.r_ready;

    assign noc_mst.aw_ready  = master_axi_rsp_i.awready;
    assign noc_mst.w_ready   = master_axi_rsp_i.wready;
    assign noc_mst.b_id      = master_axi_rsp_i.bid;
    assign noc_mst.b_resp    = master_axi_rsp_i.bresp;
    assign noc_mst.b_valid   = master_axi_rsp_i.bvalid;
    assign noc_mst.ar_ready  = master_axi_rsp_i.arready;
    assign noc_mst.r_id      = master_axi_rsp_i.rid;
    assign noc_mst.r_data    = master_axi_rsp_i.rdata;
    assign noc_mst.r_resp    = master_axi_rsp_i.rresp;
    assign noc_mst.r_last    = master_axi_rsp_i.rlast;
    assign noc_mst.r_valid   = master_axi_rsp_i.rvalid;
    assign noc_mst.b_user    = '0;
    assign noc_mst.r_user    = '0;

    // ------------------------------------------------------------------
    // VIP classes
    // ------------------------------------------------------------------
    typedef axi_test::axi_file_master #(
        .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .IW(XBAR_SLV_ID_W), .UW(AWUSER_WIDTH),
        .TA(ApplTime), .TT(TestTime)
    ) file_master_t;
    typedef axi_test::axi_scoreboard #(
        .IW(XBAR_SLV_ID_W), .AW(ADDR_WIDTH), .DW(DATA_WIDTH), .UW(AWUSER_WIDTH), .TT(TestTime)
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
                .AXI_ADDR_WIDTH(ADDR_WIDTH),   .AXI_DATA_WIDTH(DATA_WIDTH),
                .AXI_ID_WIDTH(XBAR_SLV_ID_W),  .AXI_USER_WIDTH(AWUSER_WIDTH)
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
    mcast_preload_scoreboard scoreboard;

    // Stimulus root: <stim_dir>/node<NODE_ID>/{write,read}.txt (emitter output).
    string stim_dir = "sim/test_patterns/directed";
    string write_path;
    string read_path;

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
    // stimulus ids_per_initiator > 1, B responses reorder across ids; within one id
    // AXI returns B in AW issue order, so a per-id count identifies the paired
    // write's B exactly.
    int unsigned b_returned[2**XBAR_SLV_ID_W];

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            b_returned <= '{default: '0};
        end else if (mst_flat_rsp.bvalid && mst_flat_req.bready) begin
            b_returned[mst_bid] <= b_returned[mst_bid] + 1;
            // axi_sim_mem answers every mapped access OKAY, so any error
            // response here is a fabric bug (e.g. a corrupted merged B), or a
            // write that missed every tile window. +decerr_fault_wr=1 proves it
            // fires.
            if (mst_flat_rsp.bresp != axi_pkg::RESP_OKAY)
                $fatal(1, "[mcast_sb] node%0d: BRESP=%0h on id=%0h, expected OKAY",
                       NODE_ID, mst_flat_rsp.bresp, mst_bid);
        end
    end

    // RRESP twin of the BRESP fatal above, and the read half of the tile-window
    // gate: the tile crossbar DECERRs any address outside every window, so a
    // disagreement between the address map the SAM matched and the generated
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
    // config traffic at 0x100000000+off lands inside the memory window, where the
    // write and its readback agree. And a config access overrunning its SAM
    // entry falls into the NEXT entry, routes to a different node's config RAM,
    // rebases to a legal offset there, and also agrees; that one is held off
    // upstream by gen_test_patterns' probe-window guard.
    always_ff @(posedge clk_i) begin
        if (rst_ni && mst_flat_rsp.rvalid && mst_flat_req.rready &&
                !resp_ok(mst_flat_rsp.rresp))
            $fatal(1, "[tile_decode] node%0d: RRESP=%0h on id=%0h, expected OKAY (address outside every tile window?)",
                   NODE_ID, mst_flat_rsp.rresp, mst_rid);
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
                      mst_flat_req.awvalid, mst_flat_rsp.awready,
                      mst_flat_req.wvalid,  mst_flat_rsp.wready,
                      mst_flat_rsp.bvalid,  mst_flat_req.bready);
    end

    // ------------------------------------------------------------------
    // Multicast scoreboard (S4 collectives): replica golden + readback
    // compare, keyed by full byte address (== (dst_id, local_addr): the tile
    // base encodes dst_id, the offset is the node-local address). The pulp
    // scoreboard models only the address the multicast AW names; the other
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
    mcast_txn_t  mcast_ar_q [2**XBAR_SLV_ID_W][$];   // same-id R follows AR order
    mcast_txn_t  mcast_rd_active [2**XBAR_SLV_ID_W];
    bit          mcast_rd_busy [2**XBAR_SLV_ID_W];
    int unsigned mcast_rd_beat [2**XBAR_SLV_ID_W];
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
            if (mst_flat_req.awvalid && mst_flat_rsp.awready) begin
                mcast_wr_q.push_back('{addr: mst_flat_req.awaddr,
                                       len:  mst_flat_req.awlen,
                                       size: mst_flat_req.awsize,
                                       mask: (mst_flat_awuser[9:8] == 2'd1)
                                             ? longint'(mst_flat_awuser[57:10]) : 0});
            end
            // W: golden capture for every member replica of a multicast burst.
            if (mst_flat_req.wvalid && mst_flat_rsp.wready) begin
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
                        if (mst_flat_req.wstrb[j]) begin
                            automatic longint unsigned byte_addr = bus_addr + j;
                            automatic logic [7:0] golden =
                                mst_flat_req.wdata[8*j +: 8] ^ (mcast_fault ? 8'h01 : 8'h00);
                            automatic longint unsigned sub = mc_mask;
                            automatic bit done = 1'b0;
                            while (!done) begin
                                automatic longint unsigned replica =
                                    (byte_addr & ~mc_mask) | sub;
                                // Seed the pulp scoreboard too: replicas at
                                // remote nodes never appear on this node's
                                // master wires, so without this its read
                                // check has no golden for the replica
                                // readback (under +mcast_fault the corrupted
                                // byte flows here as well -- both checkers
                                // then flag, which is the red test's point).
                                // Every replica the wildcard names is delivered:
                                // nothing clips the closure, and the NMU refuses
                                // a mask that reaches a coordinate with no node.
                                mcast_mem[replica] = golden;
                                scoreboard.preload_byte(replica, golden);
                                if (sub == 0) done = 1'b1;
                                else sub = (sub - 1) & mc_mask;
                            end
                        end
                    end
                end
                if (mst_flat_req.wlast) begin
                    void'(mcast_wr_q.pop_front());
                    mcast_wr_beat = 0;
                end else begin
                    mcast_wr_beat = mcast_wr_beat + 1;
                end
            end
            // AR: read descriptor capture, per id.
            if (mst_flat_req.arvalid && mst_flat_rsp.arready) begin
                mcast_ar_q[mst_arid].push_back('{addr: mst_flat_req.araddr,
                                                 len:  mst_flat_req.arlen,
                                                 size: mst_flat_req.arsize,
                                                 mask: 0});
            end
            // R: compare any byte the multicast golden knows.
            if (mst_flat_rsp.rvalid && mst_flat_req.rready) begin
                automatic int unsigned rid = 32'(mst_rid);
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
                if (resp_ok(mst_flat_rsp.rresp)) begin
                    for (int unsigned j = first_byte;
                         j < axi_pkg::num_bytes(mcast_rd_active[rid].size); j++) begin
                        automatic longint unsigned ba = beat_address + j;
                        if (mcast_mem.exists(ba)) begin
                            automatic logic [7:0] act =
                                mst_flat_rsp.rdata[8 * (ba % MC_BUS_BYTES) +: 8];
                            mcast_checked = mcast_checked + 1;
                            if (act !== mcast_mem[ba])
                                $fatal(1, "[mcast_sb] node%0d: replica readback mismatch addr=%h exp=%h act=%h (id=%0d beat=%0d)",
                                       NODE_ID, ba, mcast_mem[ba], act, rid, mcast_rd_beat[rid]);
                        end
                    end
                end
                if (mst_flat_rsp.rlast) mcast_rd_busy[rid] = 1'b0;
                else mcast_rd_beat[rid] = mcast_rd_beat[rid] + 1;
            end
        end
    end

    // Mode 2: read i issues only after write i's B returns. Pair i is
    // transaction i of write.txt/read.txt (same id, same address), so the
    // paired write is the issued[id]-th write with that id and
    // b_returned[id] >= issued[id] is the exact release condition.
    task automatic run_ar_after_b();
        int unsigned issued[2**XBAR_SLV_ID_W];
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
            for (int i = 0; i < 2**XBAR_SLV_ID_W; i++) b_total += b_returned[i];
            if (b_total != int'(file_master.num_writes))
                $fatal(1, "[mcast_sb] node%0d: %0d B handshakes for %0d AWs -- duplicate or lost B",
                       NODE_ID, b_total, file_master.num_writes);
            if (mst_flat_rsp.bvalid)
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
                + 32'(mst_flat_req.awvalid && mst_flat_rsp.awready)
                + 32'(mst_flat_req.arvalid && mst_flat_rsp.arready);
        end
    end

endmodule

`endif  // USER_NODE_ENDPOINT_SV
