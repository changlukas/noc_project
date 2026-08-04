// NSU (Network Slave Unit) — interface skeleton per docs/noc-target-spec.md §4.
// NoC target interface toward the fabric, AXI master interface toward the AXI slave.
// Ports and parameters only, for block-diagram generation. No implementation.

module nsu #(
    parameter int unsigned AXI_ADDR_WIDTH = 48,   // §5 fixed
    parameter int unsigned AXI_ID_WIDTH   = 8,    // §5 fixed
    parameter int unsigned AXI_DATA_WIDTH = 512,  // 64 narrow class, 512 data class
    parameter int unsigned AWUSER_WIDTH   = 58,   // collective bits arrive cleared, §6.1
    parameter int unsigned XUSER_WIDTH    = 8,    // ARUSER / WUSER / RUSER / BUSER
    parameter int unsigned NUM_VC         = 1,    // §5, 1-8
    // Flit widths, §6: 44 b header + the network's largest payload. Fixed.
    localparam int unsigned REQ_FLIT_WIDTH = 137, // 44 + 93  (Aw/Ar)
    localparam int unsigned RSP_FLIT_WIDTH = 127, // 44 + 83  (NarrowR)
    localparam int unsigned DAT_FLIT_WIDTH = 629  // 44 + 585 (DataW)
) (
    // ----- global, §4.1: two asynchronous domains, CDC at the AXI boundary -----
    input  logic                        ACLK,
    input  logic                        ARESETn,
    input  logic                        noc_clk,
    input  logic                        noc_rst_n,

    // ----- NoC target port, §4.3: REQ, ready/valid -----
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
    output logic [NUM_VC-1:0]           RXDATCRDVALID,

    // ----- AXI master port, §4.2: write address channel -----
    output logic [AXI_ID_WIDTH-1:0]     AWID,
    output logic [AXI_ADDR_WIDTH-1:0]   AWADDR,
    output logic [7:0]                  AWLEN,
    output logic [2:0]                  AWSIZE,
    output logic [1:0]                  AWBURST,
    output logic                        AWLOCK,
    output logic [3:0]                  AWCACHE,
    output logic [2:0]                  AWPROT,
    output logic [3:0]                  AWQOS,
    output logic [3:0]                  AWREGION,
    output logic [AWUSER_WIDTH-1:0]     AWUSER,
    output logic                        AWVALID,
    input  logic                        AWREADY,

    // ----- write data channel -----
    output logic [AXI_DATA_WIDTH-1:0]   WDATA,
    output logic [AXI_DATA_WIDTH/8-1:0] WSTRB,
    output logic                        WLAST,
    output logic [XUSER_WIDTH-1:0]      WUSER,
    output logic                        WVALID,
    input  logic                        WREADY,

    // ----- write response channel -----
    input  logic [AXI_ID_WIDTH-1:0]     BID,
    input  logic [1:0]                  BRESP,
    input  logic [XUSER_WIDTH-1:0]      BUSER,
    input  logic                        BVALID,
    output logic                        BREADY,

    // ----- read address channel -----
    output logic [AXI_ID_WIDTH-1:0]     ARID,
    output logic [AXI_ADDR_WIDTH-1:0]   ARADDR,
    output logic [7:0]                  ARLEN,
    output logic [2:0]                  ARSIZE,
    output logic [1:0]                  ARBURST,
    output logic                        ARLOCK,
    output logic [3:0]                  ARCACHE,
    output logic [2:0]                  ARPROT,
    output logic [3:0]                  ARQOS,
    output logic [3:0]                  ARREGION,
    output logic [XUSER_WIDTH-1:0]      ARUSER,
    output logic                        ARVALID,
    input  logic                        ARREADY,

    // ----- read data channel -----
    input  logic [AXI_ID_WIDTH-1:0]     RID,
    input  logic [AXI_DATA_WIDTH-1:0]   RDATA,
    input  logic [1:0]                  RRESP,
    input  logic                        RLAST,
    input  logic [XUSER_WIDTH-1:0]      RUSER,
    input  logic                        RVALID,
    output logic                        RREADY
);

endmodule
