// Passive inter-router link monitor. Counts flits (valid high) and credit-deficit
// stall cycles. Credit is a single-cycle pulse; valid is gated on credit upstream,
// so backpressure is observable only as credit_count==0 (downstream buffer full).

`ifndef LINK_PERF_MONITOR_SV
`define LINK_PERF_MONITOR_SV

module link_perf_monitor #(
    parameter string LINK_NAME = "link",
    parameter int    BUFFER_DEPTH = 4
) (
    input logic clk_i,
    input logic rst_ni,
    input logic valid,         // a flit is on the wire this cycle
    input logic credit_pulse   // a downstream slot freed this cycle
);
    import "DPI-C" context function void cmodel_perf_link(
        input string name, input longint flit_count, input longint stall_cyc);

    longint      flit_count, stall_cyc;
    int unsigned credit;

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            flit_count <= 0; stall_cyc <= 0; credit <= BUFFER_DEPTH;
        end else begin
            // next-value (pre-NBA) so the live push is not one cycle short.
            automatic longint next_flit  = flit_count + (valid ? 1 : 0);
            automatic longint next_stall = stall_cyc  + ((credit == 0) ? 1 : 0);
            flit_count <= next_flit;
            credit     <= credit - (valid ? 1 : 0) + (credit_pulse ? 1 : 0);
            stall_cyc  <= next_stall;
            // live push (set/last-write-wins): final cycle's call carries the total.
            cmodel_perf_link(LINK_NAME, next_flit, next_stall);
        end
    end

    // Credit must never be consumed below zero: valid is gated on credit
    // upstream, so valid && credit==0 means a mis-wire or DUT bug, not a
    // real flit. Assert loudly rather than silently mis-count stall_cyc.
    assert property (@(posedge clk_i) disable iff (!rst_ni)
        !(valid && credit == 0))
        else $error("[%s] credit underflow: valid asserted with zero credit", LINK_NAME);
endmodule

`endif  // LINK_PERF_MONITOR_SV
