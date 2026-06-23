// tb_top_vcs — self-clocked wrapper around tb_top for event-driven
// simulators (VCS). Under Verilator, clk_i/rst_ni are driven by the C++
// harness (sim/verilator/main.cpp), which also calls cmodel_finalize()
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

    // Perf instrumentation — mirrors sim/verilator/main.cpp. The SV perf
    // monitors feed the c_model perf collector: per-transaction data during the
    // run, aggregate counters (backpressure / link) in their own `final` blocks.
    // We sample router occupancy once per rising edge and dump perf.json at the
    // end. ORDERING NOTE: SystemVerilog does not define `final`-block order
    // across modules, so this top-level dump may run before the monitors'
    // `final` pushes; per-transaction throughput is always present, the
    // backpressure/link aggregates may be missing if VCS runs this final first.
    import "DPI-C" context function void cmodel_perf_sample_tick();
    import "DPI-C" context function void cmodel_perf_set_run(input string scenario,
                                                            input longint total_cyc);
    import "DPI-C" context function void cmodel_perf_dump(input string path);

    string        perf_out_path = "perf.json";
    string        perf_scn      = "";
    int unsigned  perf_cycle    = 0;
    initial begin
        void'($value$plusargs("perf_out=%s", perf_out_path));
        void'($value$plusargs("perf_scenario=%s", perf_scn));
    end
    // main.cpp samples every rising edge, ungated by reset.
    always @(posedge clk_i) begin
        cmodel_perf_sample_tick();
        perf_cycle = perf_cycle + 1;
    end

    // tb_top calls $finish on cmodel_done(); finalize here (Verilator flow
    // does this in main.cpp after the eval loop exits). Dump perf before
    // finalize so g_perf is intact.
    import "DPI-C" context function void cmodel_finalize();
    final begin
        cmodel_perf_set_run(perf_scn, longint'(perf_cycle));
        cmodel_perf_dump(perf_out_path);
        cmodel_finalize();
    end

    tb_top u_tb (
        .clk_i (clk_i),
        .rst_ni(rst_ni)
    );

    // FSDB waveform dump — compiled in only by the VCS flow with FSDB=1
    // (+define+FSDB_DUMP). Path comes from +fsdb=<abs-path>; the Makefile
    // run recipe supplies output/<scenario>/tb_top.fsdb.
`ifdef FSDB_DUMP
    initial begin
        string fsdb_path;
        if (!$value$plusargs("fsdb=%s", fsdb_path))
            fsdb_path = "dump.fsdb";
        $fsdbDumpfile(fsdb_path);
        $fsdbDumpvars(0, tb_top_vcs); // depth 0 = full hierarchy below top
    end
`endif
endmodule
