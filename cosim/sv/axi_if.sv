// Parameterized AXI4 bundle interface used to wire the C-model proxy
// outputs into the wb2axip checker inputs. Defaults are this project's
// canonical widths (ni_flit_constants.h: AXI_ID_WIDTH=8, AXI_ADDR_WIDTH=64,
// NOC_DATA_WIDTH=256, WSTRB_WIDTH=32 = DATA_WIDTH/8).

interface axi_if #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input logic clk,
    input logic rst_n
);
    // AW
    logic                  awvalid, awready;
    logic [ID_WIDTH-1:0]   awid;
    logic [ADDR_WIDTH-1:0] awaddr;
    logic [7:0]            awlen;
    logic [2:0]            awsize;
    logic [1:0]            awburst;
    logic                  awlock;
    logic [3:0]            awcache;
    logic [2:0]            awprot;
    logic [3:0]            awqos;
    // W
    logic                    wvalid, wready, wlast;
    logic [DATA_WIDTH-1:0]   wdata;
    logic [DATA_WIDTH/8-1:0] wstrb;
    // B
    logic                  bvalid, bready;
    logic [ID_WIDTH-1:0]   bid;
    logic [1:0]            bresp;
    // AR
    logic                  arvalid, arready;
    logic [ID_WIDTH-1:0]   arid;
    logic [ADDR_WIDTH-1:0] araddr;
    logic [7:0]            arlen;
    logic [2:0]            arsize;
    logic [1:0]            arburst;
    logic                  arlock;
    logic [3:0]            arcache;
    logic [2:0]            arprot;
    logic [3:0]            arqos;
    // R
    logic                  rvalid, rready, rlast;
    logic [ID_WIDTH-1:0]   rid;
    logic [DATA_WIDTH-1:0] rdata;
    logic [1:0]            rresp;
endinterface
