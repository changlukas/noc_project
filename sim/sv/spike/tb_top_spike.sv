`timescale 1ns/1ps
// SPIKE testbench: mesh_2x1_vc8 struct-port elaboration de-risk.
// Instantiates noc_fabric_spike, checks the one-cycle req→credit handshake.
// PASS: prints "SPIKE PASS: struct-port-in-array elaboration + handshake OK"
// FAIL: $fatal after TIMEOUT_CYCLES.

`ifndef TB_TOP_SPIKE_SV
`define TB_TOP_SPIKE_SV

`include "noc_fabric_spike.sv"

module tb_top_spike;
    localparam int unsigned NUM_NODES      = 2;
    localparam int unsigned TIMEOUT_CYCLES = 50;

    logic clk_i  = 1'b0;
    logic rst_ni = 1'b0;

    always #5 clk_i = ~clk_i;

    initial begin
        repeat (4) @(posedge clk_i);
        rst_ni = 1'b1;
    end

    // Timeout watchdog: separate initial block.
    initial begin
        repeat (TIMEOUT_CYCLES) @(posedge clk_i);
        $fatal(1, "SPIKE FAIL: timeout after %0d cycles", TIMEOUT_CYCLES);
    end

    // Struct-typed unpacked arrays driven by the fabric.
    ni_signals_pkg::noc_chan_t    node_req  [NUM_NODES];
    ni_signals_pkg::noc_credit_t node_cred [NUM_NODES];
    logic                          cred_seen [NUM_NODES];

    // DUT: noc_fabric_spike with struct array ports.
    noc_fabric_spike #(
        .NUM_NODES(NUM_NODES)
    ) u_fabric (
        .clk_i     (clk_i),
        .rst_ni    (rst_ni),
        .node_req  (node_req),
        .node_cred (node_cred),
        .cred_seen (cred_seen)
    );

    // Handshake check: wait for node0 to see the credit.
    initial begin
        @(posedge rst_ni);
        @(posedge clk_i iff cred_seen[0] === 1'b1);
        $display("SPIKE PASS: struct-port-in-array elaboration + handshake OK");
        $display("  node_req[0].valid=%0b flit[7:0]=0x%02h",
                 node_req[0].valid, node_req[0].flit[7:0]);
        $display("  node_cred[0].credit=%0b", node_cred[0].credit);
        $finish;
    end

endmodule

`endif // TB_TOP_SPIKE_SV
