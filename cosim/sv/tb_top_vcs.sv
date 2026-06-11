// tb_top_vcs — self-clocked wrapper around tb_top for event-driven
// simulators (VCS). Under Verilator, clk_i/rst_ni are driven by the C++
// harness (cosim/verilator/main.cpp), which also calls cmodel_finalize()
// after $finish and enforces a cycle timeout; this wrapper reproduces those
// three responsibilities in SV so tb_top itself stays simulator-neutral
// and UNMODIFIED.
//
// Clock: 10 ns period. Reset: deasserted after 4 cycles (mirrors
// main.cpp RESET_CYCLES). Timeout: 100000 cycles (mirrors main.cpp
// TIMEOUT_CYCLES) — $fatal turns a hang into a non-zero exit.
`timescale 1ns/1ps

module tb_top_vcs;
    logic clk_i  = 1'b0;
    logic rst_ni = 1'b0;

    always #5 clk_i = ~clk_i;

    initial begin
        repeat (4) @(posedge clk_i);
        rst_ni = 1'b1;
    end

    localparam int unsigned TIMEOUT_CYCLES = 100000;
    initial begin
        repeat (TIMEOUT_CYCLES) @(posedge clk_i);
        $fatal(1, "tb_top_vcs: timeout after %0d cycles", TIMEOUT_CYCLES);
    end

    // tb_top calls $finish on cmodel_done(); finalize here (Verilator flow
    // does this in main.cpp after the eval loop exits).
    import "DPI-C" context function void cmodel_finalize();
    final begin
        cmodel_finalize();
    end

    tb_top u_tb (
        .clk_i (clk_i),
        .rst_ni(rst_ni)
    );
endmodule
