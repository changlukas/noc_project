// NoC request bundle. Credit-based (no AXI-style ready); credit_return is
// a reverse-direction per-VC signal from consumer to producer.
//
// Parameters per Stage 5b spec §6.2:
//   NUM_VC  — number of virtual channels; credit_return width
//   FLIT_W  — flit data width in bits
//
// Default: NUM_VC=1, FLIT_W=256 (project PoC values; multi-VC via top-level
// localparam override).

`ifndef NOC_REQ_INTF_SV
`define NOC_REQ_INTF_SV

interface noc_req_intf #(
    parameter int NUM_VC = 1,
    parameter int FLIT_W = 256
) (
    input logic clk_i,
    input logic rst_ni
);
    logic              valid;
    logic [FLIT_W-1:0] flit;
    logic [NUM_VC-1:0] credit_return;

    modport producer (
        output valid,
        output flit,
        input  credit_return
    );

    modport consumer (
        input  valid,
        input  flit,
        output credit_return
    );

endinterface

`endif // NOC_REQ_INTF_SV
