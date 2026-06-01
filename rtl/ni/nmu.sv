// NMU (Network Manager Unit) — top-level shell
//
// AXI4 manager-side network interface for AXI4-over-NoC fabric.
// Receives AXI4 requests from a local AXI master, packetises them into NoC
// flits on noc_req_o, and de-packetises NoC response flits from noc_rsp_i back
// into AXI4 B / R responses. Per the per-tile single-NI pattern, the NMU
// shares the physical NoC link pair with a sibling NSU at the NI top level.
//
// Spec source-of-truth: noc-sim/spec/ni/doc/signal_interface.md
//
// Empty implementation shell — module body intentionally left blank.

module nmu #(
  parameter int unsigned ADDR_WIDTH         = 64,
  parameter int unsigned DATA_WIDTH         = 256,
  parameter int unsigned USER_WIDTH         = 8,
  parameter int unsigned IN_ID_WIDTH        = 8,
  parameter int unsigned X_WIDTH            = 4,
  parameter int unsigned Y_WIDTH            = 4,
  parameter int unsigned NUM_VC             = 1,
  parameter int unsigned FLIT_WIDTH         = 406,
  parameter int unsigned MAX_TXNS           = 32,
  parameter int unsigned MAX_TXNS_PER_ID    = 32,
  parameter int unsigned MAX_BURST_LEN      = 16,
  parameter int unsigned CDC_FIFO_DEPTH     = 16,
  parameter bit          ENABLE_AXI_PARITY  = 1'b1,
  // localparams
  localparam int unsigned ID_WIDTH          = X_WIDTH + Y_WIDTH,
  localparam int unsigned STRB_WIDTH        = DATA_WIDTH / 8,
  localparam int unsigned ADDR_PAR_WIDTH    = ADDR_WIDTH / 8,
  localparam int unsigned DATA_PAR_WIDTH    = DATA_WIDTH / 8
) (
  // Clocks and resets
  input  logic                          aclk_i,
  input  logic                          arst_ni,
  input  logic                          noc_clk_i,
  input  logic                          noc_rst_ni,

  // Sideband — this NI's tile coordinate
  input  logic [ID_WIDTH-1:0]           id_i,

  // AXI4 Manager port (local AXI master → NMU)
  // AW channel
  input  logic                          axi_awvalid_i,
  output logic                          axi_awready_o,
  input  logic [IN_ID_WIDTH-1:0]        axi_awid_i,
  input  logic [ADDR_WIDTH-1:0]         axi_awaddr_i,
  input  logic [7:0]                    axi_awlen_i,
  input  logic [2:0]                    axi_awsize_i,
  input  logic [1:0]                    axi_awburst_i,
  input  logic                          axi_awlock_i,
  input  logic [3:0]                    axi_awcache_i,
  input  logic [2:0]                    axi_awprot_i,
  input  logic [3:0]                    axi_awqos_i,
  input  logic [3:0]                    axi_awregion_i,
  input  logic [USER_WIDTH-1:0]         axi_awuser_i,
  input  logic [5:0]                    axi_awatop_i,
  // W channel
  input  logic                          axi_wvalid_i,
  output logic                          axi_wready_o,
  input  logic [DATA_WIDTH-1:0]         axi_wdata_i,
  input  logic [STRB_WIDTH-1:0]         axi_wstrb_i,
  input  logic                          axi_wlast_i,
  input  logic [USER_WIDTH-1:0]         axi_wuser_i,
  // B channel
  output logic                          axi_bvalid_o,
  input  logic                          axi_bready_i,
  output logic [IN_ID_WIDTH-1:0]        axi_bid_o,
  output logic [1:0]                    axi_bresp_o,
  output logic [USER_WIDTH-1:0]         axi_buser_o,
  // AR channel
  input  logic                          axi_arvalid_i,
  output logic                          axi_arready_o,
  input  logic [IN_ID_WIDTH-1:0]        axi_arid_i,
  input  logic [ADDR_WIDTH-1:0]         axi_araddr_i,
  input  logic [7:0]                    axi_arlen_i,
  input  logic [2:0]                    axi_arsize_i,
  input  logic [1:0]                    axi_arburst_i,
  input  logic                          axi_arlock_i,
  input  logic [3:0]                    axi_arcache_i,
  input  logic [2:0]                    axi_arprot_i,
  input  logic [3:0]                    axi_arqos_i,
  input  logic [3:0]                    axi_arregion_i,
  input  logic [USER_WIDTH-1:0]         axi_aruser_i,
  // R channel
  output logic                          axi_rvalid_o,
  input  logic                          axi_rready_i,
  output logic [IN_ID_WIDTH-1:0]        axi_rid_o,
  output logic [DATA_WIDTH-1:0]         axi_rdata_o,
  output logic [1:0]                    axi_rresp_o,
  output logic                          axi_rlast_o,
  output logic [USER_WIDTH-1:0]         axi_ruser_o,

  // Optional AXI parity sideband (manager port)
  // Present in the wire list when ENABLE_AXI_PARITY=1; tie 0 when disabled.
  input  logic [ADDR_PAR_WIDTH-1:0]     axi_awaddr_par_i,
  input  logic [ADDR_PAR_WIDTH-1:0]     axi_araddr_par_i,
  input  logic [DATA_PAR_WIDTH-1:0]     axi_wdata_par_i,
  output logic [DATA_PAR_WIDTH-1:0]     axi_rdata_par_o,

  // NoC request output link (NMU → router)
  output logic                          noc_req_valid_o,
  output logic [FLIT_WIDTH-1:0]         noc_req_flit_o,
  input  logic [NUM_VC-1:0]             noc_req_credit_i,
  output logic                          noc_req_credit_init_ready_o,
  input  logic                          noc_req_credit_init_ready_i,

  // NoC response input link (router → NMU)
  input  logic                          noc_rsp_valid_i,
  input  logic [FLIT_WIDTH-1:0]         noc_rsp_flit_i,
  output logic [NUM_VC-1:0]             noc_rsp_credit_o
);

  // TODO: implementation
  //   AddrTrans → QoSGen → FlitPack → ECC Gen → Injection Buffer → VC Mapping
  //   ECC Check ← FlitUnpack ← RoB ← async-FIFO

endmodule
