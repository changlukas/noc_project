// dma_node_endpoint — per-node test endpoint with a pulp iDMA backend on the
// master face. Structurally user_node_endpoint.sv with the master swapped: the
// pulp axi_xbar tile crossbar, the per-space axi_delayer + axi_sim_mem targets,
// the id remap toward the NI and the FlooNoC axi_bw_monitor are the same, so
// what the fabric sees differs only in who generates the traffic.
//
// What is NOT here, and why: axi_file_master replays a chosen AXI shape, so
// every checker built on it -- the injection-mode case, the read-after-B
// interlock, the B-count epilogue, the axi_scoreboard and its multicast golden
// -- reads that master's own wires and assumes its write-then-read stimulus.
// A DMA issues what its legalizer decides, so none of those hold. The check
// that does is in the generated top: it preloads every source region, waits for
// every job to retire, and compares each destination region against its source
// through a backdoor into axi_sim_mem's array. The job counts leave the driver
// on jobs_issued / jobs_retired for it.
//
// The BRESP and RRESP fatals below are NOT among the dropped set: they read the
// response wires rather than the stimulus, so they hold for any master.
//
// Slave face (tile decode): unchanged from user_node_endpoint -- the NSU
// forwards the request's own address, so the crossbar decodes on this node's
// windows exactly as the topology address_map placed them.
//
// Stimulus: <stim_dir>/node<ID>/jobs.txt (+stim_dir=), read by idma_job_driver.

`ifndef DMA_NODE_ENDPOINT_SV
`define DMA_NODE_ENDPOINT_SV

`include "axi/assign.svh"

module dma_node_endpoint #(
    parameter int unsigned NODE_ID      = 0,
    parameter int unsigned ID_WIDTH     = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    // ADDR/DATA/AWUSER widths are what the crossbar, the memories and the id
    // remap are sized by. The DMA's own types come from idma_types_pkg, which
    // fixes them at the same ni_params_pkg defaults the generator passes here;
    // the two are one set of values, stated in two places because a package
    // cannot take a parameter.
    parameter int unsigned ADDR_WIDTH   = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH   = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    // THIS node's own crossbar windows, stamped by gen_tb_top.py from the
    // topology YAML. Field t is target t, m0 = config, LAST = data (see
    // gen_tb_top.tile_targets). No defaults: only the generator knows a
    // topology's tile layout.
    parameter int unsigned TILE_TARGETS,
    parameter logic [TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_BASE_ADDR,
    parameter logic [TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_SIZE,
    // Where the crossbar's NoC egress aperture sits (address_map.noc_egress_base).
    parameter logic [ADDR_WIDTH-1:0] NOC_EGRESS_BASE,
    // Tile-memory latency, one profile stamped by gen_tb_top.py from
    // _MEM_LATENCY. Input covers AW/W/AR, output covers B/R.
    parameter bit          MEM_STALL_RANDOM_INPUT  = 1'b0,
    parameter bit          MEM_STALL_RANDOM_OUTPUT = 1'b0,
    parameter int unsigned MEM_FIXED_DELAY_INPUT   = 0,
    parameter int unsigned MEM_FIXED_DELAY_OUTPUT  = 0,
    // Master-face consumer backpressure, response side only.
    parameter bit          MST_STALL_RANDOM_OUTPUT = 1'b0,
    parameter int unsigned MST_FIXED_DELAY_OUTPUT  = 0,
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

    // Tile id widths. Slave-port ID width is one initiator's own share of the
    // field; the master-port width adds the $clog2(NoSlvPorts) index axi_xbar
    // appends to route responses back. The DMA drives the slave-port width,
    // which is what idma_types_pkg::AXI_ID_WIDTH is.
    localparam int unsigned XBAR_SLV_PORTS = 2;
    localparam int unsigned XBAR_SLV_ID_W  = ni_params_pkg::AXI_INITIATOR_ID_WIDTH_DFLT;
    localparam int unsigned XBAR_MST_ID_W  = XBAR_SLV_ID_W + $clog2(XBAR_SLV_PORTS);

    // ------------------------------------------------------------------
    // iDMA backend + job driver
    // ------------------------------------------------------------------
    idma_types_pkg::idma_req_t dma_job_req;
    logic                      dma_job_req_valid, dma_job_req_ready;
    idma_types_pkg::idma_rsp_t dma_job_rsp;
    logic                      dma_job_rsp_valid;

    idma_types_pkg::axi_req_t  dma_read_req, dma_write_req, dma_req;
    idma_types_pkg::axi_resp_t dma_read_rsp, dma_write_rsp, dma_rsp;
    idma_pkg::idma_busy_t      dma_busy;

    int unsigned jobs_issued, jobs_retired;

    idma_job_driver #(
        .NODE_ID(NODE_ID)
    ) i_job_driver (
        .clk_i, .rst_ni,
        .req_o          ( dma_job_req       ),
        .req_valid_o    ( dma_job_req_valid ),
        .req_ready_i    ( dma_job_req_ready ),
        .rsp_valid_i    ( dma_job_rsp_valid ),
        .jobs_issued_o  ( jobs_issued       ),
        .jobs_retired_o ( jobs_retired      )
    );

    idma_backend_rw_axi #(
        .DataWidth            ( idma_types_pkg::DATA_WIDTH   ),
        .AddrWidth            ( idma_types_pkg::ADDR_WIDTH   ),
        .UserWidth            ( idma_types_pkg::USER_WIDTH   ),
        .AxiIdWidth           ( idma_types_pkg::AXI_ID_WIDTH ),
        .NumAxInFlight        ( 32'd64 ),   // MaxMstTrans, what the crossbar allows one initiator
        .BufferDepth          ( 32'd3  ),   // iDMA's own recommendation for misaligned transfers
        .TFLenWidth           ( idma_types_pkg::TF_LEN_WIDTH ),
        .MemSysDepth          ( 32'd0  ),   // no round-trip constant exists here yet
        .RAWCouplingAvail     ( 1'b1   ),
        .MaskInvalidData      ( 1'b1   ),
        .HardwareLegalizer    ( 1'b1   ),
        .RejectZeroTransfers  ( 1'b1   ),
        .ErrorCap             ( idma_pkg::NO_ERROR_HANDLING ),
        .idma_req_t           ( idma_types_pkg::idma_req_t  ),
        .idma_rsp_t           ( idma_types_pkg::idma_rsp_t  ),
        .idma_eh_req_t        ( idma_pkg::idma_eh_req_t ),
        .idma_busy_t          ( idma_pkg::idma_busy_t   ),
        .axi_req_t            ( idma_types_pkg::axi_req_t  ),
        .axi_rsp_t            ( idma_types_pkg::axi_resp_t ),
        .read_meta_channel_t  ( idma_types_pkg::read_meta_channel_t  ),
        .write_meta_channel_t ( idma_types_pkg::write_meta_channel_t )
    ) i_dma (
        .clk_i, .rst_ni,
        .testmode_i      ( 1'b0              ),
        .idma_req_i      ( dma_job_req       ),
        .req_valid_i     ( dma_job_req_valid ),
        .req_ready_o     ( dma_job_req_ready ),
        .idma_rsp_o      ( dma_job_rsp       ),
        .rsp_valid_o     ( dma_job_rsp_valid ),
        .rsp_ready_i     ( 1'b1              ),
        .idma_eh_req_i   ( '0                ),   // ErrorCap is NO_ERROR_HANDLING
        .eh_req_valid_i  ( 1'b0              ),
        .eh_req_ready_o  (                   ),
        .axi_read_req_o  ( dma_read_req      ),
        .axi_read_rsp_i  ( dma_read_rsp      ),
        .axi_write_req_o ( dma_write_req     ),
        .axi_write_rsp_i ( dma_write_rsp     ),
        .busy_o          ( dma_busy          )
    );

    // The backend keeps read and write on separate AXI ports; the tile crossbar
    // has one slave port per initiator, so the two rejoin here.
    axi_rw_join #(
        .axi_req_t  ( idma_types_pkg::axi_req_t  ),
        .axi_resp_t ( idma_types_pkg::axi_resp_t )
    ) i_rw_join (
        .clk_i, .rst_ni,
        .slv_read_req_i   ( dma_read_req  ), .slv_read_resp_o  ( dma_read_rsp  ),
        .slv_write_req_i  ( dma_write_req ), .slv_write_resp_o ( dma_write_rsp ),
        .mst_req_o        ( dma_req       ), .mst_resp_i       ( dma_rsp       )
    );

    // ------------------------------------------------------------------
    // Master face: DMA -> consumer backpressure -> tile crossbar slave port 0
    // ------------------------------------------------------------------
    // Response side only: stalling AW/W/AR here would be injection-rate
    // control, and a DMA sets its own injection rate.
    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),   .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_SLV_ID_W),  .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) mst_pre_delay ();

    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),   .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_SLV_ID_W),  .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) mst_post_delay ();

    `AXI_ASSIGN_FROM_REQ(mst_pre_delay, dma_req)
    `AXI_ASSIGN_TO_RESP(dma_rsp, mst_pre_delay)

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

    // Evidence the knob acts, reported once per node at $finish. Zero by
    // construction under the "ideal" profile, where the delayer is wires.
    int unsigned mst_r_stall_cycles = 0;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) mst_r_stall_cycles <= 0;
        else if (master_axi_rsp_i.rvalid && !master_axi_req_o.rready)
            mst_r_stall_cycles <= mst_r_stall_cycles + 1;
    end
    final $display("[mst_bp] node%0d: R backpressure held %0d cycles",
                   NODE_ID, mst_r_stall_cycles);

    // ------------------------------------------------------------------
    // Tile crossbar: DMA + NSU in, config / data memory / NMU out
    // ------------------------------------------------------------------
    // Two initiators share one decoder, so a request this node addresses to its
    // own tile is answered here and never enters the fabric:
    //
    //   s0  DMA   hit -> m0/m1     miss -> m2 (default) -> NMU -> NoC
    //   s1  NSU   hit -> m0/m1     miss -> DECERR (no default)
    //
    // s1 has no default on purpose: an address arriving from the fabric that is
    // not this node's means the fabric misrouted, and DECERR is the honest
    // answer. Forbidding s1 -> m2 keeps a delivered request off the NoC.
    //
    // A third rule covers the NoC egress aperture and points at m2. Nothing
    // this endpoint issues lands in it -- the aperture exists for the collective
    // offset user_node_endpoint applies, and a DMA emits no collectives -- but
    // the rule stays so both endpoints present the same decode to the fabric.
    localparam int unsigned NMU_TARGET     = TILE_TARGETS;  // last master port
    localparam int unsigned XBAR_MST_PORTS = TILE_TARGETS + 1;

    // DESCENDING ranges, not [N]: axi_xbar_intf declares its ports
    // [NoSlvPorts-1:0] / [NoMstPorts-1:0] and SystemVerilog binds an interface
    // array port element-by-element in declared order.
    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),  .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_SLV_ID_W), .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) tile_axi [XBAR_SLV_PORTS-1:0] ();

    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH),  .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(XBAR_MST_ID_W), .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) tile_mst [XBAR_MST_PORTS-1:0] ();

    // m2 after the id remap: the NoC-facing face of the tile, at the NI's id
    // width. axi_id_remap stalls an id that finds no free downstream id rather
    // than erroring, and never puts two distinct upstream ids on one downstream
    // id, so per-id ordering survives the fold.
    AXI_BUS #(
        .AXI_ADDR_WIDTH(ADDR_WIDTH), .AXI_DATA_WIDTH(DATA_WIDTH),
        .AXI_ID_WIDTH(ID_WIDTH),     .AXI_USER_WIDTH(AWUSER_WIDTH)
    ) noc_mst ();

    // s0: the DMA's own traffic.
    `AXI_ASSIGN(tile_axi[0], mst_post_delay)

    // Fault injection for the DECERR gate: +decerr_fault=1 sets the top address
    // bit on this node's inbound AR, which lands outside every window exactly as
    // a Python/C++ tile-layout divergence would. The crossbar routes it to its
    // internal axi_err_slv and the RRESP fatal below names it. The write twin,
    // +decerr_fault_wr=1, is a SEPARATE plusarg: faulting AW and AR together
    // would rebase a pair to the same wrong window, where nothing disagrees. On
    // the AW alone the err slave absorbs the W beats and answers BRESP = DECERR.
    // Without these two the fatals below are checkers nobody has watched fire.
    localparam logic [ADDR_WIDTH-1:0] DECERR_FAULT_BIT = 1 << (ADDR_WIDTH - 1);
    bit decerr_fault = 1'b0;
    bit decerr_fault_wr = 1'b0;
    initial void'($value$plusargs("decerr_fault=%d", decerr_fault));
    initial void'($value$plusargs("decerr_fault_wr=%d", decerr_fault_wr));

    // s1: requests delivered by the fabric. The NSU has already rewritten the
    // node-coordinate field to this node (nsu::Depacketize::rebase_).
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

    // Crossbar sizing, as user_node_endpoint: testbench limits provisioned so
    // none of them becomes the bottleneck. MaxMstTrans 64 is what one initiator
    // may have in flight -- the same number the DMA's NumAxInFlight is set to,
    // so the crossbar never throttles it. Overflow stalls, it never errors.
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
    // timing are separate modules so a DRAM behavioural model can later replace
    // axi_sim_mem without touching the decode, the delayer or this wiring.
    //
    // axi_sim_mem addresses every beat through axi_pkg::beat_addr, so INCR,
    // FIXED and WRAP all land correctly. UNINITIALIZED_DATA("undefined") gives
    // X on a never-written address.
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
            .mon_w_id_o(), .mon_w_user_o(), .mon_w_beat_count_o(),
            .mon_w_last_o(),
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
    assign master_axi_req_o.awaddr   = noc_mst.aw_addr;
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
    // Response checks (ported verbatim in intent from user_node_endpoint:
    // neither depends on the file master -- both read the master-face response
    // wires, which here are the DMA's)
    // ------------------------------------------------------------------
    // The one "not an error response" predicate, {OKAY, EXOKAY}, the set pulp
    // uses (axi_test.sv:2133-2134). EXOKAY is unreachable today: nothing issues
    // exclusive accesses.
    function automatic bit resp_ok(input logic [1:0] resp);
        return resp inside {axi_pkg::RESP_OKAY, axi_pkg::RESP_EXOKAY};
    endfunction

    // axi_sim_mem answers every mapped access OKAY, so an error response here is
    // a fabric bug (a corrupted merged B) or a write that missed every tile
    // window.
    always_ff @(posedge clk_i) begin
        if (rst_ni && mst_post_delay.b_valid && mst_post_delay.b_ready &&
                mst_post_delay.b_resp != axi_pkg::RESP_OKAY)
            $fatal(1, "[dma_ep] node%0d: BRESP=%0h on id=%0h, expected OKAY",
                   NODE_ID, mst_post_delay.b_resp, mst_post_delay.b_id);
    end

    // The read half of the tile-window gate, and the only thing in the testbench
    // that catches an address matching NO window: the crossbar DECERRs it, and
    // pulp's read-data compare is SKIPPED once r_resp leaves {OKAY, EXOKAY}
    // (axi_test.sv:2133-2134), so an error response would otherwise pass
    // silently. A DMA reaches this failure mode more easily than the file
    // master did: gen_dma_jobs.py recomputes the window bases through
    // address_map.pack(), and a disagreement with the SAM lands here.
    always_ff @(posedge clk_i) begin
        if (rst_ni && mst_post_delay.r_valid && mst_post_delay.r_ready &&
                !resp_ok(mst_post_delay.r_resp))
            $fatal(1, "[dma_ep] node%0d: RRESP=%0h on id=%0h, expected OKAY (address outside every tile window?)",
                   NODE_ID, mst_post_delay.r_resp, mst_post_delay.r_id);
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
                      mst_post_delay.aw_valid, mst_post_delay.aw_ready,
                      mst_post_delay.w_valid,  mst_post_delay.w_ready,
                      mst_post_delay.b_valid,  mst_post_delay.b_ready);
    end

    // ------------------------------------------------------------------
    // Exit
    // ------------------------------------------------------------------
    // Nothing here decides when the run is over. The generated top does: every
    // DMA has retired every job its file holds and every destination has
    // answered the write bursts that carry them, read off jobs_issued /
    // jobs_retired above and the endpoints' own slave-face B handshakes.
    assign end_of_sim_o = 1'b0;

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

    // Master-face AX handshakes, the same count user_node_endpoint reports:
    // everything the initiator issued, tile-local traffic included.
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) txn_cnt_o <= 0;
        else begin
            txn_cnt_o <= txn_cnt_o
                + 32'(mst_post_delay.aw_valid && mst_post_delay.aw_ready)
                + 32'(mst_post_delay.ar_valid && mst_post_delay.ar_ready);
        end
    end

endmodule

`endif  // DMA_NODE_ENDPOINT_SV
