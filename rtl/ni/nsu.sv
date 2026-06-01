// NSU (Network Subordinate Unit) — top-level shell
//
// AXI4 subordinate-side network interface for AXI4-over-NoC fabric.
// Receives request flits from noc_req_i, de-packetises them into AXI4
// transactions driven to a local AXI slave, and packetises B / R responses
// back onto noc_rsp_o. Per the per-tile single-NI pattern, the NSU
// shares the physical NoC link pair with a sibling NMU at the NI top level.
//
// Spec source-of-truth: noc-sim/spec/ni/doc/signal_interface.md
//
// Empty implementation shell — module body intentionally left blank.

module nsu #(
  parameter int unsigned ADDR_WIDTH         = 64,
  parameter int unsigned DATA_WIDTH         = 256,
  parameter int unsigned USER_WIDTH         = 8,
  parameter int unsigned OUT_ID_WIDTH       = 8,
  parameter int unsigned X_WIDTH            = 4,
  parameter int unsigned Y_WIDTH            = 4,
  parameter int unsigned NUM_VC             = 1,
  parameter int unsigned FLIT_WIDTH         = 406,
  parameter int unsigned MAX_TXNS           = 32,
  parameter int unsigned MAX_BURST_LEN      = 16,
  parameter int unsigned NSU_R_BUFFER_DEPTH = 16,
  parameter int unsigned EXCLUSIVE_MONITOR_DEPTH = 8,
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

  // AXI4 Subordinate port (NSU → local AXI slave)
  // AW channel
  output logic                          axi_awvalid_o,
  input  logic                          axi_awready_i,
  output logic [OUT_ID_WIDTH-1:0]       axi_awid_o,
  output logic [ADDR_WIDTH-1:0]         axi_awaddr_o,
  output logic [7:0]                    axi_awlen_o,
  output logic [2:0]                    axi_awsize_o,
  output logic [1:0]                    axi_awburst_o,
  output logic                          axi_awlock_o,
  output logic [3:0]                    axi_awcache_o,
  output logic [2:0]                    axi_awprot_o,
  output logic [3:0]                    axi_awqos_o,
  output logic [3:0]                    axi_awregion_o,
  output logic [USER_WIDTH-1:0]         axi_awuser_o,
  // W channel
  output logic                          axi_wvalid_o,
  input  logic                          axi_wready_i,
  output logic [DATA_WIDTH-1:0]         axi_wdata_o,
  output logic [STRB_WIDTH-1:0]         axi_wstrb_o,
  output logic                          axi_wlast_o,
  output logic [USER_WIDTH-1:0]         axi_wuser_o,
  // B channel
  input  logic                          axi_bvalid_i,
  output logic                          axi_bready_o,
  input  logic [OUT_ID_WIDTH-1:0]       axi_bid_i,
  input  logic [1:0]                    axi_bresp_i,
  input  logic [USER_WIDTH-1:0]         axi_buser_i,
  // AR channel
  output logic                          axi_arvalid_o,
  input  logic                          axi_arready_i,
  output logic [OUT_ID_WIDTH-1:0]       axi_arid_o,
  output logic [ADDR_WIDTH-1:0]         axi_araddr_o,
  output logic [7:0]                    axi_arlen_o,
  output logic [2:0]                    axi_arsize_o,
  output logic [1:0]                    axi_arburst_o,
  output logic                          axi_arlock_o,
  output logic [3:0]                    axi_arcache_o,
  output logic [2:0]                    axi_arprot_o,
  output logic [3:0]                    axi_arqos_o,
  output logic [3:0]                    axi_arregion_o,
  output logic [USER_WIDTH-1:0]         axi_aruser_o,
  // R channel
  input  logic                          axi_rvalid_i,
  output logic                          axi_rready_o,
  input  logic [OUT_ID_WIDTH-1:0]       axi_rid_i,
  input  logic [DATA_WIDTH-1:0]         axi_rdata_i,
  input  logic [1:0]                    axi_rresp_i,
  input  logic                          axi_rlast_i,
  input  logic [USER_WIDTH-1:0]         axi_ruser_i,

  // Optional AXI parity sideband (subordinate port)
  // Present in the wire list when ENABLE_AXI_PARITY=1; tie 0 when disabled.
  output logic [ADDR_PAR_WIDTH-1:0]     axi_awaddr_par_o,
  output logic [ADDR_PAR_WIDTH-1:0]     axi_araddr_par_o,
  output logic [DATA_PAR_WIDTH-1:0]     axi_wdata_par_o,
  input  logic [DATA_PAR_WIDTH-1:0]     axi_rdata_par_i,

  // NoC request input link (router → NSU)
  input  logic                          noc_req_valid_i,
  input  logic [FLIT_WIDTH-1:0]         noc_req_flit_i,
  output logic [NUM_VC-1:0]             noc_req_credit_o,

  // NoC response output link (NSU → router)
  output logic                          noc_rsp_valid_o,
  output logic [FLIT_WIDTH-1:0]         noc_rsp_flit_o,
  input  logic [NUM_VC-1:0]             noc_rsp_credit_i,
  output logic                          noc_rsp_credit_init_ready_o,
  input  logic                          noc_rsp_credit_init_ready_i
);

  // TODO: implementation
  //   ECC Check → De-packetizing → W Reassembly → Downsize → AXI subordinate
  //   ECC Gen ← Packetizing B/R ← Read Resp Buffer ← Exclusive Monitor

endmodule
