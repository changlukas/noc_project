// Router — interface skeleton per docs/noc-target-spec.md §4.
// One node instantiates one standard-mode router on DAT and one simple-mode
// router on each of REQ and RSP (§3). This skeleton shows the merged per-node
// pin set: five ports (LOCAL + N/E/S/W), each carrying the full §4.3 link group.
// Ports and parameters only, for block-diagram generation. No implementation.

module router #(
    parameter int unsigned NUM_VC    = 1,          // §5, 1-8, DAT only
    parameter int unsigned NUM_PORTS = 5,          // LOCAL, N, E, S, W
    // Flit widths, §6: 48 b header + the network's largest payload. Fixed.
    localparam int unsigned REQ_FLIT_WIDTH = 136,  // 48 + 88  (Aw/Ar)
    localparam int unsigned RSP_FLIT_WIDTH = 126,  // 48 + 78  (NarrowR)
    localparam int unsigned DAT_FLIT_WIDTH = 633   // 48 + 585 (DataW)
) (
    // ----- global, §4.1 -----
    input  logic                        noc_clk,
    input  logic                        noc_rst_n,

    // ----- REQ network, ready/valid, per port -----
    output logic [REQ_FLIT_WIDTH-1:0]   TXREQFLIT  [NUM_PORTS],
    output logic [NUM_PORTS-1:0]        TXREQVALID,
    input  logic [NUM_PORTS-1:0]        TXREQREADY,
    input  logic [REQ_FLIT_WIDTH-1:0]   RXREQFLIT  [NUM_PORTS],
    input  logic [NUM_PORTS-1:0]        RXREQVALID,
    output logic [NUM_PORTS-1:0]        RXREQREADY,

    // ----- RSP network, ready/valid, per port -----
    output logic [RSP_FLIT_WIDTH-1:0]   TXRSPFLIT  [NUM_PORTS],
    output logic [NUM_PORTS-1:0]        TXRSPVALID,
    input  logic [NUM_PORTS-1:0]        TXRSPREADY,
    input  logic [RSP_FLIT_WIDTH-1:0]   RXRSPFLIT  [NUM_PORTS],
    input  logic [NUM_PORTS-1:0]        RXRSPVALID,
    output logic [NUM_PORTS-1:0]        RXRSPREADY,

    // ----- DAT network, credit-based, per port -----
    output logic [DAT_FLIT_WIDTH-1:0]   TXDATFLIT  [NUM_PORTS],
    output logic [NUM_PORTS-1:0]        TXDATVALID,
    input  logic [NUM_VC-1:0]           TXDATCRDVALID [NUM_PORTS],
    input  logic [DAT_FLIT_WIDTH-1:0]   RXDATFLIT     [NUM_PORTS],
    input  logic [NUM_PORTS-1:0]        RXDATVALID,
    output logic [NUM_VC-1:0]           RXDATCRDVALID [NUM_PORTS]
);

endmodule
