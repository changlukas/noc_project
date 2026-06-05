// AXI4 bundle interface with full handshake (valid/ready per channel) and
// master/slave modports for direction-safe instantiation.
//
// Carries fields per Stage 5b spec §6.1:
//   AW: awvalid, awready, awid, awaddr, awlen, awsize, awburst, awlock,
//       awcache, awprot, awqos
//   W:  wvalid, wready, wlast, wdata, wstrb
//   B:  bvalid, bready, bid, bresp
//   AR: arvalid, arready, arid, araddr, arlen, arsize, arburst, arlock,
//       arcache, arprot, arqos
//   R:  rvalid, rready, rlast, rid, rdata, rresp
//
// Excluded per Stage 5b scope: region, user (not checked by wb2axip;
// see KNOWN_LIMITATIONS).
//
// Default widths per project canonical (ni_flit_constants.h):
//   ID_WIDTH=8, ADDR_WIDTH=64, DATA_WIDTH=256

`ifndef AXI_INTF_SV
`define AXI_INTF_SV

interface axi_intf #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input logic clk_i,
    input logic rst_ni
);
    // AW channel
    logic                    awvalid;
    logic                    awready;
    logic [ID_WIDTH-1:0]     awid;
    logic [ADDR_WIDTH-1:0]   awaddr;
    logic [7:0]              awlen;
    logic [2:0]              awsize;
    logic [1:0]              awburst;
    logic                    awlock;
    logic [3:0]              awcache;
    logic [2:0]              awprot;
    logic [3:0]              awqos;
    // W channel
    logic                    wvalid;
    logic                    wready;
    logic                    wlast;
    logic [DATA_WIDTH-1:0]   wdata;
    logic [DATA_WIDTH/8-1:0] wstrb;   // DATA_WIDTH must be a multiple of 8 (AXI4 byte-lane)
    // B channel
    logic                    bvalid;
    logic                    bready;
    logic [ID_WIDTH-1:0]     bid;
    logic [1:0]              bresp;
    // AR channel
    logic                    arvalid;
    logic                    arready;
    logic [ID_WIDTH-1:0]     arid;
    logic [ADDR_WIDTH-1:0]   araddr;
    logic [7:0]              arlen;
    logic [2:0]              arsize;
    logic [1:0]              arburst;
    logic                    arlock;
    logic [3:0]              arcache;
    logic [2:0]              arprot;
    logic [3:0]              arqos;
    // R channel
    logic                    rvalid;
    logic                    rready;
    logic                    rlast;
    logic [ID_WIDTH-1:0]     rid;
    logic [DATA_WIDTH-1:0]   rdata;
    logic [1:0]              rresp;

    // master modport: drives AW/W/AR + bready/rready; reads ready signals + B/R
    modport master (
        output awvalid, awid, awaddr, awlen, awsize, awburst,
               awlock, awcache, awprot, awqos,
        output wvalid, wdata, wstrb, wlast,
        output arvalid, arid, araddr, arlen, arsize, arburst,
               arlock, arcache, arprot, arqos,
        output bready,
        output rready,
        input  awready,
        input  wready,
        input  arready,
        input  bvalid, bid, bresp,
        input  rvalid, rid, rdata, rresp, rlast
    );

    // slave modport: inverse of master
    modport slave (
        input  awvalid, awid, awaddr, awlen, awsize, awburst,
               awlock, awcache, awprot, awqos,
        input  wvalid, wdata, wstrb, wlast,
        input  arvalid, arid, araddr, arlen, arsize, arburst,
               arlock, arcache, arprot, arqos,
        input  bready,
        input  rready,
        output awready,
        output wready,
        output arready,
        output bvalid, bid, bresp,
        output rvalid, rid, rdata, rresp, rlast
    );

endinterface

`endif // AXI_INTF_SV
