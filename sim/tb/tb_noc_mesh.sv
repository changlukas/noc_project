`timescale 1ns/1ps

// The one testbench. Geometry and address map come from topology_pkg, generated
// from the sim/configs/<CONFIG>.yml the build selected; the DAT VC count and the
// NMU read RoB mode come from specgen/source/constants.yaml through
// noc_tb_top's own defaults. Nothing here varies per configuration, which is why
// there is one file instead of eight.
//
// The package name is fixed and its contents follow CONFIG, so two
// configurations cannot coexist in a build tree without regenerating -- the
// same property FlooNoC's generated packages have.
//
// The module is named tb_top so --top-module needs no per-configuration value.

module tb_top;

    import topology_pkg::*;

    // Tile-memory latency profile "ideal": the delayer in front of each memory
    // is wires, so the memory is never the bottleneck and a run measures the
    // fabric rather than the model of a DRAM this project does not have.
    localparam bit          MEM_STALL_RANDOM_INPUT  = 1'b0;
    localparam bit          MEM_STALL_RANDOM_OUTPUT = 1'b0;
    localparam int unsigned MEM_FIXED_DELAY_INPUT   = 0;
    localparam int unsigned MEM_FIXED_DELAY_OUTPUT  = 0;
    // Master-face backpressure profile "random": the file master sometimes
    // cannot keep up, so R backs up through the tile crossbar into the NMU,
    // which is what makes the RoB fill and the DAT dependency question
    // reachable at all.
    localparam bit          MST_STALL_RANDOM_OUTPUT = 1'b1;
    localparam int unsigned MST_FIXED_DELAY_OUTPUT  = 0;

    noc_tb_top #(
        .X_DIM(X_DIM), .Y_DIM(Y_DIM),
        .NUM_ENDPOINTS(NUM_ENDPOINTS),
        .TILE_TARGETS(TILE_TARGETS),
        .TILE_BASE_ADDR(TILE_BASE_ADDR),
        .TILE_SIZE(TILE_SIZE),
        .NOC_EGRESS_BASE(NOC_EGRESS_BASE),
        .REGION_BYTES(REGION_BYTES),
        .N_PERIPH(N_PERIPH),
        .PERIPH_NODE(PERIPH_NODE),
        .PERIPH_PORT(PERIPH_PORT),
        .MEM_STALL_RANDOM_INPUT(MEM_STALL_RANDOM_INPUT),
        .MEM_STALL_RANDOM_OUTPUT(MEM_STALL_RANDOM_OUTPUT),
        .MEM_FIXED_DELAY_INPUT(MEM_FIXED_DELAY_INPUT),
        .MEM_FIXED_DELAY_OUTPUT(MEM_FIXED_DELAY_OUTPUT),
        .MST_STALL_RANDOM_OUTPUT(MST_STALL_RANDOM_OUTPUT),
        .MST_FIXED_DELAY_OUTPUT(MST_FIXED_DELAY_OUTPUT)
    ) u_tb ();

endmodule
