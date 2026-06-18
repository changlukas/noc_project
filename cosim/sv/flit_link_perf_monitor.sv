// Passive inter-router link monitor. Counts flits (valid high) and credit-deficit
// stall cycles. Credit is a single-cycle pulse; valid is gated on credit upstream,
// so backpressure is observable only as credit_count==0 (downstream buffer full).
module flit_link_perf_monitor #(
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

    longint flit_count, stall_cyc;
    int     credit;

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            flit_count <= 0; stall_cyc <= 0; credit <= BUFFER_DEPTH;
        end else begin
            // credit accounting: -1 on flit sent, +1 on pulse (net 0 if both).
            if (valid) flit_count <= flit_count + 1;
            credit <= credit - (valid ? 1 : 0) + (credit_pulse ? 1 : 0);
            if (credit == 0) stall_cyc <= stall_cyc + 1;  // downstream buffer full
        end
    end

    final cmodel_perf_link(LINK_NAME, flit_count, stall_cyc);
endmodule
