`timescale 1ns/1ps

// NoC fabric: an X_DIM x Y_DIM router array plus N_PERIPH off-mesh NIs.
//
// Every node is ni_wrap (nmu+nsu+dat_merge) + REQ/RSP/DAT router_wrap, joined
// by inter-router directional links (N/E/S/W). Boundary directions are tied
// off; a tied-off direction DRIVING a valid flit is a $fatal (guards a fabric
// wiring mistake; the C++ route leak is caught by route_compute's abort). The
// DPI ctx handles arrive as ports; the fabric does no cmodel_*_create. Each
// endpoint exposes a master-side AXI port (NMU ingress) and a slave-side AXI
// port (NSU egress).
//
// A peripheral is an ni_wrap on a router's boundary port, with no router of its
// own. It SHARES its host router's coordinate and is told apart by the port it
// hangs off (the flit header's dst_port_id). Peripherals extend the ENDPOINT
// index space, not the node index space: 0..NUM_NODES-1 stay the routers and
// peripheral p is endpoint NUM_NODES + p, which router_ctx keeps out of.

`ifndef NOC_FABRIC_SV
`define NOC_FABRIC_SV

module noc_fabric #(
    // Mesh geometry. The linear node index IS the array position: X = i % X_DIM,
    // Y = i / X_DIM, and the routing id route_compute agrees on is (Y<<4)|X.
    parameter int unsigned X_DIM          = 2,
    parameter int unsigned Y_DIM          = 2,
    parameter int unsigned ID_WIDTH       = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH     = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH     = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    parameter int unsigned DAT_NUM_VC     = 1,
    parameter int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT,
    parameter int unsigned RSP_FLIT_WIDTH = ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT,
    parameter int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT,
    // ROUTER_VC_DEPTH: per-VC input FIFO depth inside the DAT router; also the
    // credit seed the DAT merge point's downstream pool is initialized with, so
    // both ends of every link agree on the credit window.
    parameter int unsigned ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT,
    // Off-mesh peripherals, in endpoint order. PERIPH_NODE[p] is the linear
    // index of the router peripheral p hangs off; PERIPH_PORT[p] is its FACE,
    // 1 = x (the WEST or EAST port) and 2 = y (SOUTH or NORTH). Which port of
    // that axis is meant follows from the host router's own edge, so the face
    // names the axis and the coordinate names the side.
    parameter int unsigned N_PERIPH       = 0,
    // Sized N_PERIPH_MAX rather than N_PERIPH: neither a packed nor an unpacked
    // array can have zero elements, and N_PERIPH = 0 (the plain mesh) has to
    // stay legal. Field p is peripheral p, so the override is written descending.
    parameter int unsigned N_PERIPH_MAX   = (N_PERIPH > 0) ? N_PERIPH : 1,
    // PACKED, not `int unsigned PERIPH_NODE [N_PERIPH]`: Verilator 5.048 sizes
    // an unpacked-array parameter from its DEFAULT, so an override whose length
    // follows a sibling parameter is rejected as "assignment pattern with too
    // many elements". Same reason TILE_BASE_ADDR is packed in the generated top.
    // 8 b is the flit's own node-index field width, so it holds every endpoint a
    // topology can carry.
    parameter logic [N_PERIPH_MAX-1:0][7:0] PERIPH_NODE = '0,
    parameter logic [N_PERIPH_MAX-1:0][7:0] PERIPH_PORT = '0
) (
    input  logic clk_i,
    input  logic rst_ni,
    // Per-node DPI ctx handle arrays (chandle-substitute longint unsigned).
    // router_ctx is per NODE; the rest are per ENDPOINT.
    input  longint unsigned router_ctx     [X_DIM*Y_DIM],
    input  longint unsigned nmu_ctx        [X_DIM*Y_DIM + N_PERIPH],
    input  longint unsigned nsu_ctx        [X_DIM*Y_DIM + N_PERIPH],
    input  longint unsigned dat_merge_ctx  [X_DIM*Y_DIM + N_PERIPH],
    // Per-endpoint AXI faces (struct arrays): NMU ingress (driven by tb master)
    input  ni_signals_pkg::axi_req_t  master_axi_req [X_DIM*Y_DIM + N_PERIPH],
    // AWUSER sideband: collective op + address mask.
    // Dedicated array beside the struct (axi_req_t has no awuser field).
    input  logic [ni_params_pkg::AXI_AWUSER_WIDTH_DFLT-1:0]
                                      master_awuser  [X_DIM*Y_DIM + N_PERIPH],
    output ni_signals_pkg::axi_rsp_t  master_axi_rsp [X_DIM*Y_DIM + N_PERIPH],
    // Per-endpoint AXI faces (struct arrays): NSU egress (consumed by tb slave)
    output ni_signals_pkg::axi_req_t  slave_axi_req  [X_DIM*Y_DIM + N_PERIPH],
    input  ni_signals_pkg::axi_rsp_t  slave_axi_rsp  [X_DIM*Y_DIM + N_PERIPH]
);

    localparam int unsigned NUM_NODES = X_DIM * Y_DIM;
    localparam int unsigned LINK_PORTS = 5;  // LOCAL + N/E/S/W
    // RouterPort direction indices (router.hpp enum).
    localparam int unsigned RP_LOCAL = 0;
    localparam int unsigned RP_NORTH = 1;
    localparam int unsigned RP_EAST = 2;
    localparam int unsigned RP_SOUTH = 3;
    localparam int unsigned RP_WEST = 4;

    // The host router of peripheral p, widened out of its packed field.
    function automatic int unsigned periph_host(input int unsigned p);
        return int'(PERIPH_NODE[p]);
    endfunction

    // The boundary port peripheral p occupies. Face x is WEST on column 0 and
    // EAST on the last column, face y SOUTH on row 0 and NORTH on the last row.
    // A corner router is legal: it has two free faces and the face says which
    // one is meant.
    function automatic int unsigned periph_rp(input int unsigned p);
        int unsigned px = periph_host(p) % X_DIM;
        int unsigned py = periph_host(p) / X_DIM;
        return (PERIPH_PORT[p] == 8'd1) ? ((px == 0) ? RP_WEST  : RP_EAST)
                                        : ((py == 0) ? RP_SOUTH : RP_NORTH);
    endfunction

    // Whether a node's direction is populated rather than tied off.
    function automatic bit periph_on(input int unsigned node, input int unsigned rp);
        for (int unsigned p = 0; p < N_PERIPH; p++)
            if (periph_host(p) == node && periph_rp(p) == rp) return 1'b1;
        return 1'b0;
    endfunction

    // Per-network per-node per-port arrays (LOCAL + N/E/S/W uniformly).
    logic [LINK_PORTS-1:0]        tx_req_valid [NUM_NODES];
    logic [REQ_FLIT_WIDTH-1:0]    tx_req_flit  [NUM_NODES][LINK_PORTS];
    logic [LINK_PORTS-1:0]        rx_req_valid [NUM_NODES];
    logic [REQ_FLIT_WIDTH-1:0]    rx_req_flit  [NUM_NODES][LINK_PORTS];
    logic [LINK_PORTS-1:0]        tx_req_ready [NUM_NODES];  // input to router_wrap
    logic [LINK_PORTS-1:0]        rx_req_ready [NUM_NODES];  // output of router_wrap

    logic [LINK_PORTS-1:0]        tx_rsp_valid [NUM_NODES];
    logic [RSP_FLIT_WIDTH-1:0]    tx_rsp_flit  [NUM_NODES][LINK_PORTS];
    logic [LINK_PORTS-1:0]        rx_rsp_valid [NUM_NODES];
    logic [RSP_FLIT_WIDTH-1:0]    rx_rsp_flit  [NUM_NODES][LINK_PORTS];
    logic [LINK_PORTS-1:0]        tx_rsp_ready [NUM_NODES];  // input to router_wrap
    logic [LINK_PORTS-1:0]        rx_rsp_ready [NUM_NODES];  // output of router_wrap

    logic [LINK_PORTS-1:0]        tx_dat_valid [NUM_NODES];
    logic [DAT_FLIT_WIDTH-1:0]    tx_dat_flit  [NUM_NODES][LINK_PORTS];
    logic [LINK_PORTS-1:0]        rx_dat_valid [NUM_NODES];
    logic [DAT_FLIT_WIDTH-1:0]    rx_dat_flit  [NUM_NODES][LINK_PORTS];
    logic [DAT_NUM_VC-1:0]        tx_dat_crdvalid [NUM_NODES][LINK_PORTS];  // input
    logic [DAT_NUM_VC-1:0]        rx_dat_crdvalid [NUM_NODES][LINK_PORTS];  // output

    // -------------------------------------------------------------------------
    // Peripherals: an NI on a populated boundary port, no router of its own
    // -------------------------------------------------------------------------
    // The three link OUTPUTS land on these arrays rather than on the per-node
    // arrays directly, because the tie-off always_comb in the node loop below
    // already drives those and a variable cannot take both a procedural and a
    // continuous driver. They are declared HERE, not inside g_periph: that
    // always_comb reads them at a runtime p, and a hierarchical name through a
    // generate block cannot take a non-constant index. The link INPUTS read the
    // host router's own outputs, which needs no intermediary.
    logic                        periph_tx_req_valid    [N_PERIPH_MAX];
    logic [REQ_FLIT_WIDTH-1:0]   periph_tx_req_flit     [N_PERIPH_MAX];
    logic                        periph_rx_req_ready    [N_PERIPH_MAX];
    logic                        periph_tx_rsp_valid    [N_PERIPH_MAX];
    logic [RSP_FLIT_WIDTH-1:0]   periph_tx_rsp_flit     [N_PERIPH_MAX];
    logic                        periph_rx_rsp_ready    [N_PERIPH_MAX];
    logic                        periph_tx_dat_valid    [N_PERIPH_MAX];
    logic [DAT_FLIT_WIDTH-1:0]   periph_tx_dat_flit     [N_PERIPH_MAX];
    logic [DAT_NUM_VC-1:0]       periph_rx_dat_crdvalid [N_PERIPH_MAX];

    for (genvar p = 0; p < N_PERIPH; p++) begin : g_periph
        localparam int unsigned EP   = NUM_NODES + p;      // endpoint index
        localparam int unsigned HOST = periph_host(p);     // host router node
        localparam int unsigned DIR  = periph_rp(p);       // its boundary port

        ni_wrap #(
            .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
            .DAT_NUM_VC(DAT_NUM_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),
            .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH)
        ) u_ni (
            .clk_i(clk_i), .rst_ni(rst_ni),
            .nmu_ctx_i(nmu_ctx[EP]), .nsu_ctx_i(nsu_ctx[EP]),
            .dat_merge_ctx_i(dat_merge_ctx[EP]),
            .master_axi_req_i(master_axi_req[EP]),
            .master_awuser_i(master_awuser[EP]),
            .master_axi_rsp_o(master_axi_rsp[EP]),
            .slave_axi_req_o(slave_axi_req[EP]),
            .slave_axi_rsp_i(slave_axi_rsp[EP]),
            .tx_req_valid_o(periph_tx_req_valid[p]),
            .tx_req_flit_o(periph_tx_req_flit[p]),
            .tx_req_ready_i(rx_req_ready[HOST][DIR]),
            .rx_req_valid_i(tx_req_valid[HOST][DIR]),
            .rx_req_flit_i(tx_req_flit[HOST][DIR]),
            .rx_req_ready_o(periph_rx_req_ready[p]),
            .tx_rsp_valid_o(periph_tx_rsp_valid[p]),
            .tx_rsp_flit_o(periph_tx_rsp_flit[p]),
            .tx_rsp_ready_i(rx_rsp_ready[HOST][DIR]),
            .rx_rsp_valid_i(tx_rsp_valid[HOST][DIR]),
            .rx_rsp_flit_i(tx_rsp_flit[HOST][DIR]),
            .rx_rsp_ready_o(periph_rx_rsp_ready[p]),
            .tx_dat_valid_o(periph_tx_dat_valid[p]),
            .tx_dat_flit_o(periph_tx_dat_flit[p]),
            .tx_dat_crdvalid_i(rx_dat_crdvalid[HOST][DIR]),
            .rx_dat_valid_i(tx_dat_valid[HOST][DIR]),
            .rx_dat_flit_i(tx_dat_flit[HOST][DIR]),
            .rx_dat_crdvalid_o(periph_rx_dat_crdvalid[p])
        );

        // Link perf monitors on the host router's boundary port, one per
        // directed edge as every inter-router link carries, named after the two
        // ENDPOINTS rather than the endpoint index: two peripherals can hang off
        // one router, so the index alone says which endpoint but not which
        // physical port carries it. The router end is ".router", not ".local" --
        // this link terminates at the face port, and a flit on it has not been
        // near the tile on the LOCAL port.
        //
        // The router-to-peripheral edge answers "did the fabric eject at the
        // face port rather than at the tile sharing this coordinate"; the
        // peripheral-to-router edge answers "did the peripheral put a flit on
        // the wire" -- without it a silent peripheral and a working one look
        // alike. The return wire is the OTHER end's: a monitor watches flits
        // leaving one end and the ready/credit coming back from the one they
        // arrive at.
        localparam string FACE = (PERIPH_PORT[p] == 8'd1) ? "x" : "y";

        link_perf_monitor #(
            .LINK_NAME($sformatf("req_node%0d.router_to_node%0d.%s", HOST, HOST, FACE)),
            .FLOW("ready_valid"),
            .BUFFER_DEPTH(ROUTER_VC_DEPTH),
            .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
        ) u_perf_req (
            .clk_i, .rst_ni,
            .valid(tx_req_valid[HOST][DIR]),
            .ready(tx_req_ready[HOST][DIR]),
            .vc_id('0),
            .credit_pulse('0)
        );
        link_perf_monitor #(
            .LINK_NAME($sformatf("req_node%0d.%s_to_node%0d.router", HOST, FACE, HOST)),
            .FLOW("ready_valid"),
            .BUFFER_DEPTH(ROUTER_VC_DEPTH),
            .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
        ) u_perf_req_out (
            .clk_i, .rst_ni,
            .valid(periph_tx_req_valid[p]),
            .ready(rx_req_ready[HOST][DIR]),
            .vc_id('0),
            .credit_pulse('0)
        );
        link_perf_monitor #(
            .LINK_NAME($sformatf("rsp_node%0d.router_to_node%0d.%s", HOST, HOST, FACE)),
            .FLOW("ready_valid"),
            .BUFFER_DEPTH(ROUTER_VC_DEPTH),
            .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
        ) u_perf_rsp (
            .clk_i, .rst_ni,
            .valid(tx_rsp_valid[HOST][DIR]),
            .ready(tx_rsp_ready[HOST][DIR]),
            .vc_id('0),
            .credit_pulse('0)
        );
        link_perf_monitor #(
            .LINK_NAME($sformatf("rsp_node%0d.%s_to_node%0d.router", HOST, FACE, HOST)),
            .FLOW("ready_valid"),
            .BUFFER_DEPTH(ROUTER_VC_DEPTH),
            .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
        ) u_perf_rsp_out (
            .clk_i, .rst_ni,
            .valid(periph_tx_rsp_valid[p]),
            .ready(rx_rsp_ready[HOST][DIR]),
            .vc_id('0),
            .credit_pulse('0)
        );
        link_perf_monitor #(
            .LINK_NAME($sformatf("dat_node%0d.router_to_node%0d.%s", HOST, HOST, FACE)),
            .FLOW("credit"),
            .BUFFER_DEPTH(ROUTER_VC_DEPTH),
            .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
        ) u_perf_dat (
            .clk_i, .rst_ni,
            .valid(tx_dat_valid[HOST][DIR]),
            .ready(1'b0),
            .vc_id(tx_dat_flit[HOST][DIR][ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]),
            .credit_pulse(tx_dat_crdvalid[HOST][DIR])
        );
        link_perf_monitor #(
            .LINK_NAME($sformatf("dat_node%0d.%s_to_node%0d.router", HOST, FACE, HOST)),
            .FLOW("credit"),
            .BUFFER_DEPTH(ROUTER_VC_DEPTH),
            .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
        ) u_perf_dat_out (
            .clk_i, .rst_ni,
            .valid(periph_tx_dat_valid[p]),
            .ready(1'b0),
            .vc_id(periph_tx_dat_flit[p][ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]),
            .credit_pulse(rx_dat_crdvalid[HOST][DIR])
        );
    end : g_periph

    // -------------------------------------------------------------------------
    // Per-node generate: ni_wrap + router_wrap + link wiring + perf monitors
    // -------------------------------------------------------------------------
    for (genvar i = 0; i < NUM_NODES; i++) begin : g_node
        localparam int unsigned X = i % X_DIM;
        localparam int unsigned Y = i / X_DIM;
        // Live-neighbor flags + peer linear indices (boundary -> tied off).
        localparam bit HAS_N = (Y + 1 < Y_DIM);
        localparam bit HAS_E = (X + 1 < X_DIM);
        localparam bit HAS_S = (Y >= 1);
        localparam bit HAS_W = (X >= 1);
        localparam int unsigned PEER_N = i + X_DIM;
        localparam int unsigned PEER_E = i + 1;
        localparam int unsigned PEER_S = i - X_DIM;
        localparam int unsigned PEER_W = i - 1;

        ni_wrap #(
            .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
            .DAT_NUM_VC(DAT_NUM_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),
            .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH)
        ) u_ni (
            .clk_i(clk_i), .rst_ni(rst_ni),
            .nmu_ctx_i(nmu_ctx[i]), .nsu_ctx_i(nsu_ctx[i]),
            .dat_merge_ctx_i(dat_merge_ctx[i]),
            .master_axi_req_i(master_axi_req[i]), .master_awuser_i(master_awuser[i]),
            .master_axi_rsp_o(master_axi_rsp[i]),
            .slave_axi_req_o(slave_axi_req[i]),   .slave_axi_rsp_i(slave_axi_rsp[i]),
            .tx_req_valid_o(rx_req_valid[i][RP_LOCAL]),
            .tx_req_flit_o(rx_req_flit[i][RP_LOCAL]),
            .tx_req_ready_i(rx_req_ready[i][RP_LOCAL]),
            .rx_req_valid_i(tx_req_valid[i][RP_LOCAL]),
            .rx_req_flit_i(tx_req_flit[i][RP_LOCAL]),
            .rx_req_ready_o(tx_req_ready[i][RP_LOCAL]),
            .tx_rsp_valid_o(rx_rsp_valid[i][RP_LOCAL]),
            .tx_rsp_flit_o(rx_rsp_flit[i][RP_LOCAL]),
            .tx_rsp_ready_i(rx_rsp_ready[i][RP_LOCAL]),
            .rx_rsp_valid_i(tx_rsp_valid[i][RP_LOCAL]),
            .rx_rsp_flit_i(tx_rsp_flit[i][RP_LOCAL]),
            .rx_rsp_ready_o(tx_rsp_ready[i][RP_LOCAL]),
            .tx_dat_valid_o(rx_dat_valid[i][RP_LOCAL]),
            .tx_dat_flit_o(rx_dat_flit[i][RP_LOCAL]),
            .tx_dat_crdvalid_i(rx_dat_crdvalid[i][RP_LOCAL]),
            .rx_dat_valid_i(tx_dat_valid[i][RP_LOCAL]),
            .rx_dat_flit_i(tx_dat_flit[i][RP_LOCAL]),
            .rx_dat_crdvalid_o(tx_dat_crdvalid[i][RP_LOCAL])
        );

        router_wrap #(
            .DAT_NUM_VC(DAT_NUM_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),
            .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH),
            .LINK_PORTS(LINK_PORTS)
        ) u_router (
            .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(router_ctx[i]),
            .tx_req_valid(tx_req_valid[i]), .tx_req_flit(tx_req_flit[i]),
            .tx_req_ready(tx_req_ready[i]),
            .rx_req_valid(rx_req_valid[i]), .rx_req_flit(rx_req_flit[i]),
            .rx_req_ready(rx_req_ready[i]),
            .tx_rsp_valid(tx_rsp_valid[i]), .tx_rsp_flit(tx_rsp_flit[i]),
            .tx_rsp_ready(tx_rsp_ready[i]),
            .rx_rsp_valid(rx_rsp_valid[i]), .rx_rsp_flit(rx_rsp_flit[i]),
            .rx_rsp_ready(rx_rsp_ready[i]),
            .tx_dat_valid(tx_dat_valid[i]), .tx_dat_flit(tx_dat_flit[i]),
            .tx_dat_crdvalid(tx_dat_crdvalid[i]),
            .rx_dat_valid(rx_dat_valid[i]), .rx_dat_flit(rx_dat_flit[i]),
            .rx_dat_crdvalid(rx_dat_crdvalid[i])
        );

        always_comb begin : link_req_in
            for (int p = 1; p < LINK_PORTS; p++) begin
                rx_req_valid[i][p]  = 1'b0;
                rx_req_flit[i][p]   = '0;
                tx_req_ready[i][p]  = '0;
            end
            if (HAS_N) begin  // NORTH: <- peer SOUTH OUT
                rx_req_valid[i][RP_NORTH] = tx_req_valid[PEER_N][RP_SOUTH];
                rx_req_flit[i][RP_NORTH]  = tx_req_flit[PEER_N][RP_SOUTH];
                tx_req_ready[i][RP_NORTH] = rx_req_ready[PEER_N][RP_SOUTH];
            end
            if (HAS_E) begin  // EAST: <- peer WEST OUT
                rx_req_valid[i][RP_EAST] = tx_req_valid[PEER_E][RP_WEST];
                rx_req_flit[i][RP_EAST]  = tx_req_flit[PEER_E][RP_WEST];
                tx_req_ready[i][RP_EAST] = rx_req_ready[PEER_E][RP_WEST];
            end
            if (HAS_S) begin  // SOUTH: <- peer NORTH OUT
                rx_req_valid[i][RP_SOUTH] = tx_req_valid[PEER_S][RP_NORTH];
                rx_req_flit[i][RP_SOUTH]  = tx_req_flit[PEER_S][RP_NORTH];
                tx_req_ready[i][RP_SOUTH] = rx_req_ready[PEER_S][RP_NORTH];
            end
            if (HAS_W) begin  // WEST: <- peer EAST OUT
                rx_req_valid[i][RP_WEST] = tx_req_valid[PEER_W][RP_EAST];
                rx_req_flit[i][RP_WEST]  = tx_req_flit[PEER_W][RP_EAST];
                tx_req_ready[i][RP_WEST] = rx_req_ready[PEER_W][RP_EAST];
            end
            // A populated boundary port takes the peripheral's NI instead of
            // the tie-off default, with the same tx/rx crossing the LOCAL port
            // uses.
            for (int unsigned p = 0; p < N_PERIPH; p++)
                if (periph_host(p) == i) begin
                    rx_req_valid[i][periph_rp(p)] = periph_tx_req_valid[p];
                    rx_req_flit[i][periph_rp(p)]  = periph_tx_req_flit[p];
                    tx_req_ready[i][periph_rp(p)] = periph_rx_req_ready[p];
                end
        end

        always_comb begin : link_rsp_in
            for (int p = 1; p < LINK_PORTS; p++) begin
                rx_rsp_valid[i][p]  = 1'b0;
                rx_rsp_flit[i][p]   = '0;
                tx_rsp_ready[i][p]  = '0;
            end
            if (HAS_N) begin  // NORTH: <- peer SOUTH OUT
                rx_rsp_valid[i][RP_NORTH] = tx_rsp_valid[PEER_N][RP_SOUTH];
                rx_rsp_flit[i][RP_NORTH]  = tx_rsp_flit[PEER_N][RP_SOUTH];
                tx_rsp_ready[i][RP_NORTH] = rx_rsp_ready[PEER_N][RP_SOUTH];
            end
            if (HAS_E) begin  // EAST: <- peer WEST OUT
                rx_rsp_valid[i][RP_EAST] = tx_rsp_valid[PEER_E][RP_WEST];
                rx_rsp_flit[i][RP_EAST]  = tx_rsp_flit[PEER_E][RP_WEST];
                tx_rsp_ready[i][RP_EAST] = rx_rsp_ready[PEER_E][RP_WEST];
            end
            if (HAS_S) begin  // SOUTH: <- peer NORTH OUT
                rx_rsp_valid[i][RP_SOUTH] = tx_rsp_valid[PEER_S][RP_NORTH];
                rx_rsp_flit[i][RP_SOUTH]  = tx_rsp_flit[PEER_S][RP_NORTH];
                tx_rsp_ready[i][RP_SOUTH] = rx_rsp_ready[PEER_S][RP_NORTH];
            end
            if (HAS_W) begin  // WEST: <- peer EAST OUT
                rx_rsp_valid[i][RP_WEST] = tx_rsp_valid[PEER_W][RP_EAST];
                rx_rsp_flit[i][RP_WEST]  = tx_rsp_flit[PEER_W][RP_EAST];
                tx_rsp_ready[i][RP_WEST] = rx_rsp_ready[PEER_W][RP_EAST];
            end
            for (int unsigned p = 0; p < N_PERIPH; p++)
                if (periph_host(p) == i) begin
                    rx_rsp_valid[i][periph_rp(p)] = periph_tx_rsp_valid[p];
                    rx_rsp_flit[i][periph_rp(p)]  = periph_tx_rsp_flit[p];
                    tx_rsp_ready[i][periph_rp(p)] = periph_rx_rsp_ready[p];
                end
        end

        always_comb begin : link_dat_in
            for (int p = 1; p < LINK_PORTS; p++) begin
                rx_dat_valid[i][p]  = 1'b0;
                rx_dat_flit[i][p]   = '0;
                tx_dat_crdvalid[i][p]  = '0;
            end
            if (HAS_N) begin  // NORTH: <- peer SOUTH OUT
                rx_dat_valid[i][RP_NORTH] = tx_dat_valid[PEER_N][RP_SOUTH];
                rx_dat_flit[i][RP_NORTH]  = tx_dat_flit[PEER_N][RP_SOUTH];
                tx_dat_crdvalid[i][RP_NORTH] = rx_dat_crdvalid[PEER_N][RP_SOUTH];
            end
            if (HAS_E) begin  // EAST: <- peer WEST OUT
                rx_dat_valid[i][RP_EAST] = tx_dat_valid[PEER_E][RP_WEST];
                rx_dat_flit[i][RP_EAST]  = tx_dat_flit[PEER_E][RP_WEST];
                tx_dat_crdvalid[i][RP_EAST] = rx_dat_crdvalid[PEER_E][RP_WEST];
            end
            if (HAS_S) begin  // SOUTH: <- peer NORTH OUT
                rx_dat_valid[i][RP_SOUTH] = tx_dat_valid[PEER_S][RP_NORTH];
                rx_dat_flit[i][RP_SOUTH]  = tx_dat_flit[PEER_S][RP_NORTH];
                tx_dat_crdvalid[i][RP_SOUTH] = rx_dat_crdvalid[PEER_S][RP_NORTH];
            end
            if (HAS_W) begin  // WEST: <- peer EAST OUT
                rx_dat_valid[i][RP_WEST] = tx_dat_valid[PEER_W][RP_EAST];
                rx_dat_flit[i][RP_WEST]  = tx_dat_flit[PEER_W][RP_EAST];
                tx_dat_crdvalid[i][RP_WEST] = rx_dat_crdvalid[PEER_W][RP_EAST];
            end
            for (int unsigned p = 0; p < N_PERIPH; p++)
                if (periph_host(p) == i) begin
                    rx_dat_valid[i][periph_rp(p)]    = periph_tx_dat_valid[p];
                    rx_dat_flit[i][periph_rp(p)]     = periph_tx_dat_flit[p];
                    tx_dat_crdvalid[i][periph_rp(p)] = periph_rx_dat_crdvalid[p];
                end
        end

        // Boundary tie-off assertion: a boundary direction (no neighbor, no
        // peripheral) must never drive OUT valid. Fires on a fabric wiring
        // mistake; the C++ route leak (dst outside mesh) is caught upstream by
        // route_compute's abort.
        always_ff @(posedge clk_i) begin
            if (rst_ni) begin
                if (!HAS_N && !periph_on(i, RP_NORTH) && tx_req_valid[i][RP_NORTH])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off NORTH (req) - fabric link wiring mistake", i);
                if (!HAS_N && !periph_on(i, RP_NORTH) && tx_rsp_valid[i][RP_NORTH])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off NORTH (rsp) - fabric link wiring mistake", i);
                if (!HAS_N && !periph_on(i, RP_NORTH) && tx_dat_valid[i][RP_NORTH])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off NORTH (dat) - fabric link wiring mistake", i);
                if (!HAS_E && !periph_on(i, RP_EAST) && tx_req_valid[i][RP_EAST])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off EAST (req) - fabric link wiring mistake", i);
                if (!HAS_E && !periph_on(i, RP_EAST) && tx_rsp_valid[i][RP_EAST])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off EAST (rsp) - fabric link wiring mistake", i);
                if (!HAS_E && !periph_on(i, RP_EAST) && tx_dat_valid[i][RP_EAST])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off EAST (dat) - fabric link wiring mistake", i);
                if (!HAS_S && !periph_on(i, RP_SOUTH) && tx_req_valid[i][RP_SOUTH])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off SOUTH (req) - fabric link wiring mistake", i);
                if (!HAS_S && !periph_on(i, RP_SOUTH) && tx_rsp_valid[i][RP_SOUTH])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off SOUTH (rsp) - fabric link wiring mistake", i);
                if (!HAS_S && !periph_on(i, RP_SOUTH) && tx_dat_valid[i][RP_SOUTH])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off SOUTH (dat) - fabric link wiring mistake", i);
                if (!HAS_W && !periph_on(i, RP_WEST) && tx_req_valid[i][RP_WEST])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off WEST (req) - fabric link wiring mistake", i);
                if (!HAS_W && !periph_on(i, RP_WEST) && tx_rsp_valid[i][RP_WEST])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off WEST (rsp) - fabric link wiring mistake", i);
                if (!HAS_W && !periph_on(i, RP_WEST) && tx_dat_valid[i][RP_WEST])
                    $fatal(1, "noc_fabric: node%0d drove a flit on tied-off WEST (dat) - fabric link wiring mistake", i);
        end
        end

        // Inter-router link perf monitors (passive): one per live
        // neighbor direction, named {net}_{i}to{peer}. vc_id bit window
        // from ni_flit_pkg; DAT credit_pulse is per-VC (not OR-collapsed).
        if (HAS_N) begin : g_perf_north
            link_perf_monitor #(
                .LINK_NAME($sformatf("req_%0dto%0d", i, PEER_N)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_req (
                .clk_i, .rst_ni,
                .valid(tx_req_valid[i][RP_NORTH]),
                .ready(tx_req_ready[i][RP_NORTH]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("rsp_%0dto%0d", i, PEER_N)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_rsp (
                .clk_i, .rst_ni,
                .valid(tx_rsp_valid[i][RP_NORTH]),
                .ready(tx_rsp_ready[i][RP_NORTH]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("dat_%0dto%0d", i, PEER_N)),
                .FLOW("credit"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_dat (
                .clk_i, .rst_ni,
                .valid(tx_dat_valid[i][RP_NORTH]),
                .ready(1'b0),
                .vc_id(tx_dat_flit[i][RP_NORTH][ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]),
                .credit_pulse(tx_dat_crdvalid[i][RP_NORTH])
            );
        end
        if (HAS_E) begin : g_perf_east
            link_perf_monitor #(
                .LINK_NAME($sformatf("req_%0dto%0d", i, PEER_E)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_req (
                .clk_i, .rst_ni,
                .valid(tx_req_valid[i][RP_EAST]),
                .ready(tx_req_ready[i][RP_EAST]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("rsp_%0dto%0d", i, PEER_E)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_rsp (
                .clk_i, .rst_ni,
                .valid(tx_rsp_valid[i][RP_EAST]),
                .ready(tx_rsp_ready[i][RP_EAST]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("dat_%0dto%0d", i, PEER_E)),
                .FLOW("credit"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_dat (
                .clk_i, .rst_ni,
                .valid(tx_dat_valid[i][RP_EAST]),
                .ready(1'b0),
                .vc_id(tx_dat_flit[i][RP_EAST][ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]),
                .credit_pulse(tx_dat_crdvalid[i][RP_EAST])
            );
        end
        if (HAS_S) begin : g_perf_south
            link_perf_monitor #(
                .LINK_NAME($sformatf("req_%0dto%0d", i, PEER_S)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_req (
                .clk_i, .rst_ni,
                .valid(tx_req_valid[i][RP_SOUTH]),
                .ready(tx_req_ready[i][RP_SOUTH]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("rsp_%0dto%0d", i, PEER_S)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_rsp (
                .clk_i, .rst_ni,
                .valid(tx_rsp_valid[i][RP_SOUTH]),
                .ready(tx_rsp_ready[i][RP_SOUTH]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("dat_%0dto%0d", i, PEER_S)),
                .FLOW("credit"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_dat (
                .clk_i, .rst_ni,
                .valid(tx_dat_valid[i][RP_SOUTH]),
                .ready(1'b0),
                .vc_id(tx_dat_flit[i][RP_SOUTH][ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]),
                .credit_pulse(tx_dat_crdvalid[i][RP_SOUTH])
            );
        end
        if (HAS_W) begin : g_perf_west
            link_perf_monitor #(
                .LINK_NAME($sformatf("req_%0dto%0d", i, PEER_W)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_req (
                .clk_i, .rst_ni,
                .valid(tx_req_valid[i][RP_WEST]),
                .ready(tx_req_ready[i][RP_WEST]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("rsp_%0dto%0d", i, PEER_W)),
                .FLOW("ready_valid"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_rsp (
                .clk_i, .rst_ni,
                .valid(tx_rsp_valid[i][RP_WEST]),
                .ready(tx_rsp_ready[i][RP_WEST]),
                .vc_id('0),
                .credit_pulse('0)
            );
            link_perf_monitor #(
                .LINK_NAME($sformatf("dat_%0dto%0d", i, PEER_W)),
                .FLOW("credit"),
                .BUFFER_DEPTH(ROUTER_VC_DEPTH),
                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)
            ) u_perf_link_dat (
                .clk_i, .rst_ni,
                .valid(tx_dat_valid[i][RP_WEST]),
                .ready(1'b0),
                .vc_id(tx_dat_flit[i][RP_WEST][ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]),
                .credit_pulse(tx_dat_crdvalid[i][RP_WEST])
            );
        end
    end : g_node

endmodule

`endif  // NOC_FABRIC_SV
