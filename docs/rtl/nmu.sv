// NMU (Network Master Unit) — interface skeleton per docs/noc-target-spec.md §4.
// AXI slave interface toward the AXI master, NoC initiator interface toward the fabric.
// Ports and parameters only, for block-diagram generation. No implementation.

module nmu #(
    parameter int unsigned AXI_ADDR_WIDTH = 48,   // §5 fixed
    parameter int unsigned AXI_ID_WIDTH   = 3,    // §5 fixed
    parameter int unsigned AXI_DATA_WIDTH = 512,  // 64 narrow class, 512 data class
    parameter int unsigned AWUSER_WIDTH   = 58,   // 50 b collective attributes, §6
    parameter int unsigned XUSER_WIDTH    = 8,    // ARUSER / WUSER / RUSER / BUSER
    parameter int unsigned NUM_VC         = 1,    // §5, 1-8
    // Flit widths, §6: 44 b header + the network's largest payload. Fixed.
    localparam int unsigned REQ_FLIT_WIDTH = 132, // 44 + 88  (Aw/Ar)
    localparam int unsigned RSP_FLIT_WIDTH = 122, // 44 + 78  (NarrowR)
    localparam int unsigned DAT_FLIT_WIDTH = 629  // 44 + 585 (DataW)
) (
    // ----- global, §4.1: two asynchronous domains, CDC at the AXI boundary -----
    input  logic                        ACLK,
    input  logic                        ARESETn,
    input  logic                        noc_clk,
    input  logic                        noc_rst_n,

    // ----- AXI slave port, §4.2: write address channel -----
    input  logic [AXI_ID_WIDTH-1:0]     AWID,
    input  logic [AXI_ADDR_WIDTH-1:0]   AWADDR,
    input  logic [7:0]                  AWLEN,
    input  logic [2:0]                  AWSIZE,
    input  logic [1:0]                  AWBURST,
    input  logic                        AWLOCK,
    input  logic [3:0]                  AWCACHE,
    input  logic [2:0]                  AWPROT,
    input  logic [3:0]                  AWQOS,
    input  logic [3:0]                  AWREGION,
    input  logic [AWUSER_WIDTH-1:0]     AWUSER,
    input  logic                        AWVALID,
    output logic                        AWREADY,

    // ----- write data channel -----
    input  logic [AXI_DATA_WIDTH-1:0]   WDATA,
    input  logic [AXI_DATA_WIDTH/8-1:0] WSTRB,
    input  logic                        WLAST,
    input  logic [XUSER_WIDTH-1:0]      WUSER,
    input  logic                        WVALID,
    output logic                        WREADY,

    // ----- write response channel -----
    output logic [AXI_ID_WIDTH-1:0]     BID,
    output logic [1:0]                  BRESP,
    output logic [XUSER_WIDTH-1:0]      BUSER,
    output logic                        BVALID,
    input  logic                        BREADY,

    // ----- read address channel -----
    input  logic [AXI_ID_WIDTH-1:0]     ARID,
    input  logic [AXI_ADDR_WIDTH-1:0]   ARADDR,
    input  logic [7:0]                  ARLEN,
    input  logic [2:0]                  ARSIZE,
    input  logic [1:0]                  ARBURST,
    input  logic                        ARLOCK,
    input  logic [3:0]                  ARCACHE,
    input  logic [2:0]                  ARPROT,
    input  logic [3:0]                  ARQOS,
    input  logic [3:0]                  ARREGION,
    input  logic [XUSER_WIDTH-1:0]      ARUSER,
    input  logic                        ARVALID,
    output logic                        ARREADY,

    // ----- read data channel -----
    output logic [AXI_ID_WIDTH-1:0]     RID,
    output logic [AXI_DATA_WIDTH-1:0]   RDATA,
    output logic [1:0]                  RRESP,
    output logic                        RLAST,
    output logic [XUSER_WIDTH-1:0]      RUSER,
    output logic                        RVALID,
    input  logic                        RREADY,

    // ----- NoC initiator port, §4.3: REQ, ready/valid -----
    output logic [REQ_FLIT_WIDTH-1:0]   TXREQFLIT,
    output logic                        TXREQVALID,
    input  logic                        TXREQREADY,
    input  logic [REQ_FLIT_WIDTH-1:0]   RXREQFLIT,
    input  logic                        RXREQVALID,
    output logic                        RXREQREADY,

    // ----- RSP, ready/valid -----
    output logic [RSP_FLIT_WIDTH-1:0]   TXRSPFLIT,
    output logic                        TXRSPVALID,
    input  logic                        TXRSPREADY,
    input  logic [RSP_FLIT_WIDTH-1:0]   RXRSPFLIT,
    input  logic                        RXRSPVALID,
    output logic                        RXRSPREADY,

    // ----- DAT, credit-based, no ready -----
    output logic [DAT_FLIT_WIDTH-1:0]   TXDATFLIT,
    output logic                        TXDATVALID,
    input  logic [NUM_VC-1:0]           TXDATCRDVALID,
    input  logic [DAT_FLIT_WIDTH-1:0]   RXDATFLIT,
    input  logic                        RXDATVALID,
    output logic [NUM_VC-1:0]           RXDATCRDVALID
);

endmodule
