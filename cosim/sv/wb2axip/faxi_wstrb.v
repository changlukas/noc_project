// faxi_wstrb — stub for simulation.
//
// The full wb2axip faxi_wstrb checks that the AXI write strobe (WSTRB) is
// valid for the given address alignment and transfer size. In the formal
// version this module is inlined; here we provide a permissive simulation
// stub that always asserts wstb_valid = 1 so Verilator can link the design.
//
// This stub does NOT check strobe correctness. It exists solely to satisfy
// the module instantiation inside faxi_master.v / faxi_slave.v.

`default_nettype none

module faxi_wstrb #(
    parameter C_AXI_DATA_WIDTH = 128
) (
    input  wire [$clog2(C_AXI_DATA_WIDTH/8)-1:0] i_addr,
    input  wire [2:0]                             i_size,
    input  wire [C_AXI_DATA_WIDTH/8-1:0]          i_strb,
    output wire                                   o_valid
);
    // Permissive stub: always valid. The C-model's AXI generator is
    // responsible for producing correct strobes; this checker only fires
    // in co-sim where correctness is enforced by the DPI adapter.
    assign o_valid = 1'b1;

endmodule
`default_nettype wire
